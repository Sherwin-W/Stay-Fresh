// Stay-Fresh: ESP32 fridge-door monitor.
//
// This file is hardware glue only (I2C, button, buzzer, serial, cloud
// snapshots). All decisions live in include/door_logic.h, which is unit
// tested on a laptop: see tools/native_tests/run.sh.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "door_logic.h"
#include "cloud_upload.h"

using namespace sf;

constexpr uint8_t MPU_ADDR = 0x68;

// ------------------------------------------------------------
// State
// ------------------------------------------------------------
Adafruit_BMP280 bmp;
bool bmpReady = false;
bool mpuReady = false;

BiasMeasurement biasMeasurement;
DoorEstimator door;
CalibrationSession calibration;
AlarmTimer alarmTimer(cfg::DOOR_OPEN_LIMIT_MS);
Debouncer buttonDebounce(cfg::BUTTON_DEBOUNCE_MS);
Debouncer reedDebounce(cfg::REED_DEBOUNCE_MS);

CalState calState = CalState::Fault;
DoorState doorState = DoorState::Unknown;
bool sensorValid = false;

// Gyro sample timing: real timestamps, not an assumed 20 ms.
uint32_t nextSampleUs = 0;
uint32_t lastGoodUs = 0;
uint32_t lastGoodMs = 0;
bool haveGoodSample = false;
uint32_t failedReads = 0;
uint32_t lastReinitMs = 0;

uint32_t lastPrintMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t telemetrySeq = 0;

bool lastButtonPressed = false;
bool lastReedClosed = false;

// ------------------------------------------------------------
// Buzzer (explicit LEDC channel; tone()/noTone() caused
// "LEDC is not initialized" on Arduino core 2.x)
// ------------------------------------------------------------
void buzzerOn() { ledcWriteTone(cfg::BUZZER_CHANNEL, 1000); }
void buzzerOff() { ledcWrite(cfg::BUZZER_CHANNEL, 0); }

// ------------------------------------------------------------
// MPU6050
// ------------------------------------------------------------
bool writeMpu(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool configureMpu() {
  bool ok = writeMpu(0x6B, 0x00);        // wake
  ok = ok && writeMpu(0x1A, 0x03);       // low-pass filter (~44 Hz)
  ok = ok && writeMpu(0x1B, 0x00);       // gyro range +/-250 deg/s
  return ok;
}

bool readGyro(Vec3& g) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43);  // GYRO_XOUT_H
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6) return false;

  uint8_t raw[6];
  for (int i = 0; i < 6; i++) raw[i] = Wire.read();

  int16_t x = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
  int16_t y = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
  int16_t z = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);

  // 131 LSB per deg/s on the +/-250 deg/s range.
  g = Vec3(x / 131.0f, y / 131.0f, z / 131.0f);
  return true;
}

// ------------------------------------------------------------
// Prompts
// ------------------------------------------------------------
void printCalibrationPrompt() {
  if (calibration.awaitingRestart()) {
    Serial.printf("Fully CLOSE the door, hold it reasonably still, then press Button 1 to retry round %d.\n",
                  calibration.round());
    return;
  }
  Serial.printf("\nCalibration round %d of 2\n", calibration.round());
  Serial.println(calibration.expectingOpen()
                     ? "OPEN the door to your normal open position."
                     : "CLOSE the door completely.");
  Serial.println("Hold it reasonably still, then press Button 1.");
}

// Rate limiting: returns true at most once per everyMs (and the first time).
bool due(uint32_t& last, uint32_t nowMs, uint32_t everyMs) {
  if (last != 0 && nowMs - last < everyMs) return false;
  last = nowMs ? nowMs : 1;
  return true;
}

BiasMeasurement::Status prevBiasStatus = BiasMeasurement::Status::Collecting;
uint32_t lastPauseMsgMs = 0, lastProgressMs = 0, lastSettleMsgMs = 0;
int lastPercent = -1;
bool pauseAnnounced = false;
SettleGate zeroGate(cfg::SETTLE_MS, cfg::SETTLE_TIMEOUT_MS);

