# Stay-Fresh: ESP32 fridge-door monitor (UCI CS244P)

An ESP32 on a refrigerator door reads temperature every 5 s, estimates door rotation with an MPU6050 gyro, sounds a buzzer if the door stays open too long, and uploads snapshots to Supabase. A localhost React dashboard shows them.

```
include/config.h         all pins, thresholds, feature flags (edit alarm time here)
include/door_logic.h     ALL decision logic, no hardware calls (unit tested on a laptop)
src/main.cpp             hardware glue: I2C, button, buzzer, serial, snapshots
include/cloud_upload.h   FreeRTOS network task, Supabase auth + insert
include/secrets.h        YOUR credentials (git-ignored). Template: secrets.example.h
tools/native_tests/      host-side tests for door_logic.h
supabase/migrations/     001_additive_telemetry.sql
supabase/proposals/      device_sharing.sql (NOT applied; optional design)
dashboard/               React + TypeScript + Vite
```

## ⚠ Security first
* **Rotate** the Wi-Fi password and the device-account password: they were shared in chat, and `include/secrets.h` content was printed into an AI-session transcript while this work was done.
* `include/secrets.h` was previously *not* in `.gitignore`; it is now. Run `git status` before committing and make sure it is not listed. If it was ever committed, rotate and purge history.
* The browser only ever uses the **publishable** key. Never put a service-role/secret key in `dashboard/`.

## Firmware

```bash
pio run                                   # compile (core 2.x, espressif32@^6.9.0)
pio run -t upload && pio device monitor   # flash + 115200 baud
./tools/native_tests/run.sh               # logic tests, no hardware needed
```

### Startup / calibration (Serial Monitor, 115200)
Nobody can hold a sensor perfectly still, so "still enough" is judged from **variability**, not from zero readings (a resting gyro has a non-zero offset anyway).

**Step 1: gyro offset (door closed).** 3 s warm-up (discarded), then the firmware gathers 5 s of *accepted* steady samples. Every 20 ms it looks at the last 1 s of data:
* *Steady* if each axis's standard deviation is ≤ `STILL_MAX_STD_DPS` (1.0 dps) and the window's average is within `STILL_MAX_SHIFT_DPS` (0.5 dps) of the samples already accepted.
* *Not steady* (a bump, a button press, a sustained turn): collection **pauses** ("Movement detected; pausing briefly."). Samples already accepted are **kept**, and collection resumes when the window settles ("Steady again; resuming."). Nothing restarts.
* The pool is discarded only if the average stays shifted for `SHIFT_RESTART_S` (8 s), meaning the offset itself moved (e.g. still warming up).
* The offset is a **trimmed mean** per axis: sort, drop the lowest and highest 10 % (`BIAS_TRIM_FRACTION`), average the rest, so a few leftover spikes cannot drag it. The two halves of the pool must agree within 0.5 dps, otherwise the older half is dropped and collection continues.
* Progress is printed as `Calibration: 65%` (at most every 1.5 s). Messages are rate-limited.
* **Timeout:** after `BIAS_MAX_WAIT_MS` (30 s) it stops, prints *why* (e.g. "only 1.2 of 5.0 s were steady enough; the calmest 1 s window varied by 1.4 dps, limit 1.0"), accepts nothing, and waits. **Press Button 1 or send `c`** to retry. The door stays `unknown` until it succeeds.

**Step 2: open/close positions (2 rounds).** OPEN → press Button 1 → CLOSE fully → press Button 1, twice.
* A press only *starts* a capture. After `SETTLE_MS` (0.5 s) the position is saved as soon as the last `CAPTURE_WINDOW_MS` (0.5 s) looks steady (variability ≤ limit **and** average rotation ≤ `CAPTURE_MAX_MEAN_DPS`). So a button bump leaves the endpoint alone, while a continuing slow turn is not mistaken for "settled".
* Rotation is integrated the whole time. Real motion during settling is included, and if it is > 1.5° you are told ("Position saved." plus a note).
* If it cannot settle within `SETTLE_TIMEOUT_MS` (4 s) you're asked to press again. Nothing is consumed or lost.
* Each round is validated as before (open vs close mismatch ≤ max(4°, 5 %); rounds agree within max(5°, 8 %); same hinge axis) with specific retry prompts.
* Manual re-zero on a closed door (Button 1 after calibration) uses the same settle step.
* Send `c` any time to recalibrate without reflashing.

