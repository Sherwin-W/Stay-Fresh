import { describe, expect, it } from "vitest";
import {
  GAP_BREAK_MS, buildSpans, classifyFreshness, formatAge, formatNumber, mergeReadings, normalizeRow, splitSegments,
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
