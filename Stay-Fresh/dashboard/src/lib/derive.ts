import type { DoorState, Freshness, RawRow, Reading, TimeRange } from "./types";

/** Firmware uploads every 5 s while the door is closed (best effort) and
 *  every 1 s while it is open or the alarm is on, plus once per door/alarm change. */
export const EXPECTED_INTERVAL_MS = 5_000;
/** Up to this age we call the feed "live" (3 intervals + upload latency). */
export const LIVE_MAX_AGE_MS = 15_000;
/** Beyond this the displayed values are clearly old. */
export const STALE_AFTER_MS = 60_000;
/** A line is broken where consecutive readings are further apart than this. */
export const GAP_BREAK_MS = 30_000;
/** Page size PostgREST returns by default (Supabase max-rows). */
export const PAGE_SIZE = 1000;
/** Hard cap on rows held in memory. Must fit the longest range (72 h): at one upload
 *  per 5 s that is 51,840 rows, plus extra 1 s uploads while the door is open. */
export const MAX_ROWS = 120_000;

export const RANGES: TimeRange[] = [
  { key: "15m", label: "15 min", ms: 15 * 60_000 },
  { key: "1h", label: "1 hour", ms: 60 * 60_000 },
  { key: "6h", label: "6 hours", ms: 6 * 3_600_000 },
  { key: "24h", label: "24 hours", ms: 24 * 3_600_000 },
  { key: "72h", label: "72 hours", ms: 72 * 3_600_000 },
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

// ---------------------------------------------------------------------------
// Temperature alerts
// ---------------------------------------------------------------------------

/** Food-safety limit for a refrigerator. */
export const TEMP_SAFE_MAX_C = 4;
/** Alert when the temperature has stayed above the limit this long. */
export const TEMP_SAFE_MAX_MS = 60 * 60_000;
/** The "above limit" run is broken by a hole in the data longer than this. */
export const TEMP_RUN_GAP_MS = 2 * 60_000;
/** Trend window for the "rising" alert. */
export const RISE_WINDOW_MS = 10 * 60_000;
/** Alert when the fitted trend climbs at least this much over the window (°C). */
export const RISE_MIN_C = 1.0;
/** Need this much of the window actually covered by data before judging a trend. */
export const RISE_MIN_SPAN_MS = 5 * 60_000;
export const RISE_MIN_POINTS = 8;
/** The dashboard must hold at least this much history for the alerts to work. */
export const ALERT_WINDOW_MS = TEMP_SAFE_MAX_MS + 5 * 60_000;

export interface RisingAlert { riseC: number; ratePerMin: number; windowMs: number; fromC: number; toC: number }
export interface HotAlert { sinceMs: number; durationMs: number; currentC: number }
export interface TempAlerts { rising: RisingAlert | null; hot: HotAlert | null }

/**
 * Evaluates both temperature alerts from the readings held in memory.
 * Returns no alerts when the newest reading is stale: an old value must not
 * raise (or clear) a "now" alert. A missing temperature is skipped, never
 * treated as 0.
 */
export function evaluateTempAlerts(rows: Reading[], nowMs: number): TempAlerts {
  const none: TempAlerts = { rising: null, hot: null };
  const temps = rows.filter((r) => r.temperatureC !== null);
  const latest = temps[temps.length - 1];
  if (!latest || nowMs - latest.receivedAt > STALE_AFTER_MS) return none;
  const cur = latest.temperatureC as number;

  // 1) Continuously above the safe limit for an hour.
  let hot: HotAlert | null = null;
  if (cur > TEMP_SAFE_MAX_C) {
    let start = latest.receivedAt;
    for (let i = temps.length - 2; i >= 0; i--) {
      const r = temps[i];
      if ((r.temperatureC as number) <= TEMP_SAFE_MAX_C) break;
      if (start - r.receivedAt > TEMP_RUN_GAP_MS) break;
      start = r.receivedAt;
    }
    const durationMs = latest.receivedAt - start;
    if (durationMs >= TEMP_SAFE_MAX_MS) hot = { sinceMs: start, durationMs, currentC: cur };
  }

  // 2) Rising: least-squares slope over the last RISE_WINDOW_MS.
  let rising: RisingAlert | null = null;
  const w = temps.filter((r) => r.receivedAt >= latest.receivedAt - RISE_WINDOW_MS);
  if (w.length >= RISE_MIN_POINTS && w[w.length - 1].receivedAt - w[0].receivedAt >= RISE_MIN_SPAN_MS) {
    const t0 = w[0].receivedAt;
    const n = w.length;
    let sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const r of w) {
      const x = (r.receivedAt - t0) / 60_000; // minutes
      const y = r.temperatureC as number;
      sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    const denom = n * sxx - sx * sx;
    if (denom > 0) {
      const slope = (n * sxy - sx * sy) / denom; // °C per minute
      const riseC = slope * (RISE_WINDOW_MS / 60_000);
      if (riseC >= RISE_MIN_C) {
        rising = { riseC, ratePerMin: slope, windowMs: RISE_WINDOW_MS, fromC: w[0].temperatureC as number, toC: cur };
      }
    }
  }
  return { rising, hot };
}
