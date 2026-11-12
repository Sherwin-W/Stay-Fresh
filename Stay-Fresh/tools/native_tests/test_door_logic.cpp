// Host-side tests for include/door_logic.h. No hardware, no PlatformIO needed.
//   ./tools/native_tests/run.sh
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "door_logic.h"

using namespace sf;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, ...)                                         \
  do {                                                           \
    g_checks++;                                                  \
    if (!(cond)) {                                               \
      g_fails++;                                                 \
      printf("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);   \
      printf("" __VA_ARGS__);                                    \
      printf("\n");                                              \
    }                                                            \
  } while (0)

// ---------- deterministic noise ----------
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed) {}
  float uni() { s = s * 1664525u + 1013904223u; return ((s >> 8) & 0xFFFFFF) / 16777216.0f; }
  float gauss() {
    float u1 = uni() + 1e-7f, u2 = uni();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
  }
};

// Simulated sensor glued to a door that rotates about 'axis' (body frame).
struct Sim {
  Rng rng;
  Vec3 axis, trueBias;
  float noise;
  float trueAngle;
  float t;  // seconds
  Sim(uint32_t seed, Vec3 a, Vec3 b, float n) : rng(seed), axis(normalized(a)), trueBias(b), noise(n), trueAngle(0), t(0) {}
  Vec3 sample(float rateDps) {
    return axis * rateDps + trueBias + Vec3(noise * rng.gauss(), noise * rng.gauss(), noise * rng.gauss());
  }
};

// Feed the estimator for 'seconds' at a constant true door rate.
static void run(Sim& s, DoorEstimator& e, float seconds, float rate, float dt = 0.02f) {
  int n = (int)(seconds / dt + 0.5f);
  for (int i = 0; i < n; i++) {
    e.step(s.sample(rate), dt);
    s.trueAngle += rate * dt;
    s.t += dt;
  }
}
static void moveTo(Sim& s, DoorEstimator& e, float target, float seconds) {
  run(s, e, seconds, (target - s.trueAngle) / seconds);
}
static void hold(Sim& s, DoorEstimator& e, float seconds) { run(s, e, seconds, 0); }

static Vec3 axisA() { return Vec3(0.3f, 0.2f, 0.93f); }

// ------------------------------------------------------------------
static void test_bias_average() {
  printf("bias average (simple)\n");
  const Vec3 trueBias(1.2f, -0.7f, 0.4f);
  {  // 100 samples, noisy: done at exactly sample 100
    Sim s(1, axisA(), trueBias, 0.08f);
    BiasAverager b;
    int n = 0;
    bool done = false;
    while (!done && n < 1000) { done = b.add(s.sample(0)); n++; }
    CHECK(done && n == cfg::BIAS_SAMPLES, "n=%d", n);
    CHECK(magnitude(b.bias() - trueBias) < 0.05f, "err=%.3f", magnitude(b.bias() - trueBias));
    CHECK(!b.noisy());
  }
  {  // hand tremor and one bump: no stillness requirement, no restart, still finishes at 100 samples
    Sim s(2, axisA(), trueBias, 0.08f);
    BiasAverager b;
    int n = 0;
    bool done = false;
    while (!done && n < 1000) {
      float t = n * 0.02f;
      Vec3 g = s.sample(0) + Vec3(1.5f * sinf(6.2832f * 8 * t), 1.5f * sinf(6.2832f * 8 * t + 2), 1.5f * sinf(6.2832f * 8 * t + 4));
      if (n == 40) g = g + Vec3(30, 30, 30);  // bump
      done = b.add(g);
      n++;
    }
    CHECK(done && n == cfg::BIAS_SAMPLES, "n=%d", n);
    CHECK(b.noisy(), "informational flag only");
    CHECK(magnitude(b.bias() - trueBias) < 1.0f, "err=%.3f (a plain average is only approximately right)", magnitude(b.bias() - trueBias));
  }
}

static void test_calibration_validation() {
  printf("calibration validation (basic checks)\n");
  Vec3 ax = normalized(axisA());
  {  // the field log that the stricter checks rejected is accepted again (original behaviour)
    Vec3 m[4] = {ax * 64.7f, ax * -47.1f, ax * 84.4f, ax * -71.9f};
    CalibrationResult r = validateCalibration(m);
    CHECK(r.ok, "%s", r.message);
    CHECK(fabsf(r.openAngleDeg - 67.0f) < 0.1f);
    CHECK(dot(r.axis, ax) > 0.999f);
  }
  {  // negligible movement
    Vec3 m[4] = {ax * 5.0f, ax * -5.0f, ax * 5.0f, ax * -5.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(!r.ok);
    printf("  negligible -> %s\n", r.message);
  }
  {  // one tiny movement among good ones
    Vec3 m[4] = {ax * 60.0f, ax * -60.0f, ax * 60.0f, ax * -4.0f};
    CHECK(!validateCalibration(m).ok);
  }
  {  // "close" turns the same way as "open"
    Vec3 m[4] = {ax * 60.0f, ax * 60.0f, ax * 60.0f, ax * 60.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(!r.ok);
    printf("  same direction -> %s\n", r.message);
  }
  {  // different axes
    Vec3 other = normalized(Vec3(1.0f, -0.2f, -0.3f));  // ~90 deg from ax
    Vec3 m[4] = {ax * 60.0f, ax * -60.0f, other * 60.0f, other * -60.0f};
    CHECK(!validateCalibration(m).ok);
  }
  {  // sizes more than 40 % from the average (original rule)
    Vec3 m[4] = {ax * 30.0f, ax * -30.0f, ax * 100.0f, ax * -100.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(!r.ok);
    printf("  inconsistent sizes -> %s\n", r.message);
  }
  {  // opposite-sign (door opens the other way round) is fine: the axis flips so "opening" is positive
    Vec3 m[4] = {ax * -60.0f, ax * 60.0f, ax * -60.0f, ax * 60.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(r.ok && dot(r.axis, ax) < -0.999f);
  }
}

static void test_calibration_session() {
  printf("calibration session (immediate button presses)\n");
  Vec3 ax = normalized(axisA());
  CalibrationSession c;
  CHECK(c.round() == 1 && c.expectingOpen());
  CHECK(c.press(ax * 64.7f) == CalibrationSession::Outcome::Saved);
  CHECK(c.round() == 1 && !c.expectingOpen());
  CHECK(c.press(ax * -47.1f) == CalibrationSession::Outcome::Saved);
  CHECK(c.round() == 2 && c.expectingOpen());
  CHECK(c.press(ax * 84.4f) == CalibrationSession::Outcome::Saved);
  CHECK(c.press(ax * -71.9f) == CalibrationSession::Outcome::Complete, "four presses, then done");
  CHECK(c.result().ok);

  CalibrationSession d;  // inconsistent: starts the four presses again, nothing else gates it
  d.press(ax * 60.0f);
  d.press(ax * 60.0f);
  d.press(ax * 60.0f);
  CHECK(d.press(ax * 60.0f) == CalibrationSession::Outcome::Failed);
  CHECK(d.round() == 1 && d.expectingOpen());
  printf("  message: %s\n", d.result().message);
  d.press(ax * 60.0f);
  d.press(ax * -58.0f);
  d.press(ax * 62.0f);
  CHECK(d.press(ax * -61.0f) == CalibrationSession::Outcome::Complete);
}

// Whole workflow on simulated data: bias average, 4 presses, angle = 0, monitoring, manual reset.
static void test_full_calibration_flow() {
  printf("full calibration flow (simulated)\n");
  Vec3 trueBias(0.7f, -0.4f, 0.3f);
  Sim s(60, axisA(), trueBias, 0.08f);
  DoorEstimator e;
  BiasAverager avg;
  while (!avg.add(s.sample(0))) {}
  e.setBias(avg.bias());

  CalibrationSession c;
  CalibrationSession::Outcome o = CalibrationSession::Outcome::Saved;
  const float targets[4] = {70, 0, 72, 0};
  for (int i = 0; i < 4; i++) {
    moveTo(s, e, targets[i], 1.5f);
    hold(s, e, 1.0f);
    Vec3 rot = e.rotation();
    e.resetRotation();
    o = c.press(rot);  // the press is accepted immediately
  }
  CHECK(o == CalibrationSession::Outcome::Complete, "%s", c.result().message);
  CHECK(fabsf(c.result().openAngleDeg - 71.0f) < 3.0f, "open angle %.1f", c.result().openAngleDeg);
  e.setCalibration(c.result().axis, c.result().openAngleDeg);
  e.zero(ClosureSource::Manual);
  CHECK(e.angle() == 0 && e.estimatedClosed() && e.closureSource() == ClosureSource::Manual);
  CHECK(e.biasAdjustments() == 0, "no bias refinement during calibration");

  // monitoring works with the learned axis
  s.trueAngle = 0;
  moveTo(s, e, 60, 2.0f);
  hold(s, e, 1.0f);
  CHECK(!e.estimatedClosed() && fabsf(e.angle() - 60.0f) < 3.0f, "angle=%.1f", e.angle());

  // Button 1 after calibration: manual reset of the closed reference AND the alarm
  AlarmTimer alarm(10000);
  uint32_t ms = 0;
  alarm.update(ms, DoorState::Open);
  CHECK(alarm.update(ms + 10000, DoorState::Open) == AlarmTimer::Event::Started);
  e.zero(ClosureSource::Manual);
  DoorInputs in = {false, false, false, true, e.calibrated(), e.estimatedClosed()};
  CHECK(e.angle() == 0 && resolveDoorState(in) == DoorState::Closed);
  CHECK(alarm.update(ms + 10020, resolveDoorState(in)) == AlarmTimer::Event::Stopped);
  CHECK(!alarm.active() && !alarm.running());
}

static void test_no_adaptation_before_calibration() {
  printf("calibration phase is plain integration\n");
  Sim s(70, axisA(), Vec3(0, 0, 0), 0.05f);
  DoorEstimator e;
  e.setBias(Vec3(0, 0, 0));
  run(s, e, 10.0f, 0, 0.02f);  // 10 s at rest with a true 0 offset
  CHECK(e.biasAdjustments() == 0);
  DoorEstimator f;
  f.setBias(Vec3(0, 0, 0));
  Sim t(71, axisA(), Vec3(1.0f, 0, 0), 0.05f);  // uncorrected 1 dps offset on X
  run(t, f, 10.0f, 0, 0.02f);
  CHECK(f.biasAdjustments() == 0, "no hidden bias adjustment while calibrating");
  CHECK(fabsf(f.rotation().x - 10.0f) < 0.5f, "rotation.x=%.2f (pure integral)", f.rotation().x);
}

static void test_alarm() {
  printf("alarm transitions\n");
  AlarmTimer a(10000);
  uint32_t t = 1000;
  CHECK(a.update(t, DoorState::Closed) == AlarmTimer::Event::None);
  CHECK(a.update(t, DoorState::Open) == AlarmTimer::Event::None);  // timer starts
  CHECK(a.running() && !a.active());
  CHECK(a.update(t + 9999, DoorState::Open) == AlarmTimer::Event::None);
  CHECK(a.update(t + 10000, DoorState::Open) == AlarmTimer::Event::Started);
  CHECK(a.active());
  CHECK(a.update(t + 10020, DoorState::Open) == AlarmTimer::Event::None, "BUZZ must fire once");
  CHECK(a.update(t + 30000, DoorState::Open) == AlarmTimer::Event::None);
  CHECK(a.update(t + 30020, DoorState::Closed) == AlarmTimer::Event::Stopped);
  CHECK(!a.active() && !a.running() && a.openMs(t + 30020) == 0);

  // Timer restarts from zero after closing
  CHECK(a.update(t + 40000, DoorState::Open) == AlarmTimer::Event::None);
  CHECK(a.update(t + 49999, DoorState::Open) == AlarmTimer::Event::None);
  CHECK(a.update(t + 50000, DoorState::Open) == AlarmTimer::Event::Started);

  // Unknown never starts an alarm and never resets one
  AlarmTimer u(10000);
  for (uint32_t i = 0; i < 40000; i += 100) CHECK(u.update(i, DoorState::Unknown) == AlarmTimer::Event::None);
  CHECK(!u.running() && !u.active());
  AlarmTimer v(10000);
  v.update(0, DoorState::Open);
  v.update(5000, DoorState::Unknown);  // sensor lost mid-open: timer keeps running
  CHECK(v.update(10000, DoorState::Unknown) == AlarmTimer::Event::None);
  CHECK(v.update(10100, DoorState::Open) == AlarmTimer::Event::Started);
  CHECK(v.update(20000, DoorState::Unknown) == AlarmTimer::Event::None && v.active(), "alarm persists while unknown");

  // millis() wrap-around
  AlarmTimer w(10000);
  uint32_t near = 0xFFFFFFFFu - 4000u;
  w.update(near, DoorState::Open);
  CHECK(w.update(near + 9999u, DoorState::Open) == AlarmTimer::Event::None);
  CHECK(w.update(near + 10000u, DoorState::Open) == AlarmTimer::Event::Started);
}

static void test_door_state_resolution() {
  printf("door state resolution\n");
  DoorInputs i = {false, false, false, true, true, true};
  CHECK(resolveDoorState(i) == DoorState::Closed);
  i.estimatedClosed = false;
  CHECK(resolveDoorState(i) == DoorState::Open);
  i.sensorValid = false;
  CHECK(resolveDoorState(i) == DoorState::Unknown, "invalid sensor must not read closed");
  i.sensorValid = true;
  i.calibrated = false;
  CHECK(resolveDoorState(i) == DoorState::Unknown);
  // Reed mode: contact decides, gyro ignored
  DoorInputs r = {true, true, true, false, false, false};
  CHECK(resolveDoorState(r) == DoorState::Closed);
  r.reedClosed = false;
  CHECK(resolveDoorState(r) == DoorState::Open);
  r.reedKnown = false;
  CHECK(resolveDoorState(r) == DoorState::Unknown);
}

static void test_debouncer() {
  printf("debouncer\n");
  Debouncer d(50);
  d.begin(true, 0);
  CHECK(!d.known());
  d.update(true, 49);
  CHECK(!d.known());
  d.update(true, 50);
  CHECK(d.known() && d.value());
  // contact bounce shorter than 50 ms is ignored
  d.update(false, 100);
  d.update(true, 110);
  d.update(false, 120);
  d.update(true, 130);
  CHECK(d.value());
  d.update(true, 200);
  CHECK(d.value());
  d.update(false, 300);
  CHECK(d.value(), "not yet");
  d.update(false, 350);
  CHECK(!d.value());
}

static void test_estimator_tracking() {
  printf("estimator tracking\n");
  Vec3 trueBias(0.9f, -0.6f, 0.5f);
  {  // slow opening (not discarded), pause, finish; then close
    Sim s(10, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias);
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    moveTo(s, e, 30, 8.0f);     // 3.75 dps: slow
    hold(s, e, 6.0f);           // pause mid-way (3 s adaptation blocks run here)
    CHECK(fabsf(e.angle() - 30) < 2.0f, "angle=%.2f", e.angle());
    CHECK(!e.estimatedClosed());
    moveTo(s, e, 75, 3.0f);
    hold(s, e, 4.0f);
    CHECK(fabsf(e.angle() - 75) < 2.5f, "angle=%.2f", e.angle());
    moveTo(s, e, 0, 3.0f);
    hold(s, e, 3.0f);
    CHECK(e.estimatedClosed(), "angle=%.2f", e.angle());
    CHECK(e.closureSource() == ClosureSource::Estimated);
  }
  {  // holding still does NOT close the door
    Sim s(11, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias);
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    moveTo(s, e, 60, 2.0f);
    hold(s, e, 60.0f);
    CHECK(!e.estimatedClosed() && fabsf(e.angle() - 60) < 3.0f, "angle=%.2f", e.angle());
    CHECK(e.closureSource() == ClosureSource::None);
  }
  {  // 1 dps residual bias (what the field log showed) on a CLOSED door
    Sim s(12, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias - normalized(axisA()) * 1.0f);  // estimate is 1 dps too low along the hinge
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    hold(s, e, 60.0f);
    CHECK(e.biasAdjustments() > 0);
    CHECK(fabsf(e.angle()) < 2.0f && e.estimatedClosed(), "angle=%.2f (would be 60 without adaptation)", e.angle());
  }
  {  // same residual WITHOUT any stationary period long enough: drift remains (honest limit)
    Sim s(13, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias - normalized(axisA()) * 1.0f);
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    // alternate 2 s still / 1 s of tiny wobble so no 3 s quiet block completes
    for (int k = 0; k < 10; k++) {
      hold(s, e, 2.0f);
      run(s, e, 0.5f, 6.0f);
      run(s, e, 0.5f, -6.0f);
    }
    CHECK(e.biasAdjustments() == 0, "blocks interrupted by motion must not adapt");
  }
  {  // real motion must not be absorbed as bias
    Sim s(14, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias);
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    moveTo(s, e, 90, 30.0f);  // 3 dps for 30 s
    CHECK(e.biasAdjustments() == 0, "adjustments=%u", e.biasAdjustments());
    CHECK(fabsf(e.angle() - 90) < 2.5f, "angle=%.2f", e.angle());
  }
  {  // negative drift clamps at the frame stop
    Sim s(15, axisA(), trueBias, 0.08f);
    DoorEstimator e;
    e.setBias(trueBias + normalized(axisA()) * 2.0f);  // bias estimated too HIGH -> negative drift
    e.setCalibration(s.axis, 70);
    e.zero(ClosureSource::Manual);
    // Interrupt quiet blocks so adaptation cannot rescue it
    for (int k = 0; k < 8; k++) {
      hold(s, e, 2.0f);
      run(s, e, 0.2f, 10.0f);
      run(s, e, 0.2f, -10.0f);
    }
    CHECK(e.angle() >= -cfg::NEGATIVE_CLAMP_DEG - 1e-3f && e.negativeClamps() > 0, "angle=%.2f", e.angle());
    CHECK(e.estimatedClosed());
  }
}

static void test_estimator_gaps_and_sources() {
  printf("gaps, quality, closure sources\n");
  Vec3 trueBias(0.2f, 0.1f, -0.3f);
  Sim s(20, axisA(), trueBias, 0.08f);
  DoorEstimator e;
  e.setBias(trueBias);
  e.setCalibration(s.axis, 70);
  CHECK(e.quality() == AngleQuality::None);
  e.zero(ClosureSource::Manual);
  CHECK(e.quality() == AngleQuality::Estimated);
  CHECK(e.closureSource() == ClosureSource::Manual);
  moveTo(s, e, 40, 2.0f);
  CHECK(e.closureSource() == ClosureSource::None);

  uint32_t before = e.missedSamples();
  e.step(s.sample(0), 0.10f);  // 100 ms: ~4 samples lost, still integrated
  CHECK(e.missedSamples() - before >= 3 && e.missedSamples() - before <= 5, "missed=%u", e.missedSamples() - before);
  CHECK(e.quality() == AngleQuality::Estimated);

  float a0 = e.angle();
  e.step(s.sample(50.0f), 1.0f);  // 1 s outage: must not invent 50 degrees
  CHECK(fabsf(e.angle() - a0) < 0.01f);
  CHECK(e.quality() == AngleQuality::Degraded);
  CHECK(e.missedSamples() - before > 40);

  e.zero(ClosureSource::Reed);
  CHECK(e.quality() == AngleQuality::Estimated && e.closureSource() == ClosureSource::Reed);
  CHECK(fabsf(e.angle()) < 1e-6f && e.secondsSinceReference() == 0);
  CHECK(fabsf(e.lastZeroCorrection() - 40.0f) < 4.0f, "reported accumulated error before zero: %.2f", e.lastZeroCorrection());
}

// Full chain: estimator -> door state -> alarm
static void test_end_to_end_alarm() {
  printf("end-to-end alarm\n");
  Vec3 trueBias(0.5f, 0.3f, -0.4f);
  Sim s(30, axisA(), trueBias, 0.08f);
  DoorEstimator e;
  e.setBias(trueBias);
  e.setCalibration(s.axis, 70);
  e.zero(ClosureSource::Manual);
  AlarmTimer alarm(10000);
  uint32_t ms = 0;
  int started = 0, stopped = 0;
  uint32_t startedAt = 0, stoppedAt = 0, closedAt = 0;

  auto tick = [&](float rate, float seconds) {
    int n = (int)(seconds / 0.02f + 0.5f);
    for (int i = 0; i < n; i++) {
      e.step(s.sample(rate), 0.02f);
      s.trueAngle += rate * 0.02f;
      ms += 20;
      DoorInputs in = {false, false, false, true, e.calibrated(), e.estimatedClosed()};
      AlarmTimer::Event ev = alarm.update(ms, resolveDoorState(in));
      if (ev == AlarmTimer::Event::Started) { started++; startedAt = ms; }
      if (ev == AlarmTimer::Event::Stopped) { stopped++; stoppedAt = ms; }
    }
  };
  tick(0, 5);
  uint32_t openStart = ms;
  tick(40.0f, 1.5f);   // open to 60 deg in 1.5 s
  tick(0, 15);
  CHECK(started == 1);
  uint32_t delay = startedAt - openStart;
  CHECK(delay > 10000 && delay < 10600, "alarm after %u ms (threshold crossing + 10 s)", delay);
  uint32_t closeStart = ms;
  tick(-30.0f, 2.0f);  // close
  closedAt = ms;
  tick(0, 1);
  CHECK(stopped == 1 && !alarm.active());
  // The estimate crosses the 5 deg closed tolerance just before the swing ends.
  CHECK(stoppedAt > closeStart && stoppedAt <= closedAt, "alarm stopped %u ms into a 2000 ms close", stoppedAt - closeStart);
  CHECK(e.estimatedClosed());
}

int main() {
  test_bias_average();
  test_calibration_validation();
  test_calibration_session();
  test_full_calibration_flow();
  test_no_adaptation_before_calibration();
  test_alarm();
  test_door_state_resolution();
  test_debouncer();
  test_estimator_tracking();
  test_estimator_gaps_and_sources();
  test_end_to_end_alarm();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
