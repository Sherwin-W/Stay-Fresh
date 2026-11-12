import type { DoorState, Freshness, RawRow, Reading, TimeRange } from "./types";

/** Firmware uploads a snapshot every 5 s (best effort). */
export const EXPECTED_INTERVAL_MS = 5_000;
/** Up to this age we call the feed "live" (3 intervals + upload latency). */
export const LIVE_MAX_AGE_MS = 15_000;
/** Beyond this the displayed values are clearly old. */
export const STALE_AFTER_MS = 60_000;
/** A line is broken where consecutive readings are further apart than this. */
export const GAP_BREAK_MS = 30_000;
/** Page size PostgREST returns by default (Supabase max-rows). */
export const PAGE_SIZE = 1000;
/** Hard cap on rows held in memory. */
export const MAX_ROWS = 6000;

export const RANGES: TimeRange[] = [
  { key: "15m", label: "15 min", ms: 15 * 60_000 },
  { key: "1h", label: "1 hour", ms: 60 * 60_000 },
  { key: "6h", label: "6 hours", ms: 6 * 3_600_000 },
  { key: "24h", label: "24 hours", ms: 24 * 3_600_000 },
];

const num = (v: unknown): number | null => {
  if (v === null || v === undefined || v === "") return null;
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? n : null;
};

const ts = (v: unknown): number | null => {
  if (typeof v !== "string") return null;
  const t = Date.parse(v);
  return Number.isFinite(t) ? t : null;
};

export function normalizeRow(r: RawRow): Reading {
  const extended = r.door_state !== undefined && r.door_state !== null;
  let doorState: DoorState;
  if (extended && (r.door_state === "closed" || r.door_state === "open" || r.door_state === "unknown")) {
    doorState = r.door_state;
  } else {
    // Legacy rows only have a boolean: it cannot express "unknown".
    doorState = r.door_open ? "open" : "closed";
  }
  return {
    id: r.id,
    receivedAt: ts(r.received_at) ?? 0,
    measuredAt: ts(r.measured_at),
    deviceId: r.device_id,
    uptimeMs: num(r.uptime_ms) ?? 0,
    bootId: r.boot_id ?? null,
    temperatureC: num(r.temperature_c), // null stays null; 0 stays 0
    angleDeg: num(r.angle_deg) ?? 0,
    doorState,
    legacyDoorState: !extended,
    alarmActive: !!r.alarm_active,
    openDurationMs: Math.max(0, num(r.open_duration_ms) ?? 0),
    closureSource: r.closure_source ?? null,
    calibrationState: r.calibration_state ?? null,
    angleQuality: r.angle_quality ?? null,
    sensorValid: r.sensor_valid ?? null,
    sinceReferenceMs: num(r.since_reference_ms),
    missedSamples: num(r.missed_samples),
    biasAdjustments: num(r.bias_adjustments),
    reedEnabled: r.reed_enabled ?? null,
    reedClosed: r.reed_closed ?? null,
    firmwareVersion: r.firmware_version ?? null,
    extended,
  };
}

/** Sort ascending by server arrival time and drop duplicate ids. */
export function mergeReadings(existing: Reading[], incoming: Reading[], windowStartMs: number): Reading[] {
  const byId = new Map<string, Reading>();
  for (const r of existing) byId.set(r.id, r);
  for (const r of incoming) byId.set(r.id, r);
  const all = [...byId.values()].filter((r) => r.receivedAt >= windowStartMs);
  all.sort((a, b) => a.receivedAt - b.receivedAt || a.uptimeMs - b.uptimeMs);
  return all.length > MAX_ROWS ? all.slice(all.length - MAX_ROWS) : all;
}

export function ageMs(latest: Reading | undefined, nowMs: number): number | null {
  if (!latest) return null;
  // Clock skew can make a fresh row look slightly "from the future".
  return Math.max(0, nowMs - latest.receivedAt);
}

export function classifyFreshness(age: number | null): Freshness {
  if (age === null) return "none";
  if (age <= LIVE_MAX_AGE_MS) return "live";
  if (age <= STALE_AFTER_MS) return "delayed";
  return "stale";
}

export function formatAge(ms: number | null): string {
  if (ms === null) return "—";
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s ago`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h}h ${m % 60}m ago`;
  return `${Math.floor(h / 24)}d ${h % 24}h ago`;
}

export function formatDuration(ms: number): string {
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  return `${Math.floor(s / 60)}m ${s % 60}s`;
}

/** Number formatting that keeps "no value" distinct from a real zero. */
export function formatNumber(v: number | null | undefined, digits = 1): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "—";
  return v.toFixed(digits);
}

export interface Point {
  t: number;
  v: number | null;
}

/** Split into continuous runs. A run ends at a missing value or a time gap. */
export function splitSegments(points: Point[], gapMs = GAP_BREAK_MS): Point[][] {
  const out: Point[][] = [];
  let cur: Point[] = [];
  let prevT: number | null = null;
  for (const p of points) {
    const gap = prevT !== null && p.t - prevT > gapMs;
    if (p.v === null || gap) {
      if (cur.length) out.push(cur);
      cur = [];
    }
    if (p.v !== null) cur.push(p);
    prevT = p.t;
  }
  if (cur.length) out.push(cur);
  return out;
}

export interface Span {
  start: number;
  end: number;
  kind: DoorState | "nodata";
  alarm: boolean;
}

/**
 * Timeline of door state built from snapshots. Each snapshot is assumed to
 * hold until the next one, but never for longer than GAP_BREAK_MS: beyond that
 * we show "no data" instead of pretending we know. Events shorter than the
 * 5 s sampling interval can be missed entirely.
 */
export function buildSpans(rows: Reading[], gapMs = GAP_BREAK_MS): Span[] {
  const spans: Span[] = [];
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    const next = rows[i + 1];
    const holdUntil = next ? Math.min(next.receivedAt, r.receivedAt + gapMs) : r.receivedAt + EXPECTED_INTERVAL_MS;
    const prev = spans[spans.length - 1];
    if (prev && prev.end < r.receivedAt - 1) {
      spans.push({ start: prev.end, end: r.receivedAt, kind: "nodata", alarm: false });
    }
    const last = spans[spans.length - 1];
    if (last && last.kind === r.doorState && last.alarm === r.alarmActive && Math.abs(last.end - r.receivedAt) <= 1) {
      last.end = holdUntil;
    } else {
      spans.push({ start: r.receivedAt, end: holdUntil, kind: r.doorState, alarm: r.alarmActive });
    }
  }
  return spans;
}

/** Plain-language quality labels for the calibration panel. */
export function describeAngleQuality(q: string | null): string {
  switch (q) {
    case "estimated": return "Estimated (gyro integration; drifts over time)";
    case "degraded": return "Degraded (missed samples or sensor loss since last reference)";
    case "none": return "No angle (not calibrated)";
    default: return "Not reported";
  }
}

export function describeClosureSource(s: string | null): string {
  switch (s) {
    case "estimated": return "Gyro estimate only";
    case "manual": return "Button press (user asserted closed)";
    case "reed": return "Reed/Hall contact (independently confirmed)";
    case "none": return "Not closed / unknown";
    default: return "Not reported";
  }
}