### Tuning calibration sensitivity
All values are at the **top of `include/config.h`** ("CALIBRATION TUNING"). They are starting points from simulation, **not validated on your hardware**. Change one at a time and watch the monitor.

| Symptom | Adjust | Initial → try |
|---|---|---|
| Keeps pausing / times out with "calmest window varied by X" | `STILL_MAX_STD_DPS`: set a little above the "calmest window" X printed in the timeout message | 1.0 → 1.5–2.5 dps |
| Pauses right after a bump take too long to resume | `STILL_WINDOW_MS` (shorter) | 1000 → 600 ms |
| "Steady again" never comes even though still (offset changing) | `STILL_MAX_SHIFT_DPS` | 0.5 → 0.8 dps |
| Takes too long overall | `BIAS_REQUIRED_ACCEPTED_MS` (less data, noisier offset) | 5000 → 3000 ms |
| Gives up before it can finish | `BIAS_MAX_WAIT_MS` | 30000 → 60000 ms |
| Button press never settles | `CAPTURE_MAX_MEAN_DPS`, `SETTLE_TIMEOUT_MS` | 1.5 → 2.5 dps; 4000 → 6000 ms |
| Healthy unit rejected as "implausibly large" offset | `BIAS_MAX_ABS_DPS` | 10 → up to 20 dps |

Don't loosen the limits indefinitely: it hides drift and lets slow movement through (below).

### Limits you should know about (physics, not tuning)
* **A gyro turning at a constant slow rate looks exactly like a gyro with a constant offset.** The tests include a case that shows this: a perfectly constant 2 dps turn from the very first sample is accepted as offset. What *is* caught: rotation above the plausibility cap (`BIAS_MAX_ABS_DPS`), anything with variability above the limit, turns that start or stop during collection (the window mean shifts, so it pauses and the old samples are kept), and a bad offset shows up as an open/close mismatch in step 2. A creeping turn slower than `STILL_MAX_SHIFT_DPS` (0.5 dps = 30°/min) can be absorbed.
* **Stationary ≠ closed.** Nothing here zeroes the angle when motion stops. The angle is zeroed only by a button press on a door *you* say is closed (manual) or a reed/Hall contact. The closed threshold is still 5°; it was not widened to hide drift.
* Raising the variability limit trades tolerance for the ability to notice real movement. A reed switch remains the only reliable closure signal.

### States (never silently "closed")
| Thing | Values |
|---|---|
| Door state | `closed`, `open`, `unknown` (not calibrated, or MPU6050 not answering, or reed not yet read) |
| Closure source | `estimated` (gyro angle fell inside 5°), `manual` (you pressed the button on a door you say is closed), `reed` (independent contact), `none` |
| Angle quality | `none`, `estimated`, `degraded` (missed samples / sensor loss since the last zero) |
| Calibration | `warmup`, `bias`, `awaiting_moves`, `calibrated`, `fault` |

An `unknown` door never starts an alarm; an already-open door whose sensor then fails keeps its timer and alarm (fail-safe).

### Drift handling: what it does
* Real timestamps (`micros()`), not an assumed 20 ms. Gaps 60–250 ms are integrated and counted as missed samples; gaps > 250 ms are **not** integrated and mark the angle `degraded`; no good read for 1 s → sensor invalid (door `unknown`), with MPU re-initialisation attempts.
* Light smoothing (EMA) only. No blanket dead-band, so slow openings aren't discarded.
* **Bias refinement** only inside a 3 s block where every axis is quiet (σ ≤ 0.2 °/s), no sample strays > 2.5 °/s from the bias, the block mean is within 1.5 °/s, and total change stays within 3 °/s of the original calibration. The drift accumulated during that stationary block is subtracted from the angle. *Limit:* a door creeping slower than 1.5 °/s for 3 s is indistinguishable from bias and would be absorbed; human door motion is much faster.
* The frame stops the door at 0°, so the estimate is clamped at −3°. This is a physical limit, not "zero whenever still".
* **Stationary never means closed.** The angle is zeroed only by the button (manual) or the reed switch.

### What software alone can NOT guarantee
A gyro integrates a rate: error accumulates, and the accelerometer cannot fix yaw about a vertical hinge (gravity is the same open or closed). Without an independent sensor, a long enough session *will* drift: a closed door may read open (alarm stuck on) or an open door closed. Mitigations above shrink and label this; only the reed/Hall sensor removes it. With the reed enabled the gyro is just an approximate angle that is reset to 0 on every verified closure.

