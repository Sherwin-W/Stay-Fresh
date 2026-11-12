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
// Feed `seconds` of samples; stops early at a terminal status. Returns the last status.
static BiasMeasurement::Status feedBias(Sim& s, BiasMeasurement& b, float& t, float seconds, float rate = 0,
                                        Vec3 offset = Vec3(), float tremorAmp = 0) {
  BiasMeasurement::Status st = BiasMeasurement::Status::Collecting;
  int n = (int)(seconds / 0.02f + 0.5f);
  for (int i = 0; i < n; i++) {
    Vec3 g = s.sample(rate) + offset;
    if (tremorAmp > 0) {  // ~8 Hz hand tremor, different phase per axis
      g = g + Vec3(tremorAmp * sinf(6.2832f * 8 * t), tremorAmp * sinf(6.2832f * 8 * t + 2.0f), tremorAmp * sinf(6.2832f * 8 * t + 4.0f));
    }
    st = b.add(g, 0.02f);
    t += 0.02f;
    if (st == BiasMeasurement::Status::Done || st == BiasMeasurement::Status::TimedOut) break;
  }
  return st;
}
using BS = BiasMeasurement::Status;

static void test_trimmed_mean() {
  printf("trimmed mean\n");
  float a[105];
  for (int i = 0; i < 100; i++) a[i] = 1.0f + 0.001f * (i % 5);
  for (int i = 100; i < 105; i++) a[i] = 100.0f;  // 5 outliers
  float plain = 0;
  for (int i = 0; i < 105; i++) plain += a[i];
  plain /= 105;
  float tm = trimmedMean(a, 105, 0.10f);
  CHECK(plain > 5.0f && fabsf(tm - 1.002f) < 0.01f, "plain=%.2f trimmed=%.4f", plain, tm);
  float one[1] = {3.5f};
  CHECK(trimmedMean(one, 1, 0.10f) == 3.5f);
}