void startBiasMeasurement() {
  calState = CalState::Warmup;
  biasMeasurement.restart();
  door.clearAll();
  calibration.begin();
  zeroGate.cancel();
  prevBiasStatus = BiasMeasurement::Status::Collecting;
  lastPauseMsgMs = lastProgressMs = 0;
  lastPercent = -1;
  pauseAnnounced = false;
  Serial.println("Keep the door CLOSED. Hold reasonably still: small vibrations are okay.");
  Serial.printf("Warming up %lu s, then collecting %lu s of steady samples (gives up after %lu s).\n",
                (unsigned long)(cfg::WARMUP_MS / 1000UL),
                (unsigned long)(cfg::BIAS_REQUIRED_ACCEPTED_MS / 1000UL),
                (unsigned long)(cfg::BIAS_MAX_WAIT_MS / 1000UL));
}

// Readable, rate-limited progress for the offset measurement.
void biasFeedback(BiasMeasurement::Status st, uint32_t nowMs) {
  if (st == BiasMeasurement::Status::Paused && prevBiasStatus != BiasMeasurement::Status::Paused) {
    if (due(lastPauseMsgMs, nowMs, 2000)) {
      Serial.println("Movement detected; pausing briefly.");
      if (biasMeasurement.pauses() % 3 == 0) Serial.println("Hold reasonably still: small vibrations are okay.");
      pauseAnnounced = true;
    }
  } else if (st == BiasMeasurement::Status::Collecting && prevBiasStatus == BiasMeasurement::Status::Paused &&
             pauseAnnounced) {
    Serial.println("Steady again; resuming.");
    pauseAnnounced = false;
  }
  prevBiasStatus = st;

  int pct = biasMeasurement.percent();
  if (!biasMeasurement.inWarmup() && pct != lastPercent && due(lastProgressMs, nowMs, 1500)) {
    Serial.printf("Calibration: %d%%\n", pct);
    lastPercent = pct;
  }
}

// ------------------------------------------------------------
// One gyro sample (called every ~20 ms)
// ------------------------------------------------------------
void pollGyro() {
  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) < 0) return;
  nextSampleUs += cfg::SAMPLE_US;
  if ((int32_t)(nowUs - nextSampleUs) > 0) nextSampleUs = nowUs + cfg::SAMPLE_US;  // fell behind: resync

  Vec3 g;
  if (!readGyro(g)) {
    failedReads++;
    uint32_t nowMs = millis();
    // A brown-out can reset the MPU6050 into sleep mode: re-configure it.
    if (haveGoodSample && nowMs - lastGoodMs > 500 && nowMs - lastReinitMs > 1000) {
      lastReinitMs = nowMs;
      configureMpu();
    }
    return;
  }

  // Timestamp the sample right after reading it. dt spans any failed
  // reads, so a stall shows up as a long dt (and is flagged as missed).
  uint32_t tUs = micros();
  float dt = haveGoodSample ? (tUs - lastGoodUs) / 1e6f : cfg::NOMINAL_DT_S;
  lastGoodUs = tUs;
  lastGoodMs = millis();
  haveGoodSample = true;

  if (calState == CalState::Warmup || calState == CalState::Bias) {
    BiasMeasurement::Status st = biasMeasurement.add(g, dt);
    if (calState == CalState::Warmup && !biasMeasurement.inWarmup()) {
      calState = CalState::Bias;
      Serial.println("Warm-up done; collecting steady samples.");
    }
    if (st == BiasMeasurement::Status::Done) {
      door.setBias(biasMeasurement.bias());
      calState = CalState::Moves;
      Serial.printf("Gyro offset OK (%.2f, %.2f, %.2f deg/s) from %d steady samples; %lu pause(s) along the way.\n",
                    biasMeasurement.bias().x, biasMeasurement.bias().y, biasMeasurement.bias().z,
                    biasMeasurement.samples(), (unsigned long)biasMeasurement.pauses());
      printCalibrationPrompt();
    } else if (st == BiasMeasurement::Status::TimedOut) {
      // Never accept an unreliable offset and never wait forever.
      calState = CalState::Fault;
      Serial.printf("\nCalibration timed out: %s\n", biasMeasurement.reason());
      Serial.println("Nothing was accepted. Press Button 1 or send 'c' over Serial to retry.");
    } else {
      biasFeedback(st, millis());
    }
    return;
  }

  if (calState == CalState::Fault) return;  // calibration failed: wait for a retry

  DoorEstimator::StepResult r = door.step(g, dt);
  if (r.biasAdapted) {
    Serial.printf("Gyro offset refined during a still period (adjustment #%lu).\n",
                  (unsigned long)door.biasAdjustments());
  }
}

