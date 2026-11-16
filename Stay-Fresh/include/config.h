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

constexpr uint32_t SAMPLE_US = 20000;           // gyro poll period: 20 ms = 50 Hz

// ---- Calibration (simple, button-guided) ---------------------------
constexpr int BIAS_SAMPLES = 100;               // gyro samples averaged for the offset (~2 s)
constexpr float BIAS_NOISY_RANGE_DPS = 8.0f;    // dps. Only prints a NOTE if the sensor moved this much; never blocks
constexpr float CAL_MIN_MOVE_DEG = 10.0f;       // each of the four movements must project at least this far
constexpr float CAL_MAX_DEVIATION = 0.40f;      // reject a movement > 40 % from the average size
// Reference behaviour while calibrating: if the gyro reads ~0 for STILL_MS, the
// sensor is not moving: ignore that rotation and slowly re-learn the offset.
constexpr float STILL_RATE_DPS = 1.5f;
constexpr float STILL_S = 1.0f;
constexpr float BIAS_ALPHA = 0.005f;

// ---- TEST SWITCH ----------------------------------------------------
// true  = bench testing without a fridge/door: temperature snapshots are
//         uploaded even when the door state is unknown / not calibrated
//         (door fields are sent as closed, angle 0). Everything else unchanged.
// false = normal operation.
constexpr bool TEST_TEMP_ONLY = true;

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

// ---- Cloud ----------------------------------------------------------
constexpr int REED_ACTIVE_STATE_CLOSED = 0;   // LOW = magnet near = closed

}  // namespace cfg
