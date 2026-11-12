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

// ---------------------- Stillness window --------------------------
// Rolling window of the most recent gyro samples. "Still enough" is judged
// from the VARIABILITY (std dev) of the window, never from "every reading is
// zero": a stationary gyro has a non-zero bias, and a hand adds tremor.
class StillnessWindow {
 public:
  StillnessWindow() { reset(); }
  void reset() { idx_ = 0; count_ = 0; }
  void push(const Vec3& v) {
    buf_[idx_] = v;
    idx_ = (idx_ + 1) % cfg::STILL_WINDOW_N;
    if (count_ < cfg::STILL_WINDOW_N) count_++;
  }
  int count() const { return count_; }

  // Mean and per-axis std dev of the most recent n samples. False if fewer
  // than n samples have been collected.
  bool stats(int n, Vec3& mean, Vec3& sd) const {
    if (n <= 0 || n > count_) return false;
    double s[3] = {0, 0, 0};
    for (int k = 0; k < n; k++) {
      const Vec3& v = at(k);
      s[0] += v.x; s[1] += v.y; s[2] += v.z;
    }
    const double m[3] = {s[0] / n, s[1] / n, s[2] / n};
    double q[3] = {0, 0, 0};
    for (int k = 0; k < n; k++) {
      const Vec3& v = at(k);
      q[0] += (v.x - m[0]) * (v.x - m[0]);
      q[1] += (v.y - m[1]) * (v.y - m[1]);
      q[2] += (v.z - m[2]) * (v.z - m[2]);
    }
    mean = Vec3((float)m[0], (float)m[1], (float)m[2]);
    sd = Vec3((float)sqrt(q[0] / n), (float)sqrt(q[1] / n), (float)sqrt(q[2] / n));
    return true;
  }

 private:
  const Vec3& at(int k) const {  // k = 0 is the newest sample
    int i = idx_ - 1 - k;
    if (i < 0) i += cfg::STILL_WINDOW_N;
    return buf_[i];
  }
  Vec3 buf_[cfg::STILL_WINDOW_N];
  int idx_, count_;
};

// Trimmed mean: sort, drop `frac` of the values from each end, average the
// rest. A few leftover spikes (a bump that slipped under the variability
// limit) then cannot drag the offset estimate. Sorts `a` in place.
inline float trimmedMean(float* a, int n, float frac) {
  for (int i = 1; i < n; i++) {
    float key = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > key) { a[j + 1] = a[j]; j--; }
    a[j + 1] = key;
  }
  int t = (int)(n * frac);
  if (n - 2 * t < 1) t = 0;
  double sum = 0;
  for (int i = t; i < n - t; i++) sum += a[i];
  return (float)(sum / (n - 2 * t));
}

// ---------------------- Initial gyro bias -------------------------
// Collects steady samples into a pool until it holds BIAS_REQUIRED_ACCEPTED_MS
// of them, then returns their trimmed mean.
//   * Disturbance (window std too high, or window mean far from the pool):
//     collection PAUSES; samples already accepted are KEPT; collection resumes
//     when the window settles.
//   * The pool is discarded only if the offset itself has moved (window mean
//     differs from the pool by > STILL_MAX_SHIFT_DPS for SHIFT_RESTART_S),
//     e.g. the sensor was still warming up.
//   * If BIAS_MAX_WAIT_MS passes first, it reports TimedOut with the reason.
// LIMITATION (physics, not tunable away): a gyro turning at a perfectly
// CONSTANT slow rate looks exactly like a gyro with a constant bias. Rotation
// that starts and stops is caught (it pauses and the pool is kept), and a
// slow turn below STILL_MAX_SHIFT_DPS can be absorbed. Rotation that is
// constant from the very first sample cannot be detected here; the
// open/close consistency check during calibration is the backstop.
class BiasMeasurement {
 public:
  enum class Status : uint8_t { Collecting, Paused, Done, TimedOut };

  explicit BiasMeasurement(float warmupS = cfg::WARMUP_MS / 1000.0f,
                           float requiredS = cfg::BIAS_REQUIRED_ACCEPTED_MS / 1000.0f,
                           float maxWaitS = cfg::BIAS_MAX_WAIT_MS / 1000.0f)
      : warmupS_(warmupS), requiredS_(requiredS), maxWaitS_(maxWaitS) {
    restart();
  }