// ------------------------------------------------------------
// Button: starts a capture; the position is saved once it settles.
// ------------------------------------------------------------
void handleButtonPress(uint32_t nowMs) {
  if (!mpuReady) return;

  if (calState == CalState::Fault) {
    Serial.println("Retrying calibration...");
    startBiasMeasurement();
    return;
  }
  if (calState == CalState::Warmup || calState == CalState::Bias) {
    Serial.println("Still collecting the gyro offset; wait for the calibration prompt.");
    return;
  }
  if (calState == CalState::Moves) {
    if (calibration.requestCapture(nowMs, door.rotation()))
      Serial.println("Button pressed: settling... keep the door steady for a moment.");
    return;
  }
  if (calState == CalState::Done) {
    if (zeroGate.begin(nowMs)) Serial.println("Button pressed: settling before zeroing...");
  }
}

void printCaptureOutcome(CalibrationSession::Outcome o, const Vec3& rotation) {
  switch (o) {
    case CalibrationSession::Outcome::Saved:
      Serial.printf("Position saved. Movement: %.1f degrees\n", magnitude(rotation));
      if (calibration.settleMotionDeg() > 1.5f)
        Serial.printf("(The door moved %.1f deg while settling; that motion is included.)\n",
                      calibration.settleMotionDeg());
      printCalibrationPrompt();
      break;
    case CalibrationSession::Outcome::RestartAccepted:
      Serial.println("Position saved.");
      printCalibrationPrompt();
      break;
    case CalibrationSession::Outcome::RoundFailed:
    case CalibrationSession::Outcome::Failed:
      Serial.printf("\nCalibration rejected: %s\n", calibration.result().message);
      printCalibrationPrompt();
      break;
    case CalibrationSession::Outcome::Complete:
      door.setCalibration(calibration.result().axis, calibration.result().openAngleDeg);
      door.zero(ClosureSource::Manual);  // the user just asserted "closed"
      calState = CalState::Done;
      Serial.printf("\nCalibration complete! %s\n", calibration.result().message);
      Serial.println("Closed reference: 0 degrees (manual).");
      Serial.printf("Alarm activates after %lu seconds open.\n", (unsigned long)(cfg::DOOR_OPEN_LIMIT_MS / 1000UL));
      Serial.println("To remove accumulated drift: CLOSE the door fully, hold still, press Button 1.");
      break;
    default:
      break;
  }
}

// Called every loop: finishes any pending button capture.
void serviceCapture(uint32_t nowMs) {
  if (calState == CalState::Moves && calibration.capturing()) {
    Vec3 rotation = door.rotation();
    CalibrationSession::Outcome o = calibration.service(nowMs, door.settled(), rotation);
    if (o == CalibrationSession::Outcome::Pending) return;
    if (o == CalibrationSession::Outcome::SettleTimeout) {
      // Rotation keeps accumulating: nothing was lost or consumed.
      Serial.println("Couldn't settle in time (still moving?). Hold the door steady and press Button 1 again.");
      return;
    }
    door.resetRotation();
    printCaptureOutcome(o, rotation);
  } else if (calState == CalState::Done && zeroGate.active()) {
    SettleGate::Result res = zeroGate.service(nowMs, door.settled());
    if (res == SettleGate::Result::Confirmed) {
      // This is a USER ASSERTION that the door is closed, not a measurement.
      float before = door.angle();
      door.zero(ClosureSource::Manual);
      Serial.printf("Closed reference set manually (estimate was %.1f deg before zeroing).\n", before);
    } else if (res == SettleGate::Result::TimedOut) {
      Serial.println("Couldn't settle in time; angle NOT zeroed. Hold the door steady and press Button 1 again.");
    }
  }
}

