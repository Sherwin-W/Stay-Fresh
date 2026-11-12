#pragma once
// ------------------------------------------------------------------
// Stay-Fresh decision logic. NO Arduino / hardware calls in this file.
// Everything here is plain C++11 so it can be compiled and unit-tested on
// a laptop (tools/native_tests) and reused unchanged by the firmware.
//
// What this code can and cannot do (read before trusting the angle):
//   * A gyroscope measures angular RATE. We integrate it to get an angle,
//     so any bias error accumulates ("drift"). Nothing below can remove
//     drift completely; it can only (a) measure the bias carefully,
//     (b) refine it while the sensor is verifiably still, (c) refuse to
//     calibrate with inconsistent data, and (d) label how much to trust
//     the result.
//   * An accelerometer cannot supply a yaw reference for a door turning
//     about a vertical hinge (gravity looks the same open or closed), so
//     there is no accelerometer fusion here.
//   * "Stationary" does NOT mean "closed". The angle is never zeroed just
//     because motion stopped. It is zeroed only by (1) the user pressing the
//     button on a physically closed door, or (2) a verified reed/Hall
//     closed contact.
// ------------------------------------------------------------------
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace sf {

// ---------------------------- Vec3 --------------------------------
struct Vec3 {
  float x, y, z;
  Vec3() : x(0), y(0), z(0) {}
  Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3 operator*(const Vec3& a, float s) { return Vec3(a.x * s, a.y * s, a.z * s); }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float magnitude(const Vec3& v) { return sqrtf(dot(v, v)); }
inline float maxAbsAxis(const Vec3& v) {
  float m = fabsf(v.x);
  if (fabsf(v.y) > m) m = fabsf(v.y);
  if (fabsf(v.z) > m) m = fabsf(v.z);
  return m;
}
inline Vec3 normalized(const Vec3& v) {
  float m = magnitude(v);
  return m > 1e-6f ? v * (1.0f / m) : Vec3();
}

// ---------------------------- Enums -------------------------------
enum class DoorState : uint8_t { Unknown, Closed, Open };
enum class ClosureSource : uint8_t { None, Estimated, Manual, Reed };
enum class AngleQuality : uint8_t { None, Estimated, Degraded };
enum class CalState : uint8_t { Fault, Warmup, Bias, Moves, Done };

inline const char* toString(DoorState s) {
  switch (s) {
    case DoorState::Closed: return "closed";
    case DoorState::Open: return "open";
    default: return "unknown";
  }
}
inline const char* toString(ClosureSource s) {
  switch (s) {
    case ClosureSource::Estimated: return "estimated";  // gyro angle fell inside tolerance
    case ClosureSource::Manual: return "manual";        // user pressed the button
    case ClosureSource::Reed: return "reed";            // independent contact confirmed
    default: return "none";
  }
}
inline const char* toString(AngleQuality q) {
  switch (q) {
    case AngleQuality::Estimated: return "estimated";
    case AngleQuality::Degraded: return "degraded";  // missed samples / sensor loss since last reference
    default: return "none";
  }
}
inline const char* toString(CalState s) {
  switch (s) {
    case CalState::Warmup: return "warmup";
    case CalState::Bias: return "bias";
    case CalState::Moves: return "awaiting_moves";
    case CalState::Done: return "calibrated";
    default: return "fault";
  }
}

// ------------------- Gyro bias (simple average) --------------------
// Original, simple behaviour: average BIAS_SAMPLES gyro readings (about
// 2 s at 50 Hz) taken while the door is closed. Perfect stillness is NOT
// required and the step never restarts itself. Because the estimate is a
// plain average, a bump or tremor adds a small error: that is the price of
// the easy workflow (drift is NOT fixed by this; see README).
class BiasAverager {
 public:
  BiasAverager() { reset(); }
  void reset() {
    n_ = 0;
    for (int i = 0; i < 3; i++) { sum_[i] = 0; min_[i] = 1e9f; max_[i] = -1e9f; }
    bias_ = Vec3();
  }
  // Returns true when the BIAS_SAMPLES-th sample has been added.
  bool add(const Vec3& g) {
    if (n_ >= cfg::BIAS_SAMPLES) return true;
    const float v[3] = {g.x, g.y, g.z};
    for (int i = 0; i < 3; i++) {
      sum_[i] += v[i];
      if (v[i] < min_[i]) min_[i] = v[i];
      if (v[i] > max_[i]) max_[i] = v[i];
    }
    n_++;
    if (n_ < cfg::BIAS_SAMPLES) return false;
    bias_ = Vec3((float)(sum_[0] / n_), (float)(sum_[1] / n_), (float)(sum_[2] / n_));
    return true;
  }
  const Vec3& bias() const { return bias_; }
  int count() const { return n_; }
  // Information only (never blocks): the sensor clearly moved while averaging.
  bool noisy() const {
    for (int i = 0; i < 3; i++)
      if (n_ > 0 && max_[i] - min_[i] > cfg::BIAS_NOISY_RANGE_DPS) return true;
    return false;
  }

 private:
  int n_;
  double sum_[3];
  float min_[3], max_[3];
  Vec3 bias_;
};

// ------------------------ Calibration checks ----------------------
struct CalibrationResult {
  bool ok;
  char message[200];
  Vec3 axis;            // unit hinge axis; positive rotation = opening
  float openAngleDeg;   // average measured opening angle
  CalibrationResult() : ok(false), axis(), openAngleDeg(0) { message[0] = 0; }
};

// The four button-confirmed movements: open, close, open, close
// (integrated rotation vectors, degrees, sensor frame).
// Basic checks only: negligible movement, inconsistent direction, and the
// original "no movement more than 40 % from the average" check.
inline CalibrationResult validateCalibration(const Vec3 moves[4]) {
  CalibrationResult r;
  // Opening and closing are opposite rotations about the same hinge axis.
  Vec3 direction = (moves[0] - moves[1]) + (moves[2] - moves[3]);
  float length = magnitude(direction);
  if (length < 1.0f) {
    snprintf(r.message, sizeof(r.message),
             "the four movements cancel out or are negligible: open and close must be opposite turns of the door.");
    return r;
  }
  Vec3 axis = direction * (1.0f / length);
  const float p[4] = {dot(moves[0], axis), -dot(moves[1], axis), dot(moves[2], axis), -dot(moves[3], axis)};
  for (int i = 0; i < 4; i++) {
    float mag = magnitude(moves[i]);
    const char* what = (i % 2 == 0) ? "opening" : "closing";
    if (mag < cfg::CAL_MIN_MOVE_DEG) {
      snprintf(r.message, sizeof(r.message),
               "the %s movement %d was only %.0f deg (need at least %.0f): the door barely moved.", what, i + 1, mag,
               cfg::CAL_MIN_MOVE_DEG);
      return r;
    }
    if (p[i] < cfg::CAL_MIN_MOVE_DEG || p[i] / mag < cfg::CAL_MIN_ALIGNMENT) {
      snprintf(r.message, sizeof(r.message),
               "the %s movement %d turned the wrong way or about a different axis than the others.", what, i + 1);
      return r;
    }
  }
  float average = 0.25f * (p[0] + p[1] + p[2] + p[3]);
  for (int i = 0; i < 4; i++) {
    if (fabsf(p[i] - average) > average * cfg::CAL_MAX_DEVIATION) {
      snprintf(r.message, sizeof(r.message),
               "movement sizes differ too much (%.0f, %.0f, %.0f, %.0f deg). Use the same open position each time.",
               p[0], p[1], p[2], p[3]);
      return r;
    }
  }
  r.ok = true;
  r.axis = axis;
  r.openAngleDeg = average;
  snprintf(r.message, sizeof(r.message), "Normal open angle: %.1f degrees.", average);
  return r;
}

// Collects the four movements. Each Button 1 press is accepted immediately.
class CalibrationSession {
 public:
  enum class Outcome : uint8_t { Saved, Complete, Failed };

  CalibrationSession() { begin(); }
  void begin() {
    stage_ = 0;
    result_ = CalibrationResult();
  }
  int round() const { return stage_ / 2 + 1; }
  bool expectingOpen() const { return stage_ % 2 == 0; }
  const CalibrationResult& result() const { return result_; }

  // 'rotation' = rotation integrated since the previous press.
  Outcome press(const Vec3& rotation) {
    moves_[stage_++] = rotation;
    if (stage_ < 4) return Outcome::Saved;
    result_ = validateCalibration(moves_);
    if (!result_.ok) {
      stage_ = 0;  // as in the original: start the four presses again
      return Outcome::Failed;
    }
    return Outcome::Complete;
  }

 private:
  Vec3 moves_[4];
  int stage_;
  CalibrationResult result_;
};

// ------------------------ Door-angle estimator --------------------
class DoorEstimator {
 public:
  struct StepResult {
    bool opened, closed, biasAdapted;
    StepResult() : opened(false), closed(false), biasAdapted(false) {}
  };

  DoorEstimator() { clearAll(); }

  void clearAll() {
    bias_ = calBias_ = filt_ = rotation_ = Vec3();
    clearCalibration();
    biasAdjustments_ = 0;
    missed_ = 0;
    clamped_ = 0;
  }
  void clearCalibration() {
    calibrated_ = false;
    axis_ = Vec3();
    openAngle_ = 0;
    angle_ = 0;
    estClosed_ = true;
    closure_ = ClosureSource::None;
    quality_ = AngleQuality::None;
    sinceRef_ = 0;
    rotation_ = Vec3();
    hasFilt_ = false;
    resetBlock();
  }
  void setBias(const Vec3& b) {
    bias_ = calBias_ = b;
    hasFilt_ = false;
    resetBlock();
  }
  void setCalibration(const Vec3& axis, float openAngleDeg) {
    axis_ = normalized(axis);
    openAngle_ = openAngleDeg;
    calibrated_ = true;
  }

  // dt = real seconds since the previous GOOD gyro sample.
  StepResult step(const Vec3& raw, float dt) {
    StepResult r;
    sinceRef_ += dt;
    if (dt > cfg::GAP_WARN_DT_S) countMissed(dt);
    if (dt > cfg::GAP_MAX_INTEGRATE_DT_S) {
      // Too long to extrapolate. Do not invent motion; flag the angle.
      if (calibrated_) quality_ = AngleQuality::Degraded;
      hasFilt_ = false;
      resetBlock();
      return r;
    }

    Vec3 corrected = raw - bias_;
    filt_ = hasFilt_ ? filt_ + (corrected - filt_) * cfg::RATE_EMA_ALPHA : corrected;
    hasFilt_ = true;

    if (!calibrated_) {
      rotation_ = rotation_ + filt_ * dt;
    } else {
      angle_ += dot(filt_, axis_) * dt;
    }
    // Bias refinement only AFTER calibration, so calibration itself is plain integration.
    if (calibrated_) adaptBias(raw, dt, r);
    if (calibrated_) updateState(r);
    return r;
  }

  void markSensorLost() {
    if (calibrated_) quality_ = AngleQuality::Degraded;
    hasFilt_ = false;
    resetBlock();
  }

  // Establish the closed reference. Source says WHO asserted it.
  void zero(ClosureSource src) {
    lastZeroCorrection_ = angle_;
    angle_ = 0;
    estClosed_ = true;
    closure_ = src;
    sinceRef_ = 0;
    if (calibrated_) quality_ = AngleQuality::Estimated;
  }

  void resetRotation() { rotation_ = Vec3(); }

  const Vec3& rotation() const { return rotation_; }
  bool calibrated() const { return calibrated_; }
  float angle() const { return angle_; }
  float openAngle() const { return openAngle_; }
  bool estimatedClosed() const { return estClosed_; }
  ClosureSource closureSource() const { return closure_; }
  AngleQuality quality() const { return quality_; }
  float secondsSinceReference() const { return sinceRef_; }
  uint32_t missedSamples() const { return missed_; }
  uint32_t biasAdjustments() const { return biasAdjustments_; }
  uint32_t negativeClamps() const { return clamped_; }
  float lastZeroCorrection() const { return lastZeroCorrection_; }
  const Vec3& bias() const { return bias_; }

 private:
  struct Block {
    double sum[3], sumsq[3];
    float t;
    int n;
    bool ok;
  };

  void countMissed(float dt) {
    int lost = (int)(dt / cfg::NOMINAL_DT_S + 0.5f) - 1;
    if (lost > 0) missed_ += lost;
  }
  void resetBlock() {
    for (int i = 0; i < 3; i++) block_.sum[i] = block_.sumsq[i] = 0;
    block_.t = 0;
    block_.n = 0;
    block_.ok = true;
  }

  // Refine the bias ONLY from a 3 s block in which all of these hold:
  // every axis quiet (low std), no sample strays more than ADAPT_MAX_PEAK_DPS
  // from the current bias, the block mean is within ADAPT_MAX_MEAN_DPS of it,
  // and the result stays within ADAPT_MAX_TOTAL_DPS of the original calibration.
  // Limitation: a door creeping open slower than ADAPT_MAX_MEAN_DPS (1.5 deg/s)
  // for 3 s looks identical to bias and would be absorbed. Real door motion is
  // far faster, but this is a genuine limit, not a guarantee.
  void adaptBias(const Vec3& raw, float dt, StepResult& r) {
    const float v[3] = {raw.x, raw.y, raw.z};
    const float b[3] = {bias_.x, bias_.y, bias_.z};
    for (int i = 0; i < 3; i++) {
      block_.sum[i] += v[i];
      block_.sumsq[i] += (double)v[i] * v[i];
      if (fabsf(v[i] - b[i]) > cfg::ADAPT_MAX_PEAK_DPS) block_.ok = false;
    }
    block_.n++;
    block_.t += dt;
    if (block_.t < cfg::ADAPT_BLOCK_S) return;

    bool ok = block_.ok && block_.n >= (int)(0.6f * cfg::ADAPT_BLOCK_S / cfg::NOMINAL_DT_S);
    float mean[3] = {0, 0, 0};
    for (int i = 0; ok && i < 3; i++) {
      mean[i] = (float)(block_.sum[i] / block_.n);
      double var = block_.sumsq[i] / block_.n - (double)mean[i] * mean[i];
      if (var > 0 && sqrt(var) > cfg::ADAPT_MAX_STD_DPS) ok = false;
    }
    if (ok) {
      Vec3 resid = Vec3(mean[0], mean[1], mean[2]) - bias_;
      if (magnitude(resid) > cfg::ADAPT_MAX_MEAN_DPS) ok = false;
      Vec3 delta = resid * cfg::ADAPT_GAIN;
      Vec3 candidate = bias_ + delta;
      if (ok && magnitude(candidate - calBias_) > cfg::ADAPT_MAX_TOTAL_DPS) ok = false;
      if (ok) {
        bias_ = candidate;
        // The block was integrated with the old bias; remove that
        // stationary-period error from the running angle / rotation.
        angle_ -= dot(delta, axis_) * block_.t;
        biasAdjustments_++;
        r.biasAdapted = true;
      }
    }
    resetBlock();
  }

  void updateState(StepResult& r) {
    // The frame stops the door at 0 deg: a large negative angle is drift.
    if (angle_ < -cfg::NEGATIVE_CLAMP_DEG) {
      angle_ = -cfg::NEGATIVE_CLAMP_DEG;
      clamped_++;
    }
    if (angle_ > cfg::MAX_ANGLE_DEG) angle_ = cfg::MAX_ANGLE_DEG;

    if (estClosed_ && angle_ > cfg::OPEN_THRESHOLD_DEG) {
      estClosed_ = false;
      closure_ = ClosureSource::None;
      r.opened = true;
    } else if (!estClosed_ && angle_ <= cfg::CLOSED_TOLERANCE_DEG) {
      estClosed_ = true;
      closure_ = ClosureSource::Estimated;
      r.closed = true;
    }
  }

  Vec3 bias_, calBias_, filt_, rotation_, axis_;
  bool hasFilt_, calibrated_, estClosed_;
  float angle_, openAngle_, sinceRef_, lastZeroCorrection_ = 0;
  ClosureSource closure_;
  AngleQuality quality_;
  uint32_t missed_, biasAdjustments_, clamped_;
  Block block_;
};

// --------------------- Combined door state -------------------------
struct DoorInputs {
  bool reedEnabled;
  bool reedKnown;     // debounced contact state is available
  bool reedClosed;
  bool sensorValid;   // gyro is returning fresh data
  bool calibrated;
  bool estimatedClosed;
};

// Never reports "closed" just because information is missing.
inline DoorState resolveDoorState(const DoorInputs& in) {
  if (in.reedEnabled) {
    if (!in.reedKnown) return DoorState::Unknown;
    return in.reedClosed ? DoorState::Closed : DoorState::Open;
  }
  if (!in.sensorValid || !in.calibrated) return DoorState::Unknown;
  return in.estimatedClosed ? DoorState::Closed : DoorState::Open;
}

// --------------------------- Alarm ---------------------------------
class AlarmTimer {
 public:
  enum class Event : uint8_t { None, Started, Stopped };
  explicit AlarmTimer(uint32_t limitMs) : limitMs_(limitMs), running_(false), active_(false), openedAt_(0) {}

  // Closed: stop + reset timer. Open: run timer, start alarm at the limit.
  // Unknown: change nothing (an open door whose sensor fails keeps its
  // timer and alarm; an unknown door never starts one).
  Event update(uint32_t nowMs, DoorState s) {
    if (s == DoorState::Closed) {
      running_ = false;
      if (active_) {
        active_ = false;
        return Event::Stopped;
      }
      return Event::None;
    }
    if (s == DoorState::Open) {
      if (!running_) {
        running_ = true;
        openedAt_ = nowMs;
      }
      if (!active_ && nowMs - openedAt_ >= limitMs_) {
        active_ = true;
        return Event::Started;  // caller prints "BUZZ!" exactly once
      }
    }
    return Event::None;
  }
  bool active() const { return active_; }
  bool running() const { return running_; }
  uint32_t openMs(uint32_t nowMs) const { return running_ ? nowMs - openedAt_ : 0; }

 private:
  uint32_t limitMs_;
  bool running_, active_;
  uint32_t openedAt_;
};

// ------------------------- Debouncer -------------------------------
// Accepts a boolean level only after it has been steady for debounceMs.
class Debouncer {
 public:
  explicit Debouncer(uint32_t debounceMs) : debounceMs_(debounceMs), raw_(false), stable_(false), known_(false), changedAt_(0) {}
  void begin(bool raw, uint32_t nowMs) {
    raw_ = raw;
    changedAt_ = nowMs;
    known_ = false;
  }
  bool update(bool raw, uint32_t nowMs) {  // returns debounced level
    if (raw != raw_) {
      raw_ = raw;
      changedAt_ = nowMs;
    }
    if (nowMs - changedAt_ >= debounceMs_) {
      stable_ = raw_;
      known_ = true;
    }
    return stable_;
  }
  bool known() const { return known_; }
  bool value() const { return stable_; }

 private:
  uint32_t debounceMs_;
  bool raw_, stable_, known_;
  uint32_t changedAt_;
};

}  // namespace sf