### Optional reed switch / Hall sensor
Pin map in use: GPIO21/22 (I²C), 32 (button), 27 (buzzer). **Recommended: GPIO33**: input-capable, internal pull-up, not a boot-strap pin, free.

* **Reed switch (recommended, simplest):** it's a dry contact. One lead → GPIO33, other lead → GND. The ESP32's internal pull-up (to 3.3 V) is enabled in software, so there is **no voltage-compatibility issue** and no resistor needed. A 100 nF capacitor across the contacts helps with long wires. Magnet on the door, reed on the frame so the contact is **closed when the door is closed** (LOW = closed). Test with a multimeter; mount away from the door-gasket magnets.
* **Hall sensor:** use a digital, open-drain/open-collector part rated for **3.3 V supply** (check the datasheet; e.g. many A3144 modules need 4.5 V+). Power it from 3.3 V, output → GPIO33 with the internal pull-up (or an external 10 kΩ to 3.3 V), and make sure the output is LOW at the magnet. **Never feed a 5 V output directly into an ESP32 pin.**
* Enable: set `USE_REED_SWITCH 1` in `include/config.h` (or `-DUSE_REED_SWITCH=1`). The contact is debounced 50 ms; closed contact → closed, angle := 0, alarm stops; open contact → open. A broken wire reads as "open" (fail-safe: alarm rather than silence). A door ajar by a few mm may still hold the contact closed depending on magnet gap.
* *Compile-verified only*; not wired or tested on hardware.

### Cloud upload
* Runs in its own task. Wi-Fi/Supabase failure never blocks the gyro, alarm or buzzer.
* The queue holds **only the latest** snapshot: best-effort telemetry, not a durable log. `received_at` is server arrival time.
* **Legacy schema (default, `CLOUD_SEND_EXTENDED 0`):** only the original columns. Door `unknown` snapshots are *not uploaded* (the boolean cannot express unknown).
* **Extended (`CLOUD_SEND_EXTENDED 1`):** adds boot id, sequence number, device-clock `measured_at`, door state, closure source, calibration state, angle quality, sensor validity, missed samples, bias adjustments, reed state. **Apply the migration first.**

```sql
-- Supabase SQL editor: paste supabase/migrations/001_additive_telemetry.sql
```
Then set `CLOUD_SEND_EXTENDED 1` and reflash. The migration is additive/nullable, leaves RLS and policies alone, and the dashboard works before and after.

## Dashboard (localhost)

```bash
cd dashboard
cp .env.example .env.local      # then edit: VITE_SUPABASE_URL, VITE_SUPABASE_PUBLISHABLE_KEY
npm install
npm run dev                      # http://127.0.0.1:5173  (bound to localhost only)
npm test                         # 20 tests
npm run build                    # typecheck + production build
```
`http://127.0.0.1:5173/?demo=1` (or the "demo" link on the login screen) opens an isolated, clearly bannered **demo mode** with synthetic data; it never queries Supabase.

**Login and RLS:** every row's `owner_id` is the *device's* account and the SELECT policy is `owner_id = auth.uid()`. So the browser must sign in **as that same account** to see anything: type the credentials into the login form; nothing is hardcoded. I verified live that an *unauthenticated* request is rejected (`42501 permission denied`, HTTP 401), so exposing the publishable key doesn't expose data. If other people need access without sharing the device password, see `supabase/proposals/device_sharing.sql` (explicit per-device viewer grants; not applied).

Data: bounded queries (newest first, 1000 rows/page, max 6000 rows), then one incremental request every 5 s that never overlaps the previous one and pauses while the tab is hidden. Errors keep the last data and show a banner. Ranges beyond what 6000 rows (~8 h of 5 s data) covers show a "newest N readings only" note; real long-range history would need a server-side downsampling view.

Display rules: no reading ≠ 0; "unknown" is never shown as "closed"; data older than 60 s is labelled **stale/last known** (and 15–60 s "delayed"); stale telemetry is never attributed to Wi-Fi specifically; open duration is the value at the last report, not extrapolated; history is built from 5 s snapshots and says short events can be missed. The dashboard is read-only: no remote buzzer/reset (that would need a designed, authenticated command channel).