  void restart() {
    elapsed_ = accepted_ = shiftS_ = 0;
    poolN_ = 0;
    for (int i = 0; i < 3; i++) poolSum_[i] = 0;
    pauses_ = restarts_ = dropped_ = 0;
    bigMean_ = noisy_ = 0;
    lastMeanMag_ = 0;
    bestStd_ = 1e9f;
    paused_ = false;
    bias_ = Vec3();
    reason_[0] = 0;
    status_ = Status::Collecting;
    win_.reset();
  }

  Status add(const Vec3& g, float dt) {
    if (status_ == Status::Done || status_ == Status::TimedOut) return status_;
    elapsed_ += dt;
    if (dt > cfg::GAP_MAX_INTEGRATE_DT_S) win_.reset();  // data no longer contiguous
    win_.push(g);

    if (elapsed_ >= warmupS_) {
      Vec3 mean, sd;
      if (win_.stats(cfg::STILL_WINDOW_N, mean, sd)) {
        float spread = maxAbsAxis(sd);
        if (spread < bestStd_) bestStd_ = spread;
        float meanMag = magnitude(mean);
        bool steady = spread <= cfg::STILL_MAX_STD_DPS && meanMag <= cfg::BIAS_MAX_ABS_DPS;
        if (meanMag > cfg::BIAS_MAX_ABS_DPS) { bigMean_++; lastMeanMag_ = meanMag; }
        else if (spread > cfg::STILL_MAX_STD_DPS) noisy_++;
        if (!steady) {
          shiftS_ = 0;
        } else if (poolN_ >= cfg::STILL_WINDOW_N / 2) {
          Vec3 poolMean((float)(poolSum_[0] / poolN_), (float)(poolSum_[1] / poolN_), (float)(poolSum_[2] / poolN_));
          if (magnitude(mean - poolMean) > cfg::STILL_MAX_SHIFT_DPS) {
            steady = false;  // sustained-movement check: pause, keep the pool
            shiftS_ += dt;
            if (shiftS_ >= cfg::SHIFT_RESTART_S) {  // it never went back: the offset really changed
              poolN_ = 0;
              for (int i = 0; i < 3; i++) poolSum_[i] = 0;
              accepted_ = 0;
              restarts_++;
              shiftS_ = 0;
            }
          } else {
            shiftS_ = 0;
          }
        }
        if (steady) {
          poolPush(g);
          accepted_ += dt;
          paused_ = false;
        } else if (!paused_) {
          paused_ = true;
          pauses_++;
        }
      }
    }

    if (accepted_ >= requiredS_) finalize();
    if ((status_ == Status::Collecting) && elapsed_ >= maxWaitS_) {
      explainTimeout();
      status_ = Status::TimedOut;
    }
    if (status_ == Status::Collecting && paused_) return Status::Paused;
    return status_;
  }

  const Vec3& bias() const { return bias_; }
  const char* reason() const { return reason_; }
  bool inWarmup() const { return elapsed_ < warmupS_; }
  int percent() const {
    int p = (int)(100.0f * accepted_ / requiredS_);
    return p > 100 ? 100 : p;
  }
  float acceptedS() const { return accepted_; }
  uint32_t pauses() const { return pauses_; }
  uint32_t restarts() const { return restarts_; }
  int samples() const { return poolN_; }
  float calmestWindowDps() const { return bestStd_; }

