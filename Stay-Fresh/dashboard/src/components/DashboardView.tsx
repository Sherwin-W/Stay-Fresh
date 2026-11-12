import { useMemo } from "react";
import {
  RANGES, ageMs, classifyFreshness, describeAngleQuality, describeClosureSource,
  formatAge, formatDuration, formatNumber, type Point,
} from "../lib/derive";
import type { Reading } from "../lib/types";
import type { ReadingsState } from "../hooks/useReadings";
import { useNow } from "../hooks/useNow";
import { Fridge } from "./Fridge";
import { LineChart } from "./LineChart";
import { StateTimeline } from "./StateTimeline";

interface Props {
  deviceId: string;
  state: ReadingsState;
  rangeKey: string;
  onRange: (key: string) => void;
  demo: boolean;
  userEmail?: string;
  onSignOut?: () => void;
  onExitDemo?: () => void;
}

const DOOR_LABEL = { closed: "Closed", open: "Open", unknown: "Unknown" } as const;

export function DashboardView({ deviceId, state, rangeKey, onRange, demo, userEmail, onSignOut, onExitDemo }: Props) {
  const now = useNow(1000);
  const range = RANGES.find((r) => r.key === rangeKey) ?? RANGES[1];
  const { rows } = state;
  const latest: Reading | undefined = rows[rows.length - 1];
  const age = ageMs(latest, now);
  const fresh = classifyFreshness(age);
  const stale = fresh === "stale";
  const asOf = latest ? `as of last report, ${formatAge(age)}` : "";
  const domain: [number, number] = [now - range.ms, now];

  const tempPoints: Point[] = useMemo(() => rows.map((r) => ({ t: r.receivedAt, v: r.temperatureC })), [rows]);
  // Angle is only meaningful when the door state is known.
  const anglePoints: Point[] = useMemo(
    () => rows.map((r) => ({ t: r.receivedAt, v: r.doorState === "unknown" || r.angleQuality === "none" ? null : r.angleDeg })),
    [rows],
  );

  const alarmNow = !!latest?.alarmActive;
  const showAlarmLive = alarmNow && !stale;

  return (
    <div className="page">
      {demo && (
        <div className="demo-banner" role="status">
          <strong>DEMO MODE</strong> Synthetic data generated in your browser. Nothing here comes from your fridge or the database.
          <button className="link" onClick={onExitDemo}>Exit demo</button>
        </div>
      )}

      <header className="topbar">
        <div>
          <h1>Stay-Fresh</h1>
          <p className="muted">Fridge-door monitor · device <code>{deviceId}</code></p>
        </div>
        <div className="topbar-right">
          <span className={`pill ${demo ? "demo" : fresh}`} data-testid="freshness-pill">
            <i className="dot-ind" aria-hidden="true" />
            {demo ? "Demo" : fresh === "live" ? "Live" : fresh === "delayed" ? "Delayed" : fresh === "stale" ? "Stale" : "No data"}
          </span>
          {userEmail && <span className="muted small">{userEmail}</span>}
          {onSignOut && <button className="ghost" onClick={onSignOut}>Sign out</button>}
        </div>
      </header>

      {showAlarmLive && (
        <div className="alarm-banner" role="alert">
          <span aria-hidden="true">⚠</span>
          <div>
            <strong>Alarm active: door has been open past the limit.</strong>
            <div className="small">{asOf}. Close the door; the alarm clears when closure is detected.</div>
          </div>
        </div>
      )}
      {alarmNow && stale && (
        <div className="stale-banner" role="status">
          The alarm was active in the last report ({formatAge(age)}). Its current state is unknown.
        </div>
      )}
      {stale && !alarmNow && (
        <div className="stale-banner" role="status" data-testid="stale-banner">
          No new readings for {formatAge(age).replace(" ago", "")}. Values below are the <strong>last known</strong>, not live.
          This does not say why: the device may be off, out of range of Wi-Fi, or unable to reach the database.
        </div>
      )}
      {fresh === "delayed" && (
        <div className="info-banner" role="status">Data is arriving later than usual (last report {formatAge(age)}).</div>
      )}
      {state.status === "error" && (
        <div className="error-banner" role="alert" data-testid="fetch-error">
          Couldn’t load from the database: {state.error}. Showing {rows.length ? "data fetched earlier" : "nothing yet"}; retrying every 5 s
          {state.lastOkAt ? ` (last success ${formatAge(now - state.lastOkAt)})` : ""}.
        </div>
      )}

      <section className="hero glass" aria-label="Current status">
        <div className="hero-fridge">
          <Fridge
            angleDeg={latest && latest.doorState !== "unknown" && latest.angleQuality !== "none" ? latest.angleDeg : null}
            state={latest ? latest.doorState : null}
            alarm={showAlarmLive}
            stale={stale}
          />
        </div>

        <div className={`cards${stale ? " is-stale" : ""}`}>
          <article className="card">
            <h2>Sensor temperature</h2>
            {latest && latest.temperatureC !== null ? (
              <p className="big" data-testid="temp-value">{formatNumber(latest.temperatureC)}<small>°C</small></p>
            ) : (
              <p className="big missing" data-testid="temp-missing">{latest ? "No reading" : "—"}</p>
            )}
            <p className="small muted">
              {latest && latest.temperatureC === null
                ? "The BMP280 did not report a value in the last snapshot."
                : "Air temperature where the sensor is mounted. On the outside of the door this is NOT the fridge interior."}
              {stale && latest?.temperatureC !== null ? " Last known." : ""}
            </p>
          </article>

          <article className="card">
            <h2>Door</h2>
            {latest ? (
              <>
                <p className={`big door-${latest.doorState}`}>{DOOR_LABEL[latest.doorState]}</p>
                <p className="small muted">
                  {latest.angleQuality === "none" || latest.doorState === "unknown"
                    ? "No angle estimate available."
                    : <>Approx. angle <strong>{formatNumber(latest.angleDeg, 0)}°</strong> (gyro estimate, drifts)</>}
                </p>
                <p className="small muted">
                  {latest.legacyDoorState
                    ? "Older telemetry format: cannot report “unknown” or how closure was determined."
                    : `Closure: ${describeClosureSource(latest.closureSource)}`}
                </p>
              </>
            ) : <p className="big missing">—</p>}
          </article>

          <article className="card">
            <h2>Alarm</h2>
            {latest ? (
              <p className={`big ${latest.alarmActive ? "alarm-on" : ""}`}>
                {latest.alarmActive ? (stale ? "Was active" : "ACTIVE") : "Off"}
              </p>
            ) : <p className="big missing">—</p>}
            <p className="small muted">{latest ? `Reported ${formatAge(age)}` : "No data"}</p>
          </article>

          <article className="card">
            <h2>Open duration</h2>
            {latest ? (
              latest.doorState === "open" ? (
                <>
                  <p className="big">{formatDuration(latest.openDurationMs)}</p>
                  <p className="small muted">{asOf}. Not extrapolated; it can be several seconds behind.</p>
                </>
              ) : latest.doorState === "closed" ? (
                <><p className="big">Not open</p><p className="small muted">{asOf}</p></>
              ) : (
                <><p className="big missing">Unknown</p><p className="small muted">Door state unknown.</p></>
              )
            ) : <p className="big missing">—</p>}
          </article>

          <article className="card wide">
            <h2>Last received</h2>
            {latest ? (
              <>
                <p className="big small-big" data-testid="age">{formatAge(age)}</p>
                <p className="small muted">
                  {new Date(latest.receivedAt).toLocaleString()} <span className="fine">(server arrival time{latest.measuredAt ? `; device clock said ${new Date(latest.measuredAt).toLocaleTimeString()}` : ""})</span>
                </p>
              </>
            ) : <p className="big missing" data-testid="no-data">No readings yet</p>}
          </article>
        </div>
      </section>

      <div className="range-bar" role="group" aria-label="Time range">
        <span className="muted small">Range</span>
        {RANGES.map((r) => (
          <button key={r.key} className={`chip${r.key === rangeKey ? " on" : ""}`} aria-pressed={r.key === rangeKey} onClick={() => onRange(r.key)}>
            {r.label}
          </button>
        ))}
        {state.status === "loading" && <span className="muted small" role="status">Loading…</span>}
      </div>
      {state.truncated && (
        <p className="info-banner small">Showing the newest {rows.length.toLocaleString()} readings only; older data in this range isn’t loaded.</p>
      )}

      {state.status !== "loading" && rows.length === 0 ? (
        <section className="glass panel empty" data-testid="empty">
          <h2>No readings in this time range</h2>
          <p className="muted">Nothing has been received for device <code>{deviceId}</code> in the last {range.label}. Check that the device is powered, calibrated and uploading; try a longer range.</p>
        </section>
      ) : (
        <div className="grid">
          <section className="glass panel">
            <h2>Temperature</h2>
            <p className="muted small">Sensor-location temperature, one point per 5 s snapshot. Breaks mean no data, not zero.</p>
            <LineChart title="Temperature" unit="°C" points={tempPoints} domain={domain} minSpan={2} empty="No temperature readings in this window." />
          </section>
          <section className="glass panel">
            <h2>Door angle (estimate)</h2>
            <p className="muted small">Gyro-integrated; drifts between closed references. Dashed line = “open” threshold.</p>
            <LineChart title="Door angle" unit="°" digits={0} points={anglePoints} domain={domain} minSpan={20} refLine={{ v: 7, label: "open > 7°" }} empty="No angle estimates in this window." />
          </section>
          <section className="glass panel span2">
            <h2>Door &amp; alarm history</h2>
            <p className="muted small">Built from 5-second snapshots uploaded on a best-effort basis: openings shorter than a few seconds, or while the device was offline, can be missed, so this is not an exact event count.</p>
            <StateTimeline rows={rows} domain={domain} />
          </section>
        </div>
      )}

      {latest && <QualityPanel latest={latest} />}

      {rows.length > 0 && (
        <section className="glass panel">
          <h2>Recent readings</h2>
          <div className="table-wrap">
            <table>
              <thead>
                <tr><th>Received</th><th>Temp °C</th><th>Angle °</th><th>Door</th><th>Alarm</th><th>Open for</th></tr>
              </thead>
              <tbody>
                {rows.slice(-12).reverse().map((r) => (
                  <tr key={r.id}>
                    <td>{new Date(r.receivedAt).toLocaleTimeString()}</td>
                    <td>{r.temperatureC === null ? <span className="missing">no reading</span> : r.temperatureC.toFixed(2)}</td>
                    <td>{r.doorState === "unknown" || r.angleQuality === "none" ? <span className="missing">n/a</span> : r.angleDeg.toFixed(1)}</td>
                    <td>{DOOR_LABEL[r.doorState]}</td>
                    <td>{r.alarmActive ? "ACTIVE" : "off"}</td>
                    <td>{r.doorState === "open" ? formatDuration(r.openDurationMs) : "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </section>
      )}

      <footer className="muted small foot">
        Open/closed is a gyro <em>estimate</em> unless a reed/Hall sensor reports it. Timestamps are when the server received each reading.
        This system does not measure humidity or water level.
      </footer>
    </div>
  );
}

function QualityPanel({ latest }: { latest: Reading }) {
  const yn = (v: boolean | null) => (v === null ? "Not reported" : v ? "Yes" : "No");
  const n = (v: number | null) => (v === null ? "Not reported" : String(v)); // keeps a real 0 as "0"
  return (
    <section className="glass panel" aria-label="Measurement quality">
      <h2>Calibration &amp; measurement quality</h2>
      {!latest.extended && (
        <p className="muted small">This firmware/database is using the original schema, so quality fields aren’t available. Apply <code>supabase/migrations/001_additive_telemetry.sql</code> and enable <code>CLOUD_SEND_EXTENDED</code> to see them.</p>
      )}
      <dl className="quality">
        <div><dt>Calibration</dt><dd>{latest.calibrationState ?? "Not reported"}</dd></div>
        <div><dt>Gyro sensor valid</dt><dd>{yn(latest.sensorValid)}</dd></div>
        <div><dt>Angle quality</dt><dd>{describeAngleQuality(latest.angleQuality)}</dd></div>
        <div><dt>Since last zero reference</dt><dd>{latest.sinceReferenceMs === null ? "Not reported" : formatDuration(latest.sinceReferenceMs)}</dd></div>
        <div><dt>Missed gyro samples (since boot)</dt><dd>{n(latest.missedSamples)}</dd></div>
        <div><dt>Bias refinements (since boot)</dt><dd>{n(latest.biasAdjustments)}</dd></div>
        <div><dt>Reed/Hall sensor</dt><dd>{latest.reedEnabled === null ? "Not reported" : latest.reedEnabled ? `Enabled (${latest.reedClosed === null ? "state unknown" : latest.reedClosed ? "contact closed" : "contact open"})` : "Not installed"}</dd></div>
        <div><dt>Firmware</dt><dd>{latest.firmwareVersion ?? "Not reported"}</dd></div>
      </dl>
    </section>
  );
}