static void test_bias_measurement() {
  printf("bias measurement (rolling window)\n");
  const Vec3 trueBias(1.2f, -0.7f, 0.4f);
  {  // quiet sensor: ~3 s warm-up + 5 s accepted
    Sim s(1, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    BS st = feedBias(s, b, t, 20);
    CHECK(st == BS::Done, "%s", b.reason());
    CHECK(magnitude(b.bias() - trueBias) < 0.05f, "err=%.3f", magnitude(b.bias() - trueBias));
    CHECK(t > 7.9f && t < 9.0f, "took %.1f s", t);
    CHECK(b.restarts() == 0 && b.pauses() == 0);
  }
  {  // hand tremor (0.85 dps rms per axis) is tolerated and averages out
    Sim s(2, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    BS st = feedBias(s, b, t, 25, 0, Vec3(), 1.2f);
    CHECK(st == BS::Done, "tremor must be tolerated: %s", b.reason());
    CHECK(magnitude(b.bias() - trueBias) < 0.15f, "err=%.3f", magnitude(b.bias() - trueBias));
  }
  {  // brief bumps (button press): pause, keep accepted samples, resume. NO restart.
    Sim s(3, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    feedBias(s, b, t, 6.0f);                         // 3 s of accepted samples so far
    CHECK(b.percent() > 40 && b.percent() < 80, "percent=%d", b.percent());
    int before = b.samples();
    BS st = feedBias(s, b, t, 0.1f, 25.0f);          // 100 ms bump at 25 dps
    CHECK(st == BS::Paused || st == BS::Collecting);
    st = feedBias(s, b, t, 0.2f);
    CHECK(st == BS::Paused, "must pause while the bump is inside the window");
    CHECK(b.samples() >= before, "accepted samples must be kept");
    feedBias(s, b, t, 2.0f);
    st = feedBias(s, b, t, 0.2f, 15.0f);             // a second, rougher bump
    st = feedBias(s, b, t, 10.0f);
    CHECK(st == BS::Done, "%s", b.reason());
    CHECK(b.restarts() == 0 && b.pauses() >= 2, "restarts=%u pauses=%u", b.restarts(), b.pauses());
    CHECK(t < 15.0f, "finished at %.1f s (a full restart would take far longer)", t);
    CHECK(magnitude(b.bias() - trueBias) < 0.08f, "bump samples must not pollute: err=%.3f", magnitude(b.bias() - trueBias));
  }
  {  // deliberate rotation the whole time: must NOT be calibrated; times out with an explanation
    Sim s(4, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    BS st = feedBias(s, b, t, 40, 15.0f);
    CHECK(st == BS::TimedOut, "rotation must be rejected");
    CHECK(t > 29.0f && t < 31.0f, "gave up at %.1f s", t);
    printf("  reason: %s\n", b.reason());
    CHECK(strstr(b.reason(), "turning") != nullptr, "explains the cause");
  }
  {  // door swung back and forth (high variability): rejected, with the variability explanation
    Sim s(41, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    BS st = BS::Collecting;
    for (int i = 0; i < 2000 && st != BS::TimedOut && st != BS::Done; i++) {
      st = b.add(s.sample(20.0f * sinf(6.2832f * 0.5f * t)), 0.02f);
      t += 0.02f;
    }
    CHECK(st == BS::TimedOut);
    printf("  reason: %s\n", b.reason());
    CHECK(strstr(b.reason(), "steady enough") != nullptr);
  }
  {  // rotation that starts AND stops: pause, keep old samples, no pollution
    Sim s(5, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    feedBias(s, b, t, 6.0f);                          // 3 s accepted
    feedBias(s, b, t, 5.0f, 3.0f);                    // slow 3 dps turn: steady-looking, but shifted
    BS st = feedBias(s, b, t, 12.0f);                 // door at rest again
    CHECK(st == BS::Done, "%s", b.reason());
    CHECK(b.restarts() == 0, "restarts=%u", b.restarts());
    CHECK(magnitude(b.bias() - trueBias) < 0.1f, "slow turn leaked into the offset: err=%.3f", magnitude(b.bias() - trueBias));
  }
  {  // the offset itself steps and stays (warming up): the pool restarts, new offset adopted
    Sim s(6, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    feedBias(s, b, t, 6.0f);
    Vec3 step(1.0f, 0, 0);
    BS st = feedBias(s, b, t, 23.0f, 0, step);
    CHECK(st == BS::Done, "%s", b.reason());
    CHECK(b.restarts() == 1, "restarts=%u", b.restarts());
    CHECK(magnitude(b.bias() - (trueBias + step)) < 0.1f, "err=%.3f", magnitude(b.bias() - (trueBias + step)));
  }
  {  // HONEST LIMITATION: constant slow rotation from the first sample is indistinguishable from bias
    Sim s(7, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    float t = 0;
    BS st = feedBias(s, b, t, 20, 2.0f);
    CHECK(st == BS::Done);
    CHECK(fabsf(magnitude(b.bias() - trueBias) - 2.0f) < 0.2f, "documented limitation: constant 2 dps was taken as offset");
  }
  {  // sensor values missing (every 4th sample): not enough accepted time -> timeout, not silent acceptance
    Sim s(8, axisA(), trueBias, 0.08f);
    BiasMeasurement b;
    BS st = BS::Collecting;
    for (int i = 0; i < 2000 && st != BS::Done && st != BS::TimedOut; i++) {
      Vec3 g = s.sample(0);
      // each sample arrives 300 ms late: window never contiguous
      st = b.add(g, 0.3f);
    }
    CHECK(st == BS::TimedOut, "non-contiguous data must not be accepted");
  }
}

static void test_calibration_validation() {
  printf("calibration validation\n");
  Vec3 ax = normalized(axisA());
  // The exact numbers from the field log: 64.7 / 47.1 / 84.4 / 71.9
  {
    Vec3 m[4] = {ax * 64.7f, ax * -47.1f, ax * 84.4f, ax * -71.9f};
    CalibrationResult r = validateCalibration(m);
    CHECK(!r.ok, "old log data must now be rejected");
    printf("  field data -> %s\n", r.message);
  }
  {  // consistent small drift is accepted
    Vec3 m[4] = {ax * 80.0f, ax * -78.5f, ax * 81.0f, ax * -80.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(r.ok, "%s", r.message);
    CHECK(fabsf(r.openAngleDeg - 79.9f) < 1.0f);
    CHECK(dot(r.axis, ax) > 0.999f);
  }
  {  // rounds differ too much
    Vec3 m[4] = {ax * 60.0f, ax * -59.0f, ax * 80.0f, ax * -79.0f};
    CalibrationResult r = validateCalibration(m);
    CHECK(!r.ok);
    printf("  repeatability -> %s\n", r.message);
  }
  {  // too small a movement
    Vec3 m[4] = {ax * 10.0f, ax * -10.0f, ax * 10.0f, ax * -10.0f};
    CHECK(!validateCalibration(m).ok);
  }
  {  // opening and closing about different axes (loose sensor / twist)
    Vec3 other = normalized(Vec3(0.9f, -0.3f, 0.1f));
    Vec3 m[4] = {ax * 60.0f, other * -60.0f, ax * 60.0f, other * -60.0f};
    CHECK(!validateCalibration(m).ok);
  }
  {  // "close" in the same direction as "open"
    Vec3 m[4] = {ax * 60.0f, ax * 60.0f, ax * 60.0f, ax * 60.0f};
    CHECK(!validateCalibration(m).ok);
  }
}

static void test_calibration_session() {
  printf("calibration session\n");
  Vec3 ax = normalized(axisA());
  CalibrationSession c;
  CHECK(c.round() == 1 && c.expectingOpen());
  CHECK(c.accept(ax * 64.7f) == CalibrationSession::Outcome::Saved);
  CHECK(!c.expectingOpen());
  // round 1 closes with 17.6 deg drift -> round rejected immediately
  CHECK(c.accept(ax * -47.1f) == CalibrationSession::Outcome::RoundFailed);
  CHECK(c.awaitingRestart() && c.round() == 1);
  printf("  prompt: %s\n", c.result().message);
  CHECK(c.accept(Vec3()) == CalibrationSession::Outcome::RestartAccepted);
  CHECK(c.accept(ax * 70.0f) == CalibrationSession::Outcome::Saved);
  CHECK(c.accept(ax * -69.0f) == CalibrationSession::Outcome::Saved);
  CHECK(c.round() == 2);
  CHECK(c.accept(ax * 71.0f) == CalibrationSession::Outcome::Saved);
  CHECK(c.accept(ax * -70.0f) == CalibrationSession::Outcome::Complete);
  CHECK(c.result().ok);

  CalibrationSession d;  // repeatability failure restarts at round 1
  d.accept(ax * 60.0f);
  d.accept(ax * -59.0f);
  d.accept(ax * 85.0f);
  CHECK(d.accept(ax * -84.0f) == CalibrationSession::Outcome::Failed);
  CHECK(d.round() == 1 && d.awaitingRestart());
}

static void test_settle_gate() {
  printf("settle gate and capture\n");
  SettleGate g(500, 4000);
  CHECK(g.service(0, true) == SettleGate::Result::Idle);
  CHECK(g.begin(1000) && !g.begin(1100));
  CHECK(g.service(1100, true) == SettleGate::Result::Pending, "never confirm before the minimum settle time");
  CHECK(g.service(1499, true) == SettleGate::Result::Pending);
  CHECK(g.service(1600, false) == SettleGate::Result::Pending);
  CHECK(g.service(1700, true) == SettleGate::Result::Confirmed);
  CHECK(!g.active());
  g.begin(0);
  CHECK(g.service(3999, false) == SettleGate::Result::Pending);
  CHECK(g.service(4000, false) == SettleGate::Result::TimedOut);
  g.begin(10);
  g.cancel();
  CHECK(!g.active());

  // CalibrationSession: capture waits, includes motion during settling, timeout keeps state.
  Vec3 ax = normalized(axisA());
  CalibrationSession c;
  CHECK(c.requestCapture(1000, ax * 60.0f));
  CHECK(!c.requestCapture(1010, ax * 60.0f));
  CHECK(c.service(1200, true, ax * 60.0f) == CalibrationSession::Outcome::Pending);
  CHECK(c.service(1600, true, ax * 62.0f) == CalibrationSession::Outcome::Saved);
  CHECK(fabsf(c.settleMotionDeg() - 2.0f) < 0.01f);
  CHECK(!c.capturing() && !c.expectingOpen());
  CHECK(c.requestCapture(2000, ax * -60.0f));
  CHECK(c.service(6000, false, ax * -60.0f) == CalibrationSession::Outcome::SettleTimeout);
  CHECK(!c.capturing() && !c.expectingOpen(), "timeout must not consume the step");
  CHECK(c.requestCapture(7000, ax * -60.0f));
}

// Estimator: tolerant of tremor and a button bump, but not of a slow steady turn.
static void test_settled_detection() {
  printf("settled detection\n");
  Vec3 trueBias(0.3f, -0.2f, 0.1f);
  Sim s(40, axisA(), trueBias, 0.08f);
  DoorEstimator e;
  e.setBias(trueBias);
  float t = 0;
  auto step = [&](float rate, float seconds, float tremor) {
    int n = (int)(seconds / 0.02f + 0.5f);
    for (int i = 0; i < n; i++) {
      Vec3 g = s.sample(rate) + Vec3(tremor * sinf(6.2832f * 8 * t), tremor * sinf(6.2832f * 8 * t + 2), tremor * sinf(6.2832f * 8 * t + 4));
      e.step(g, 0.02f);
      t += 0.02f;
    }
  };
  step(0, 0.3f, 1.0f);
  CHECK(!e.settled(), "not enough data yet");
  step(0, 0.5f, 1.0f);
  CHECK(e.settled(), "tremor of ~0.7 dps rms must count as settled");
  step(30.0f, 0.06f, 0);  // button bump
  CHECK(!e.settled());
  step(0, 0.3f, 0.5f);
  CHECK(!e.settled(), "bump still inside the 0.5 s window");
  step(0, 0.4f, 0.5f);
  CHECK(e.settled(), "settles after the bump leaves the window");
  step(4.0f, 1.0f, 0);   // slow steady turn: low variance but real rotation
  CHECK(!e.settled(), "slow steady rotation must NOT count as still");
}

// Whole flow on simulated data: open, press (with a bump), settle, saved with correct angle.
static void test_capture_with_button_bump() {
  printf("capture with button bump\n");
  Vec3 trueBias(0.4f, 0.2f, -0.3f);
  Sim s(50, axisA(), trueBias, 0.08f);
  DoorEstimator e;
  e.setBias(trueBias);
  CalibrationSession c;
  uint32_t ms = 0;
  auto tick = [&](float rate, Vec3 extra = Vec3()) {
    e.step(s.sample(rate) + extra, 0.02f);
    ms += 20;
  };
  for (int i = 0; i < 50; i++) tick(0);
  for (int i = 0; i < 75; i++) tick(40.0f);            // open 60 deg in 1.5 s
  for (int i = 0; i < 25; i++) tick(0);
  CHECK(c.requestCapture(ms, e.rotation()));
  uint32_t pressAt = ms;
  // the press bumps the sensor: +/-25 dps for ~60 ms, and a little real drift of the door
  for (int i = 0; i < 3; i++) tick(0, Vec3(25, 25, 25));
  for (int i = 0; i < 3; i++) tick(0, Vec3(-25, -25, -25));
  CalibrationSession::Outcome o = CalibrationSession::Outcome::Pending;
  uint32_t doneAt = 0;
  for (int i = 0; i < 200 && o == CalibrationSession::Outcome::Pending; i++) {
    tick(0);
    o = c.service(ms, e.settled(), e.rotation());
    if (o != CalibrationSession::Outcome::Pending) doneAt = ms;
  }
  CHECK(o == CalibrationSession::Outcome::Saved);
  CHECK(doneAt - pressAt >= cfg::SETTLE_MS && doneAt - pressAt < 1500, "confirmed after %u ms", doneAt - pressAt);
  float measured = magnitude(e.rotation());
  CHECK(fabsf(measured - 60.0f) < 2.0f, "endpoint = %.2f deg (bump must not contaminate)", measured);
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
  test_trimmed_mean();
  test_bias_measurement();
  test_calibration_validation();
  test_calibration_session();
  test_settle_gate();
  test_settled_detection();
  test_capture_with_button_bump();
  test_alarm();
  test_door_state_resolution();
  test_debouncer();
  test_estimator_tracking();
  test_estimator_gaps_and_sources();
  test_end_to_end_alarm();
  printf("\n%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