 private:
  void poolPush(const Vec3& g) {
    if (poolN_ >= cfg::POOL_MAX) dropOldestHalf();
    pool_[poolN_++] = g;
    poolSum_[0] += g.x; poolSum_[1] += g.y; poolSum_[2] += g.z;
  }
  void dropOldestHalf() {
    int half = poolN_ / 2;
    for (int i = half; i < poolN_; i++) pool_[i - half] = pool_[i];
    poolN_ -= half;
    accepted_ *= (float)poolN_ / (float)(poolN_ + half);
    poolSum_[0] = poolSum_[1] = poolSum_[2] = 0;
    for (int i = 0; i < poolN_; i++) {
      poolSum_[0] += pool_[i].x; poolSum_[1] += pool_[i].y; poolSum_[2] += pool_[i].z;
    }
  }
  Vec3 trimmedOf(int from, int to) const {  // pool_[from, to)
    float a[cfg::POOL_MAX];
    float out[3];
    for (int axis = 0; axis < 3; axis++) {
      int n = 0;
      for (int i = from; i < to; i++) a[n++] = axis == 0 ? pool_[i].x : axis == 1 ? pool_[i].y : pool_[i].z;
      out[axis] = trimmedMean(a, n, cfg::BIAS_TRIM_FRACTION);
    }
    return Vec3(out[0], out[1], out[2]);
  }
  void finalize() {
    Vec3 first = trimmedOf(0, poolN_ / 2);
    Vec3 second = trimmedOf(poolN_ / 2, poolN_);
    float diff = magnitude(first - second);
    if (diff > cfg::BIAS_MAX_HALF_DIFF_DPS) {
      // Offset was still moving: forget the older half and keep collecting.
      dropOldestHalf();
      dropped_++;
      return;
    }
    bias_ = trimmedOf(0, poolN_);
    if (magnitude(bias_) > cfg::BIAS_MAX_ABS_DPS) {
      snprintf(reason_, sizeof(reason_), "offset %.1f dps is implausibly large: sensor fault?", magnitude(bias_));
      status_ = Status::TimedOut;
      return;
    }
    status_ = Status::Done;
  }
  void explainTimeout() {
    if (accepted_ < 0.5f * requiredS_ && bigMean_ > noisy_) {
      snprintf(reason_, sizeof(reason_),
               "the sensor was turning at a steady %.1f dps on average, far above any plausible gyro offset (limit %.1f). "
               "Is the door or sensor moving? Keep the door closed and still.",
               lastMeanMag_, cfg::BIAS_MAX_ABS_DPS);
    } else if (accepted_ < 0.5f * requiredS_) {
      snprintf(reason_, sizeof(reason_),
               "only %.1f of %.1f s were steady enough (%u pauses). The calmest 1 s window still varied by %.2f dps "
               "(limit %.2f). Too much movement or vibration; rest your hands on something solid, or raise STILL_MAX_STD_DPS.",
               accepted_, requiredS_, (unsigned)pauses_, bestStd_, cfg::STILL_MAX_STD_DPS);
    } else if (restarts_ > 0 || dropped_ > 0) {
      snprintf(reason_, sizeof(reason_),
               "the offset kept changing (%u restarts): the sensor may still be warming up, or slowly turning.",
               (unsigned)(restarts_ + dropped_));
    } else {
      snprintf(reason_, sizeof(reason_),
               "collected %.1f of %.1f s but kept pausing (%u pauses). Hold the door and sensor steadier.",
               accepted_, requiredS_, (unsigned)pauses_);
    }
  }

  float warmupS_, requiredS_, maxWaitS_;
  float elapsed_, accepted_, shiftS_, bestStd_, lastMeanMag_;
  uint32_t pauses_, restarts_, dropped_, bigMean_, noisy_;
  int poolN_;
  double poolSum_[3];
  Vec3 pool_[cfg::POOL_MAX];
  StillnessWindow win_;
  bool paused_;
  Vec3 bias_;
  char reason_[256];
  Status status_;
};

// ----------------------- Settle gate -------------------------------
// After a button press the housing is bumped. Wait at least minMs, then
// confirm as soon as the sensor looks steady; give up after timeoutMs.
// The caller keeps integrating rotation the whole time, so any real motion
// during the settling period is included, not lost.
class SettleGate {
 public:
  enum class Result : uint8_t { Idle, Pending, Confirmed, TimedOut };
  SettleGate(uint32_t minMs, uint32_t timeoutMs) : minMs_(minMs), timeoutMs_(timeoutMs), active_(false), start_(0) {}
  bool begin(uint32_t nowMs) {  // false if already settling
    if (active_) return false;
    active_ = true;
    start_ = nowMs;
    return true;
  }
  void cancel() { active_ = false; }
  bool active() const { return active_; }
  Result service(uint32_t nowMs, bool settled) {
    if (!active_) return Result::Idle;
    uint32_t el = nowMs - start_;
    if (el >= minMs_ && settled) { active_ = false; return Result::Confirmed; }
    if (el >= timeoutMs_) { active_ = false; return Result::TimedOut; }
    return Result::Pending;
  }

 private:
  uint32_t minMs_, timeoutMs_;
  bool active_;
  uint32_t start_;
};

// ------------------------ Calibration checks ----------------------
struct CalibrationResult {
  bool ok;
  char message[200];
  Vec3 axis;            // unit hinge axis; positive rotation = opening
  float openAngleDeg;   // average measured opening angle
  CalibrationResult() : ok(false), axis(), openAngleDeg(0) { message[0] = 0; }
};

