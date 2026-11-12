#pragma once
// ------------------------------------------------------------------
// Stay-Fresh configuration. Pure constants: no Arduino headers, so the
// same file is used by the firmware and by the host-side unit tests.
// ------------------------------------------------------------------
#include <stdint.h>

#define STAYFRESH_FW_VERSION "0.2.0"

// ---- Feature flags -------------------------------------------------

// 0 = current hardware only (gyro estimates open/closed).
// 1 = a magnetic reed switch / Hall sensor is wired to REED_PIN and
//     decides open/closed. The gyro then only supplies an approximate angle.
#ifndef USE_REED_SWITCH
#define USE_REED_SWITCH 0
#endif

// 0 = upload only the columns that exist in the original table.
// 1 = also upload the extra columns from supabase/migrations/001_*.sql.
// Leave at 0 until that migration has been applied, otherwise PostgREST
// rejects every insert (unknown column).
#ifndef CLOUD_SEND_EXTENDED
#define CLOUD_SEND_EXTENDED 0
#endif

namespace cfg {

// ====================================================================
//  CALIBRATION TUNING: START HERE if calibration feels too strict/loose
// ====================================================================
// These are STARTING VALUES chosen from simulation and typical MPU6050
// behaviour. They are not validated on your hardware. Watch the Serial
// monitor ("calmest window ...") and adjust one number at a time.
//
// Gyro units are degrees/second (dps). A resting MPU6050 shows noise
// around 0.05-0.15 dps; a hand holding the door adds tremor, typically
// well under 1 dps rms; a bumped button gives a short spike of tens of dps.

constexpr uint32_t SAMPLE_US = 20000;           // gyro poll period: 20 ms = 50 Hz

// Stability window: how much recent data is judged "still enough".
constexpr uint32_t STILL_WINDOW_MS = 1000;      // ms. Longer = steadier decision, slower to resume after a bump.
constexpr float STILL_MAX_STD_DPS = 1.0f;       // dps. Allowed per-axis spread (std dev) inside the window.
                                                //   TOO STRICT? raise to 1.5-2.5. Rotation at 5+ dps will
                                                //   still fail, but a sustained slow turn (see below) will not.
// Sustained movement: window average vs. the samples already accepted.
constexpr float STILL_MAX_SHIFT_DPS = 0.5f;     // dps. Window mean may differ from the accepted pool by this much.
constexpr float SHIFT_RESTART_S = 8.0f;         // s. Restart the pool only if the shift persists this long
                                                //   (offset really changed). Shorter shifts just pause.
// Bias measurement.
constexpr uint32_t WARMUP_MS = 3000;            // ms discarded first (sensor self-heating)
constexpr uint32_t BIAS_REQUIRED_ACCEPTED_MS = 5000;  // ms of ACCEPTED (steady) samples needed
constexpr uint32_t BIAS_MAX_WAIT_MS = 30000;    // ms total (incl. warm-up) before giving up and offering a retry
constexpr float BIAS_TRIM_FRACTION = 0.10f;     // drop this fraction from EACH end per axis before averaging
constexpr float BIAS_MAX_HALF_DIFF_DPS = 0.5f;  // dps. 1st vs 2nd half of the pool must agree (else: still drifting)
constexpr float BIAS_MAX_ABS_DPS = 10.0f;       // dps. Largest average rate accepted as an OFFSET. A steady rotation
                                                //   has low variability, so only this cap rejects fast constant turns.
                                                //   Datasheet allows +/-20 raw; typical parts are within a few dps.
                                                //   If a healthy unit is rejected as 'implausibly large', raise toward 20.

// Button presses (confirming the open/closed position, or manual zero).
constexpr uint32_t SETTLE_MS = 500;             // ms to wait after the press before trusting the position
constexpr uint32_t SETTLE_TIMEOUT_MS = 4000;    // ms to wait for it to steady before asking you to press again
constexpr uint32_t CAPTURE_WINDOW_MS = 500;     // ms of recent data that must look steady to confirm
constexpr float CAPTURE_MAX_MEAN_DPS = 1.5f;    // dps. Average rotation allowed while confirming a position

constexpr int STILL_WINDOW_N = (int)(STILL_WINDOW_MS * 1000UL / SAMPLE_US);
constexpr int CAPTURE_N = (int)(CAPTURE_WINDOW_MS * 1000UL / SAMPLE_US);
constexpr int POOL_MAX = (int)(BIAS_REQUIRED_ACCEPTED_MS * 1000UL / SAMPLE_US) + 60;
static_assert(CAPTURE_N <= STILL_WINDOW_N, "capture window must fit inside the stability window");
// ====================================================================

// ---- Pins ----------------------------------------------------------
constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;
constexpr int BUTTON_PIN = 32;   // INPUT_PULLUP, pressed = LOW
constexpr int BUZZER_PIN = 27;
constexpr int BUZZER_CHANNEL = 4;  // explicit LEDC channel (core 2.x API)
// Optional reed/Hall input. GPIO33: input-capable, has an internal pull-up,
// not a boot-strapping pin, not used by anything else in this project.
constexpr int REED_PIN = 33;

// ---- Timing --------------------------------------------------------
constexpr uint32_t PRINT_MS = 500;
constexpr uint32_t TEMP_MS = 5000;          // temperature + telemetry period
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t REED_DEBOUNCE_MS = 50;

// EDIT THIS: how long the door may stay open before the buzzer starts.
constexpr uint32_t DOOR_OPEN_LIMIT_MS = 10UL * 1000UL;

// ---- Door thresholds (hysteresis prevents flicker) -----------------
constexpr float CLOSED_TOLERANCE_DEG = 5.0f;  // <= this => closed
constexpr float OPEN_THRESHOLD_DEG = 7.0f;    // >  this => open
// The frame physically stops the door at 0 deg, so a large negative
// estimate is pure drift. Clamp it (this is a physical limit, NOT
// "zero whenever stationary").
constexpr float NEGATIVE_CLAMP_DEG = 3.0f;
constexpr float MAX_ANGLE_DEG = 200.0f;

// ---- Gyro filtering and sample handling ----------------------------
constexpr float RATE_EMA_ALPHA = 0.5f;        // light smoothing; integral-safe
constexpr float NOMINAL_DT_S = SAMPLE_US / 1e6f;
constexpr float GAP_WARN_DT_S = 0.060f;       // >3 samples late: count a miss
constexpr float GAP_MAX_INTEGRATE_DT_S = 0.250f;  // longer: do not integrate
constexpr uint32_t SENSOR_INVALID_AFTER_MS = 1000; // no good read this long

// ---- Bias adaptation (only in carefully detected stationary blocks) -
constexpr float ADAPT_BLOCK_S = 3.0f;
constexpr float ADAPT_MAX_STD_DPS = 0.20f;    // every axis, within the block
constexpr float ADAPT_MAX_PEAK_DPS = 2.5f;    // any sample vs current bias
constexpr float ADAPT_MAX_MEAN_DPS = 1.5f;    // |mean residual| must be small
constexpr float ADAPT_GAIN = 0.7f;            // move this fraction per block
constexpr float ADAPT_MAX_TOTAL_DPS = 3.0f;   // never drift >3 dps from calibration

// ---- Calibration validation ----------------------------------------
constexpr float CAL_MIN_MOVE_DEG = 20.0f;
constexpr float CAL_MIN_ALIGNMENT = 0.92f;    // cos(angle to hinge axis)
constexpr float CAL_MAX_CLOSURE_ABS_DEG = 4.0f;  // |open - close| per round
constexpr float CAL_MAX_CLOSURE_REL = 0.05f;
constexpr float CAL_MAX_REPEAT_ABS_DEG = 5.0f;   // round 1 vs round 2
constexpr float CAL_MAX_REPEAT_REL = 0.08f;

// ---- Cloud ----------------------------------------------------------
constexpr int REED_ACTIVE_STATE_CLOSED = 0;   // LOW = magnet near = closed

}  // namespace cfg
