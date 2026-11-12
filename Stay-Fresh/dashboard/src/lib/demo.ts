import type { Reading } from "./types";

/**
 * DEMO DATA. Synthetic, deterministic function of time. Used only when the
 * user explicitly switches demo mode on; it never touches Supabase and never
 * mixes with live data. Cycle: 10 min long, door open 3m20s-3m50s (alarm after 10 s).
 */
const CYCLE_MS = 600_000;

export function makeDemoReadings(nowMs: number, windowMs: number): Reading[] {
  const out: Reading[] = [];
  const first = Math.ceil((nowMs - windowMs) / 5000) * 5000;
  for (let t = first; t <= nowMs; t += 5000) {
    const c = ((t % CYCLE_MS) + CYCLE_MS) % CYCLE_MS;
    const openFor = c - 200_000;
    const isOpen = openFor >= 0 && openFor < 30_000;
    const ramp = Math.min(1, Math.max(0, openFor / 3000));
    const angle = isOpen ? 68 * ramp : 0;
    const temp = 4.8 + 0.6 * Math.sin(t / 60_000) + (isOpen ? 0.4 : 0);
    out.push({
      id: `demo-${t}`,
      receivedAt: t,
      measuredAt: t,
      deviceId: "demo-fridge",
      uptimeMs: t % 1e9,
      bootId: "demo",
      temperatureC: temp,
      angleDeg: angle,
      doorState: isOpen ? "open" : "closed",
      legacyDoorState: false,
      alarmActive: isOpen && openFor >= 10_000,
      openDurationMs: isOpen ? openFor : 0,
      closureSource: isOpen ? "none" : "estimated",
      calibrationState: "calibrated",
      angleQuality: "estimated",
      sensorValid: true,
      sinceReferenceMs: 40_000,
      missedSamples: 0,
      biasAdjustments: 3,
      reedEnabled: false,
      reedClosed: null,
      firmwareVersion: "demo",
      extended: true,
    });
  }
  return out;
}