inline float allowedMismatch(float absDeg, float rel, float reference) {
  float r = rel * reference;
  return r > absDeg ? r : absDeg;
}

// One open+close round. 'open' and 'close' are the integrated rotation
// vectors (deg, sensor frame) for the two movements.
inline CalibrationResult validateRound(const Vec3& open, const Vec3& close, int round) {
  CalibrationResult r;
  float mo = magnitude(open), mc = magnitude(close);
  if (mo < cfg::CAL_MIN_MOVE_DEG || mc < cfg::CAL_MIN_MOVE_DEG) {
    snprintf(r.message, sizeof(r.message),
             "Round %d: the door only turned %.0f deg opening / %.0f deg closing (need at least %.0f). "
             "Open it to your normal position and make sure the sensor is firmly attached.",
             round, mo, mc, cfg::CAL_MIN_MOVE_DEG);
    return r;
  }
  Vec3 axis = normalized(open - close);
  float po = dot(open, axis), pc = -dot(close, axis);
  if (po / mo < cfg::CAL_MIN_ALIGNMENT || pc / mc < cfg::CAL_MIN_ALIGNMENT) {
    snprintf(r.message, sizeof(r.message),
             "Round %d: opening and closing were not the same rotation in opposite directions "
             "(alignment %.2f / %.2f, need %.2f). The sensor may be loose, or the door was lifted or twisted.",
             round, po / mo, pc / mc, cfg::CAL_MIN_ALIGNMENT);
    return r;
  }
  float mismatch = fabsf(po - pc);
  float allowed = allowedMismatch(cfg::CAL_MAX_CLOSURE_ABS_DEG, cfg::CAL_MAX_CLOSURE_REL, 0.5f * (po + pc));
  if (mismatch > allowed) {
    snprintf(r.message, sizeof(r.message),
             "Round %d: measured %.1f deg opening but %.1f deg closing (mismatch %.1f deg, limit %.1f). "
             "The gyro drifted, the door was not fully closed, or it moved when you pressed the button.",
             round, po, pc, mismatch, allowed);
    return r;
  }
  r.ok = true;
  r.axis = axis;
  r.openAngleDeg = 0.5f * (po + pc);
  snprintf(r.message, sizeof(r.message), "Round %d OK: open %.1f deg, close %.1f deg.", round, po, pc);
  return r;
}

// All four movements: open, close, open, close.
inline CalibrationResult validateCalibration(const Vec3 moves[4]) {
  CalibrationResult r1 = validateRound(moves[0], moves[1], 1);
  if (!r1.ok) return r1;
  CalibrationResult r2 = validateRound(moves[2], moves[3], 2);
  if (!r2.ok) return r2;

  CalibrationResult r;
  float axisAgreement = dot(r1.axis, r2.axis);
  if (axisAgreement < 0.95f) {
    snprintf(r.message, sizeof(r.message),
             "The two rounds rotated about different axes (agreement %.2f, need 0.95). "
             "Use the same open position and keep the sensor fixed to the door.",
             axisAgreement);
    return r;
  }
  Vec3 axis = normalized((moves[0] - moves[1]) + (moves[2] - moves[3]));
  float p[4] = {dot(moves[0], axis), -dot(moves[1], axis), dot(moves[2], axis), -dot(moves[3], axis)};
  float mean = 0.25f * (p[0] + p[1] + p[2] + p[3]);
  float openDiff = fabsf(p[0] - p[2]);
  float allowed = allowedMismatch(cfg::CAL_MAX_REPEAT_ABS_DEG, cfg::CAL_MAX_REPEAT_REL, mean);
  if (openDiff > allowed) {
    snprintf(r.message, sizeof(r.message),
             "Round 1 opened %.1f deg but round 2 opened %.1f deg (difference %.1f, limit %.1f). "
             "Open to the same position each time.",
             p[0], p[2], openDiff, allowed);
    return r;
  }
  r.ok = true;
  r.axis = axis;
  r.openAngleDeg = mean;
  snprintf(r.message, sizeof(r.message), "Calibration OK: typical opening %.1f deg.", mean);
  return r;
}

