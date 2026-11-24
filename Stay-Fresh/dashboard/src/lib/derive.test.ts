import { describe, expect, it } from "vitest";
import {
  GAP_BREAK_MS, buildSpans, evaluateTempAlerts, classifyFreshness, formatAge, formatNumber, mergeReadings, normalizeRow, splitSegments,
} from "./derive";
import type { RawRow } from "./types";

const base = (o: Partial<RawRow> = {}): RawRow => ({
  id: "1", received_at: "2026-01-01T00:00:10Z", device_id: "fridge-05", uptime_ms: "5000",
  temperature_c: 4.5, angle_deg: 0, door_open: false, alarm_active: false, open_duration_ms: 0, ...o,
});

describe("normalizeRow", () => {
  it("keeps a real 0 °C distinct from a missing temperature", () => {
    expect(normalizeRow(base({ temperature_c: 0 })).temperatureC).toBe(0);
    expect(normalizeRow(base({ temperature_c: null })).temperatureC).toBeNull();
    expect(formatNumber(0)).toBe("0.0");
    expect(formatNumber(null)).toBe("—");
  });
  it("derives door state from the legacy boolean and flags it", () => {
    const r = normalizeRow(base({ door_open: true }));
    expect(r.doorState).toBe("open");
    expect(r.legacyDoorState).toBe(true);
    expect(r.extended).toBe(false);
    expect(r.closureSource).toBeNull(); // not reported != "none"
  });
  it("uses door_state when the migration columns exist, including 'unknown'", () => {
    const r = normalizeRow(base({ door_state: "unknown", door_open: false, closure_source: "none", missed_samples: 0 }));
    expect(r.doorState).toBe("unknown");
    expect(r.legacyDoorState).toBe(false);
    expect(r.missedSamples).toBe(0); // a real zero, not "missing"
  });
  it("parses bigint strings", () => {
    expect(normalizeRow(base({ open_duration_ms: "12000" })).openDurationMs).toBe(12000);
  });
});

describe("freshness", () => {
  it("classifies by age", () => {
    expect(classifyFreshness(null)).toBe("none");
    expect(classifyFreshness(4_000)).toBe("live");
    expect(classifyFreshness(30_000)).toBe("delayed");
    expect(classifyFreshness(61_000)).toBe("stale");
  });
  it("formats ages", () => {
    expect(formatAge(12_000)).toBe("12s ago");
    expect(formatAge(125_000)).toBe("2m 5s ago");
    expect(formatAge(null)).toBe("—");
  });
});

describe("splitSegments", () => {
  it("breaks at nulls and at time gaps instead of interpolating", () => {
    const pts = [
      { t: 0, v: 1 }, { t: 5000, v: 2 }, { t: 10000, v: null }, { t: 15000, v: 3 },
      { t: 15000 + GAP_BREAK_MS + 1, v: 4 },
    ];
    const segs = splitSegments(pts);
    expect(segs.map((s) => s.length)).toEqual([2, 1, 1]);
  });
});

describe("mergeReadings", () => {
  it("dedupes, sorts, and trims to the window", () => {
    const a = normalizeRow(base({ id: "a", received_at: "2026-01-01T00:00:05Z" }));
    const b = normalizeRow(base({ id: "b", received_at: "2026-01-01T00:00:10Z" }));
    const old = normalizeRow(base({ id: "o", received_at: "2025-12-31T00:00:00Z" }));
    const out = mergeReadings([b, old], [a, b], Date.parse("2026-01-01T00:00:00Z"));
    expect(out.map((r) => r.id)).toEqual(["a", "b"]);
  });
});

describe("buildSpans", () => {
  const mk = (id: string, sec: number, o: Partial<RawRow> = {}) =>
    normalizeRow(base({ id, received_at: new Date(Date.UTC(2026, 0, 1, 0, 0, sec)).toISOString(), ...o }));
  it("merges equal consecutive states and shows a gap as no-data", () => {
    const rows = [mk("1", 0), mk("2", 5), mk("3", 10, { door_open: true }), mk("4", 120)];
    const spans = buildSpans(rows);
    expect(spans.map((s) => s.kind)).toEqual(["closed", "open", "nodata", "closed"]);
  });
  it("marks alarm periods separately", () => {
    const rows = [mk("1", 0, { door_open: true }), mk("2", 5, { door_open: true, alarm_active: true })];
    expect(buildSpans(rows).some((s) => s.alarm)).toBe(true);
  });
});

describe("evaluateTempAlerts", () => {
  const MIN = 60_000;
  const T0 = Date.parse("2026-01-01T00:00:00Z");
  const series = (fn: (min: number) => number | null, minutes: number, stepS = 10) => {
    const rows = [];
    for (let s = 0; s <= minutes * 60; s += stepS) {
      const t = T0 + s * 1000;
      rows.push(normalizeRow(base({ id: String(s), received_at: new Date(t).toISOString(), temperature_c: fn(s / 60) })));
    }
    return { rows, now: T0 + minutes * MIN };
  };

  it("is quiet for a steady, safe temperature", () => {
    const { rows, now } = series(() => 3.2, 90);
    expect(evaluateTempAlerts(rows, now)).toEqual({ rising: null, hot: null });
  });
  it("flags a steady climb", () => {
    const { rows, now } = series((m) => 3 + 0.2 * m, 20); // 0.2 °C/min => +2 °C per 10 min
    const a = evaluateTempAlerts(rows, now);
    expect(a.rising).not.toBeNull();
    expect(a.rising!.riseC).toBeGreaterThan(1.9);
  });
  it("does not flag a slow drift or a falling temperature", () => {
    const slow = series((m) => 3 + 0.02 * m, 20);
    expect(evaluateTempAlerts(slow.rows, slow.now).rising).toBeNull();
    const fall = series((m) => 6 - 0.2 * m, 20);
    expect(evaluateTempAlerts(fall.rows, fall.now).rising).toBeNull();
  });
  it("needs enough history before judging a trend", () => {
    const { rows, now } = series((m) => 3 + 0.5 * m, 3);
    expect(evaluateTempAlerts(rows, now).rising).toBeNull();
  });
  it("flags more than an hour above 4 °C, but not 59 minutes", () => {
    const a = series(() => 5, 61);
    expect(evaluateTempAlerts(a.rows, a.now).hot).not.toBeNull();
    const b = series(() => 5, 59);
    expect(evaluateTempAlerts(b.rows, b.now).hot).toBeNull();
  });
  it("restarts the hour when it dipped to 4 °C or below", () => {
    const { rows, now } = series((m) => (m >= 25 && m < 26 ? 3.9 : 5), 80);
    expect(evaluateTempAlerts(rows, now).hot).toBeNull(); // only ~54 min since the dip
  });
  it("does not bridge a long data gap", () => {
    const rows = series(() => 5, 90).rows.filter((r) => r.receivedAt < T0 + 20 * MIN || r.receivedAt > T0 + 40 * MIN);
    expect(evaluateTempAlerts(rows, T0 + 90 * MIN).hot).toBeNull();
  });
  it("ignores missing temperatures and stale feeds", () => {
    const { rows, now } = series((m) => (m % 2 < 1 ? null : 5), 70);
    expect(evaluateTempAlerts(rows, now).hot).not.toBeNull();
    expect(evaluateTempAlerts(rows, now + 5 * MIN)).toEqual({ rising: null, hot: null });
  });
});