// ------------------------------------------------------------
// Serial commands: 'c' restarts calibration without reflashing.
// ------------------------------------------------------------
void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    if ((c == 'c' || c == 'C') && mpuReady) {
      Serial.println("\nRestarting calibration...");
      startBiasMeasurement();
    }
  }
}

// ------------------------------------------------------------
// Door state + alarm
// ------------------------------------------------------------
void updateDoorAndAlarm(uint32_t nowMs) {
  // MPU is "valid" only while it keeps returning fresh samples.
  bool valid = mpuReady && haveGoodSample && (nowMs - lastGoodMs) < cfg::SENSOR_INVALID_AFTER_MS;
  if (valid != sensorValid) {
    sensorValid = valid;
    if (!valid) {
      door.markSensorLost();
      Serial.println("WARNING: MPU6050 stopped responding. Door angle is not reliable.");
    } else {
      Serial.println("MPU6050 responding again (angle quality stays 'degraded' until re-zeroed).");
    }
  }

#if USE_REED_SWITCH
  bool reedClosed = reedDebounce.update(digitalRead(cfg::REED_PIN) == cfg::REED_ACTIVE_STATE_CLOSED, nowMs);
  bool reedKnown = reedDebounce.known();
  if (reedKnown && reedClosed) {
    // Independently confirmed closure: reset the gyro angle every time.
    door.zero(ClosureSource::Reed);
  }
  if (reedKnown && reedClosed != lastReedClosed) {
    Serial.println(reedClosed ? "Reed: contact CLOSED (door confirmed closed)."
                              : "Reed: contact OPEN (door confirmed open).");
  }
  lastReedClosed = reedClosed;
#else
  const bool reedClosed = false;
  const bool reedKnown = false;
#endif

  DoorInputs in;
  in.reedEnabled = USE_REED_SWITCH;
  in.reedKnown = reedKnown;
  in.reedClosed = reedClosed;
  in.sensorValid = sensorValid;
  in.calibrated = door.calibrated();
  in.estimatedClosed = door.estimatedClosed();

  DoorState newState = resolveDoorState(in);
  if (newState != doorState) {
    Serial.printf("EVENT: Door %s (%s)\n", toString(newState),
                  USE_REED_SWITCH ? "reed-confirmed"
                                  : (newState == DoorState::Closed ? toString(door.closureSource()) : "gyro estimate"));
    doorState = newState;
  }

  AlarmTimer::Event ev = alarmTimer.update(nowMs, doorState);
  if (ev == AlarmTimer::Event::Started) {
    buzzerOn();
    Serial.println("BUZZ!");
  } else if (ev == AlarmTimer::Event::Stopped) {
    buzzerOff();
    Serial.println("Door closed: alarm reset.");
  }
}

// ------------------------------------------------------------
// Temperature + telemetry snapshot (every 5 s)
// ------------------------------------------------------------
void takeSnapshot(uint32_t nowMs) {
  float temperature = NAN;
  if (bmpReady) {
    temperature = bmp.readTemperature();
    if (isfinite(temperature)) {
      Serial.printf("Temperature: %.2f C (sensor location, not necessarily fridge interior)\n", temperature);
    } else {
      Serial.println("Temperature read failed.");
    }
  } else {
    Serial.println("Temperature unavailable: BMP280 not found.");
  }

  telemetrySeq++;

#if !CLOUD_SEND_EXTENDED
  // The original table cannot say "unknown": door_open is a boolean. Rather
  // than upload a misleading "closed", skip door data until we know.
  if (doorState == DoorState::Unknown) {
    Serial.println("Cloud: door state unknown; snapshot not uploaded (legacy schema).");
    return;
  }
#endif

  CloudReading reading;
  memset(&reading, 0, sizeof(reading));
  reading.uptimeMs = nowMs;
  reading.temperatureC = temperature;
  reading.angleDeg = door.calibrated() ? door.angle() : 0.0f;
  reading.doorOpen = (doorState == DoorState::Open);
  reading.alarmActive = alarmTimer.active();
  reading.openDurationMs = alarmTimer.openMs(nowMs);

  reading.seq = telemetrySeq;
  time_t now = time(nullptr);
  reading.measuredAtUnix = now > 1700000000 ? (int64_t)now : 0;
  reading.doorState = toString(doorState);
  reading.closureSource = (doorState == DoorState::Closed) ? toString(
#if USE_REED_SWITCH
      ClosureSource::Reed
#else
      door.closureSource()
#endif
  ) : "none";
  reading.calibrationState = toString(calState);
  reading.angleQuality = toString(door.quality());
  reading.sensorValid = sensorValid;
  reading.sinceReferenceMs = (uint32_t)(door.secondsSinceReference() * 1000.0f);
  reading.missedSamples = door.missedSamples();
  reading.biasAdjustments = door.biasAdjustments();
  reading.reedEnabled = USE_REED_SWITCH;
#if USE_REED_SWITCH
  reading.reedKnown = reedDebounce.known();
  reading.reedClosed = reedDebounce.value();
#endif

  queueCloudReading(reading);
}