Temperature honesty: the BMP280 measures **where it is mounted**. On the outside of the door it reads the room/door-surface air, *not* the fridge interior. For interior readings, put the sensor (or a probe on a thin cable) inside, away from the door gasket and cooling vent, and keep the ESP32 outside; the gyro still must be rigidly on the door.

## Verification status (be precise)
| Item | Status |
|---|---|
| Firmware compiles (default, reed on, extended on) | ✅ `pio run`, no errors |
| Logic: rolling-window bias measurement (tremor, bumps, deliberate rotation, offset step, timeout), trimmed mean, settle gate and button-bump capture, calibration validation, drift/adaptation, gaps, alarm transitions, debouncing, state resolution | ✅ 546 host-side checks on *simulated* gyro data |
| Dashboard typecheck, build, 20 tests (login failure, empty, stale, missing temp, unknown door, fetch error, demo isolation) | ✅ |
| Layout at desktop and 390 px, dark mode | ✅ viewed via headless Chrome (demo data) |
| Anonymous DB access denied | ✅ live request |
| Authenticated owner read; extended inserts; migration | ⬜ not run (needs your login / your approval) |
| **Any behavior on real hardware** (sensor noise, real drift, buzzer, button, reed, Wi-Fi loss) | ⬜ **not tested** |

Threshold values in `config.h` are engineering estimates from simulation, not tuned on your sensor. Expect to adjust them after the checklist.

## Hardware test checklist
Record serial output for each. ☐ = pass/fail.
1. ☐ Boot: warm-up, then `Calibration: N%` climbing. Hold the board lightly in your hand: small tremor should still complete. Tap the table: "Movement detected; pausing briefly." then it **resumes** (percent not reset). Rotate the board steadily for the whole period: must time out after 30 s with a reason and no calibration. Press Button 1 to retry.
   Note the "calmest window" number in any timeout message and compare with `STILL_MAX_STD_DPS`.
2. ☐ Calibration: press Button 1 firmly at each position: it should say "settling…" then "Position saved." without the press changing the measured angle (compare a press vs. releasing the door first). Good run accepted; deliberately close only halfway in a round → rejected with a clear message; retry works.
3. ☐ Slow opening (~10 s to 60°): angle tracks, door goes `open` past 7°.
4. ☐ Fast opening (<1 s), then fast closing.
5. ☐ Pause mid-swing 5 s, continue: angle continues, no jump; door still `open`.
6. ☐ Partial opening (~15°) and ≤5° ajar: expected open / closed behavior.
7. ☐ Hold the door open 60 s: angle stays put (small drift OK), "Gyro offset refined" may print.
8. ☐ Closure: door returns to ≤5°; "EVENT: Door closed (estimated)"; alarm stops.
9. ☐ Alarm: open and wait: **"BUZZ!" printed once at 10 s**, buzzer sounds (LEDC ch.4, no "LEDC is not initialized"), stops on closure, timer restarts on next opening.
10. ☐ Drift: leave closed 30 min, then open/close 10×: note the angle error at each closure; press Button 1 (door closed) and note "estimate was X deg".
11. ☐ Unplug SDA mid-run: after ~1 s "MPU6050 stopped responding", door `unknown`, no false "closed"; replug: responding again, quality `degraded` until re-zero.
12. ☐ Wi-Fi loss: switch the router/AP off: sensing, alarm and serial keep working; "WiFi unavailable" only. Restore: reconnects and uploads resume; dashboard goes stale then live again.
13. ☐ Supabase: "reading saved", rows visible in the dashboard (with extended mode after migration).
14. ☐ Reed (if installed): wire per above, enable flag; closed/open events within ~50 ms; closure zeroes angle and stops alarm; unplug a wire: reads open.

## Known limitations / remaining work
* Gyro-only closure remains an estimate. Fit the reed/Hall sensor for real reliability.
* Uploads are latest-only; outages lose intermediate readings. A durable buffer would need NVS/ring storage + back-fill.
* The MPU6050 offset varies with temperature (fridge-door temperature swings); the adaptive bias helps but is bounded.
* Dashboard history over several hours is capped by row paging; a Postgres view/RPC with time-bucketed aggregates would fix it.
* Dashboard "age" uses the browser clock vs server `received_at`; a skewed laptop clock skews freshness.
* Realtime subscriptions were deliberately not used (polling is simpler and robust); if wanted, add the table to the `supabase_realtime` publication and unsubscribe on unmount.
