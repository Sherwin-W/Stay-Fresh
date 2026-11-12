/** A row of public.fridge_readings as returned by PostgREST.
 *  Original columns are always present; the rest only exist after
 *  supabase/migrations/001_additive_telemetry.sql, so they are optional. */
export interface RawRow {
  id: string;
  received_at: string;
  device_id: string;
  uptime_ms: number | string; // bigint may arrive as string
  temperature_c: number | null;
  angle_deg: number;
  door_open: boolean;
  alarm_active: boolean;
  open_duration_ms: number | string;
  // --- optional (migration 001) ---
  boot_id?: string | null;
  seq?: number | string | null;
  firmware_version?: string | null;
  measured_at?: string | null;
  door_state?: string | null;
  closure_source?: string | null;
  calibration_state?: string | null;
  angle_quality?: string | null;
  sensor_valid?: boolean | null;
  since_reference_ms?: number | string | null;
  missed_samples?: number | string | null;
  bias_adjustments?: number | string | null;
  reed_enabled?: boolean | null;
  reed_closed?: boolean | null;
}

export type DoorState = "closed" | "open" | "unknown";

/** Normalised reading used by the UI. `null` always means "not reported". */
export interface Reading {
  id: string;
  receivedAt: number; // ms epoch, SERVER arrival time
  measuredAt: number | null; // ms epoch from device clock, if the firmware sent it
  deviceId: string;
  uptimeMs: number;
  bootId: string | null;
  temperatureC: number | null;
  angleDeg: number;
  doorState: DoorState;
  /** true when door_state came from the old boolean column (no 'unknown' possible). */
  legacyDoorState: boolean;
  alarmActive: boolean;
  openDurationMs: number;
  closureSource: string | null;
  calibrationState: string | null;
  angleQuality: string | null;
  sensorValid: boolean | null;
  sinceReferenceMs: number | null;
  missedSamples: number | null;
  biasAdjustments: number | null;
  reedEnabled: boolean | null;
  reedClosed: boolean | null;
  firmwareVersion: string | null;
  /** True when this extended telemetry is present (migration applied + firmware flag on). */
  extended: boolean;
}

export type Freshness = "live" | "delayed" | "stale" | "none";

export interface TimeRange {
  key: string;
  label: string;
  ms: number;
}