// ------------------------------------------------------------
// Setup / loop
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);
  Wire.setTimeOut(50);

  pinMode(cfg::BUTTON_PIN, INPUT_PULLUP);
  buttonDebounce.begin(false, millis());

#if USE_REED_SWITCH
  pinMode(cfg::REED_PIN, INPUT_PULLUP);
  reedDebounce.begin(digitalRead(cfg::REED_PIN) == cfg::REED_ACTIVE_STATE_CLOSED, millis());
#endif

  ledcSetup(cfg::BUZZER_CHANNEL, 1000, 8);
  ledcAttachPin(cfg::BUZZER_PIN, cfg::BUZZER_CHANNEL);
  buzzerOff();

  Serial.printf("\nStay-Fresh: fridge monitor v%s\n", STAYFRESH_FW_VERSION);
  Serial.printf("Mode: %s\n", USE_REED_SWITCH ? "reed switch decides open/closed; gyro = approximate angle"
                                              : "gyro-only (estimated closure; will drift)");

  bmpReady = bmp.begin(0x76);
  if (!bmpReady) bmpReady = bmp.begin(0x77);
  Serial.println(bmpReady ? "BMP280 ready." : "BMP280 not found.");

  mpuReady = configureMpu();
  if (mpuReady) {
    Serial.println("MPU6050 responding.");
    startBiasMeasurement();
  } else {
    calState = CalState::Fault;
    Serial.println("MPU6050 not found. Check wiring.");
#if !USE_REED_SWITCH
    Serial.println("Without the MPU6050 (and no reed switch) the door state stays UNKNOWN.");
#endif
  }

  nextSampleUs = micros();
  lastPrintMs = lastTelemetryMs = millis();

  // Networking runs in its own task so it can never block the gyro loop,
  // and local sensing/alarm keep working if Wi-Fi or Supabase is down.
  startCloudUpload();
}

void loop() {
  uint32_t now = millis();

  bool pressed = buttonDebounce.update(digitalRead(cfg::BUTTON_PIN) == LOW, now);
  bool pressEdge = pressed && !lastButtonPressed;
  lastButtonPressed = pressed;

  handleSerial();
  if (mpuReady) pollGyro();
  if (pressEdge) handleButtonPress(now);
  serviceCapture(millis());

  now = millis();
  updateDoorAndAlarm(now);

  if (calState == CalState::Done && now - lastPrintMs >= cfg::PRINT_MS) {
    lastPrintMs = now;
    Serial.printf("Door angle: %.1f deg | %s | closure: %s | angle: %s | Open: %.1f s | Alarm: %s\n",
                  door.angle(), toString(doorState), toString(door.closureSource()),
                  toString(door.quality()), alarmTimer.openMs(now) / 1000.0f,
                  alarmTimer.active() ? "ON" : "OFF");
  }

  if (now - lastTelemetryMs >= cfg::TEMP_MS) {
    lastTelemetryMs = now;
    takeSnapshot(now);
  }
}