// Collects the four button-confirmed movements, validating each round as
// soon as it completes so the user gets a precise retry prompt.
//
// A button press only STARTS a capture. The position is saved once the
// sensor has settled (see SettleGate); the rotation integrated meanwhile is
// part of the saved movement.
class CalibrationSession {
 public:
  enum class Outcome : uint8_t { Pending, SettleTimeout, Saved, RoundFailed, Failed, Complete, RestartAccepted };

  CalibrationSession() : gate_(cfg::SETTLE_MS, cfg::SETTLE_TIMEOUT_MS), rotAtPress_(), settleMotion_(0) { begin(); }
  void begin() {
    stage_ = 0;
    awaitRestart_ = false;
    result_ = CalibrationResult();
    gate_.cancel();
  }
  int round() const { return stage_ / 2 + 1; }
  bool expectingOpen() const { return stage_ % 2 == 0; }
  bool awaitingRestart() const { return awaitRestart_; }
  bool capturing() const { return gate_.active(); }
  const CalibrationResult& result() const { return result_; }
  float settleMotionDeg() const { return settleMotion_; }  // rotation during the last settle period

  bool requestCapture(uint32_t nowMs, const Vec3& rotationNow) {
    if (!gate_.begin(nowMs)) return false;
    rotAtPress_ = rotationNow;
    return true;
  }

  // Call regularly while capturing(). 'rotation' = rotation integrated since
  // the last accepted capture. The caller must reset its accumulator for every
  // outcome except Pending and SettleTimeout.
  Outcome service(uint32_t nowMs, bool settled, const Vec3& rotation) {
    switch (gate_.service(nowMs, settled)) {
      case SettleGate::Result::Confirmed:
        settleMotion_ = magnitude(rotation - rotAtPress_);
        return accept(rotation);
      case SettleGate::Result::TimedOut: return Outcome::SettleTimeout;
      default: return Outcome::Pending;
    }
  }

  // Record one confirmed position (also usable directly in tests).
  Outcome accept(const Vec3& rotation) {
    if (awaitRestart_) {
      awaitRestart_ = false;
      return Outcome::RestartAccepted;
    }
    moves_[stage_++] = rotation;
    if (stage_ == 2 || stage_ == 4) {
      int rnd = stage_ / 2;
      CalibrationResult r = validateRound(moves_[2 * rnd - 2], moves_[2 * rnd - 1], rnd);
      if (!r.ok) {
        result_ = r;
        stage_ = 2 * (rnd - 1);
        awaitRestart_ = true;
        return Outcome::RoundFailed;
      }
    }
    if (stage_ == 4) {
      result_ = validateCalibration(moves_);
      if (!result_.ok) {
        stage_ = 0;
        awaitRestart_ = true;
        return Outcome::Failed;
      }
      return Outcome::Complete;
    }
    return Outcome::Saved;
  }

 private:
  Vec3 moves_[4];
  int stage_;
  bool awaitRestart_;
  CalibrationResult result_;
  SettleGate gate_;
  Vec3 rotAtPress_;
  float settleMotion_;
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
    still_.reset();
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
      still_.reset();
      resetBlock();
      return r;
    }

    Vec3 corrected = raw - bias_;
    filt_ = hasFilt_ ? filt_ + (corrected - filt_) * cfg::RATE_EMA_ALPHA : corrected;
    hasFilt_ = true;
    still_.push(corrected);  // unfiltered: judge real variability

    if (!calibrated_) {
      rotation_ = rotation_ + filt_ * dt;
    } else {
      angle_ += dot(filt_, axis_) * dt;
    }
    adaptBias(raw, dt, r);
    if (calibrated_) updateState(r);
    return r;
  }

  void markSensorLost() {
    if (calibrated_) quality_ = AngleQuality::Degraded;
    hasFilt_ = false;
    still_.reset();
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
  // Steady enough to confirm a position: the last CAPTURE_WINDOW_MS of
  // bias-corrected samples have low variability AND a small mean rotation
  // rate (so a slow continuing turn is not mistaken for "still").
  bool settled() const {
    Vec3 m, sd;
    if (!still_.stats(cfg::CAPTURE_N, m, sd)) return false;
    return maxAbsAxis(sd) <= cfg::STILL_MAX_STD_DPS && magnitude(m) <= cfg::CAPTURE_MAX_MEAN_DPS;
  }
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
        if (calibrated_) angle_ -= dot(delta, axis_) * block_.t;
        else rotation_ = rotation_ - delta * block_.t;
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
  StillnessWindow still_;
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
