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
#include "oled_ui.h"

using namespace sf;

constexpr uint8_t MPU_ADDR = 0x68;

// ------------------------------------------------------------
// State
// ------------------------------------------------------------
Adafruit_BMP280 bmp;
bool bmpReady = false;
bool mpuReady = false;

BiasAverager biasAverager;
DoorEstimator door;
CalibrationSession calibration;
AlarmTimer alarmTimer(cfg::DOOR_OPEN_LIMIT_MS);
TempMonitor tempMonitor;
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

bool uploadsPaused = false; // door state unknown: snapshots are not uploaded yet
uint32_t lastStatusMs = 0;
bool snapshotNow = false; // door/alarm changed: upload right away
float lastTempC = NAN;
uint32_t seenOkCount = 0, seenFailCount = 0;

bool lastButtonPressed = false;
bool lastReedClosed = false;

// ------------------------------------------------------------
// Buzzer (explicit LEDC channel; tone()/noTone() caused
// "LEDC is not initialized" on Arduino core 2.x)
// ------------------------------------------------------------
static bool buzzerIsOn = false;
void buzzerOn()
{
  if (buzzerIsOn)
    return;
  buzzerIsOn = true;
  ledcWriteTone(cfg::BUZZER_CHANNEL, cfg::BUZZER_FREQ_HZ);
}
void buzzerOff()
{
  if (!buzzerIsOn)
    return;
  buzzerIsOn = false;
  ledcWrite(cfg::BUZZER_CHANNEL, 0);
}

// Only the door alarm makes sound (fast beeping). Temperature alerts are
// visual only: the OLED and the dashboard.
void updateBuzzer(uint32_t nowMs)
{
  bool on = alarmTimer.active() && nowMs % cfg::BEEP_PERIOD_MS < cfg::BEEP_PERIOD_MS / 2;
  if (on)
    buzzerOn();
  else
    buzzerOff();
}

// ------------------------------------------------------------
// MPU6050
// ------------------------------------------------------------
bool writeMpu(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool configureMpu()
{
  bool ok = writeMpu(0x6B, 0x00);  // wake
  ok = ok && writeMpu(0x1A, 0x03); // low-pass filter (~44 Hz)
  ok = ok && writeMpu(0x1B, 0x10); // gyro range +/-1000 deg/s (fast swings do not saturate)
  return ok;
}

bool readGyro(Vec3 &g)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43); // GYRO_XOUT_H
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6)
    return false;

  uint8_t raw[6];
  for (int i = 0; i < 6; i++)
    raw[i] = Wire.read();

  int16_t x = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
  int16_t y = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
  int16_t z = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);

  // 32.8 LSB per deg/s on the +/-1000 deg/s range (must match register 0x1B).
  g = Vec3(x / 32.8f, y / 32.8f, z / 32.8f);
  return true;
}

// ------------------------------------------------------------
// BMP280 power switching. The BMP280 sometimes locks up (most likely latch-up
// from noise on its long wire into the fridge); restarting the ESP32 does not
// help, only cutting its power does. Its VCC comes from BMP_POWER_PIN, so the
// firmware can do that itself.
// ------------------------------------------------------------
uint8_t bmpAddr = 0x76;     // updated by beginBmp()
bool bmpExpected = false;   // found at boot, so a missing BMP280 later means "stuck"
bool bmpBadReading = false; // last read was NaN / out of range
uint32_t lastBmpRecoveryMs = 0;
uint8_t bmpFailStreak = 0; // power cycles in a row that did not fix it
uint32_t bmpRecoveries = 0;

bool beginBmp()
{
  if (bmp.begin(0x76))
  {
    bmpAddr = 0x76;
    return true;
  }
  if (bmp.begin(0x77))
  {
    bmpAddr = 0x77;
    return true;
  }
  return false;
}

void bmpPower(bool on)
{
  pinMode(cfg::BMP_POWER_PIN, OUTPUT);
  digitalWrite(cfg::BMP_POWER_PIN, on ? HIGH : LOW);
}

bool bmpResponds()
{
  Wire.beginTransmission(bmpAddr);
  return Wire.endTransmission() == 0;
}

// The software version of unplugging the BMP280.
void powerCycleBmp()
{
  bmpRecoveries++;
  Serial.printf("BMP280 stuck: power-cycling it (recovery #%lu)\n", (unsigned long)bmpRecoveries);
  oledLog("BMP: power cycle");

  Wire.end();
  // Hold SCL then SDA low while it is off, or the BMP280 stays half-powered
  // through its data pins. (This order is not a START condition for the MPU.)
  pinMode(cfg::I2C_SCL, OUTPUT);
  digitalWrite(cfg::I2C_SCL, LOW);
  pinMode(cfg::I2C_SDA, OUTPUT);
  digitalWrite(cfg::I2C_SDA, LOW);
  bmpPower(false);
  delay(500); // let the breakout's capacitors drain
  bmpPower(true);
  pinMode(cfg::I2C_SCL, INPUT);
  pinMode(cfg::I2C_SDA, INPUT);
  delay(100); // BMP280 power-up time

  Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);
  Wire.setTimeOut(50);
  bmpReady = beginBmp();
  if (mpuReady)
    configureMpu();      // the MPU saw the bus go quiet: make sure it is still awake
  bmpBadReading = false; // judged again on the next reading
  Serial.println(bmpReady ? "  BMP280 OK" : "  BMP280 not responding");
}

// Called once a second: power-cycle the BMP280 if it stopped working.
void checkBmp(uint32_t nowMs)
{
  if (!bmpExpected)
    return; // not found at boot: nothing to recover
  bool stuck = !bmpReady || bmpBadReading || !bmpResponds();
  if (!stuck)
  {
    bmpFailStreak = 0;
    return;
  }
  bmpReady = false; // never upload a reading from a stuck sensor

  // Retry every 3 s; after 3 failures in a row, every 30 s.
  uint32_t wait = bmpFailStreak >= 3 ? 30000 : 3000;
  if (lastBmpRecoveryMs != 0 && nowMs - lastBmpRecoveryMs < wait)
    return;
  lastBmpRecoveryMs = nowMs;
  powerCycleBmp();
  if (!bmpReady && bmpFailStreak < 255)
    bmpFailStreak++;
}

// ------------------------------------------------------------
// Prompts
// ------------------------------------------------------------
void printPrompt()
{
  Serial.printf("\nCalibration round %d of 2\n", calibration.round());
  Serial.println(calibration.expectingOpen()
                     ? "OPEN the door to your normal open position."
                     : "CLOSE the door completely.");
  Serial.println("Hold it still, then press Button 1.");
}

void startBiasMeasurement()
{
  calState = CalState::Bias;
  biasAverager.reset();
  door.clearAll();
  calibration.begin();
  oledLog("Measuring offset");
  Serial.println("Keep the door CLOSED and reasonably still.");
  Serial.printf("Measuring gyro offset from %d samples (about %lu seconds)...\n", cfg::BIAS_SAMPLES,
                (unsigned long)(cfg::BIAS_SAMPLES * cfg::SAMPLE_US / 1000000UL));
}

// ------------------------------------------------------------
// One gyro sample (called every ~20 ms)
// ------------------------------------------------------------
void sampleMotion()
{
  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) < 0)
    return;
  nextSampleUs += cfg::SAMPLE_US;
  if ((int32_t)(nowUs - nextSampleUs) > 0)
    nextSampleUs = nowUs + cfg::SAMPLE_US; // fell behind: resync

  Vec3 g;
  if (!readGyro(g))
  { // failed reads are skipped; they never count as samples
    failedReads++;
    uint32_t nowMs = millis();
    // A brown-out can reset the MPU6050 into sleep mode: re-configure it.
    if (haveGoodSample && nowMs - lastGoodMs > 500 && nowMs - lastReinitMs > 1000)
    {
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

  // Step 1: average the gyro offset (no stillness gate, no restarts).
  if (calState == CalState::Bias)
  {
    if (biasAverager.add(g))
    {
      door.setBias(biasAverager.bias());
      calState = CalState::Moves;
      Serial.println("Gyroscope offset calibration complete.");
      oledLog("Offset done");
      printPrompt();
    }
    return;
  }

  // From here on the angle is integrated continuously (also during the
  // calibration movements, which accumulate in door.rotation()).
  DoorEstimator::StepResult r = door.step(g, dt);
  if (r.biasAdapted)
  {
    // (quiet: refinements happen often; the count is in the DB / status)
    (void)r;
  }
}

// ------------------------------------------------------------
// Calibration: Button 1 saves currentRotation, clears it, advances.
// ------------------------------------------------------------
void finishCalibration()
{
  // Hinge axis = (m0 - m1 + m2 - m3) / |...|, opening angle from the two openings.
  if (!calibration.result().ok)
  {
    oledLog("Calib FAILED: redo");
    Serial.printf("\nCalibration movements were inconsistent: %s\n", calibration.result().message);
    Serial.println("Keep the sensor firmly attached and start with the door CLOSED.");
    printPrompt();
    return;
  }
  door.setCalibration(calibration.result().axis, calibration.result().openAngleDeg);
  door.zero(ClosureSource::Manual); // the final closing press asserts "closed": angle = 0
  calState = CalState::Done;
  oledLog("Calibration DONE");
  Serial.println("\nCalibration complete!");
  Serial.println(calibration.result().message);
  if (calibration.result().warning[0])
    Serial.printf("WARNING: %s\n", calibration.result().warning);
  Serial.println("Closed reference: 0 degrees. Angle and open/closed are gyro ESTIMATES and will drift.");
  Serial.printf("Alarm activates after %lu seconds open.\n", (unsigned long)(cfg::DOOR_OPEN_LIMIT_MS / 1000UL));
  Serial.println("To reset the closed reference and the alarm: CLOSE the door fully, then press Button 1.");
}

void handleCalibrationPress()
{
  Vec3 rotation = door.rotation();                             // currentRotation
  door.resetRotation();                                        // cleared
  CalibrationSession::Outcome o = calibration.press(rotation); // stage advances
  Serial.printf("Saved movement: %.1f degrees\n", magnitude(rotation));
  {
    char m[22];
    snprintf(m, sizeof(m), "Saved %.0f deg", magnitude(rotation));
    oledLog(m);
  }
  if (o == CalibrationSession::Outcome::Saved)
    printPrompt();
  else
    finishCalibration(); // Complete or Failed
}

// Every debounced press is accepted immediately.
void handleButtonPress()
{
  if (!mpuReady)
    return;
  if (calState == CalState::Bias)
  {
    Serial.println("Still measuring the gyro offset; wait for the prompt.");
  }
  else if (calState == CalState::Moves)
  {
    handleCalibrationPress();
  }
  else if (calState == CalState::Done)
  {
    // A USER ASSERTION that the door is physically closed, not a measurement.
    float before = door.angle();
    door.zero(ClosureSource::Manual); // gyro mode: door reads closed, so the alarm stops and its timer resets
    oledLog("Closed ref reset");
    Serial.printf("Closed reference reset: 0 degrees (estimate was %.1f). Press Button 1 ONLY when the door is physically closed.\n",
                  before);
#if USE_REED_SWITCH
    Serial.println("(Reed switch enabled: the contact, not this button, decides open/closed and the alarm.)");
#endif
  }
}

// ------------------------------------------------------------
// Serial commands: 'c' restarts calibration without reflashing.
// ------------------------------------------------------------
void handleSerial()
{
  while (Serial.available())
  {
    int c = Serial.read();
    if ((c == 'c' || c == 'C') && mpuReady)
    {
      Serial.println("\nRestarting calibration...");
      startBiasMeasurement();
    }
  }
}

// ------------------------------------------------------------
// Door state + alarm
// ------------------------------------------------------------
void updateDoorAndAlarm(uint32_t nowMs)
{
  // MPU is "valid" only while it keeps returning fresh samples.
  bool valid = mpuReady && haveGoodSample && (nowMs - lastGoodMs) < cfg::SENSOR_INVALID_AFTER_MS;
  if (valid != sensorValid)
  {
    sensorValid = valid;
    if (!valid)
    {
      door.markSensorLost();
      oledLog("MPU6050 LOST");
      Serial.println("WARNING: MPU6050 stopped responding. Door angle is not reliable.");
    }
    else
    {
      Serial.println("MPU6050 responding again (angle quality stays 'degraded' until re-zeroed).");
    }
  }

#if USE_REED_SWITCH
  bool reedClosed = reedDebounce.update(digitalRead(cfg::REED_PIN) == cfg::REED_ACTIVE_STATE_CLOSED, nowMs);
  bool reedKnown = reedDebounce.known();
  if (reedKnown && reedClosed)
  {
    // Independently confirmed closure: reset the gyro angle every time.
    door.zero(ClosureSource::Reed);
  }
  if (reedKnown && reedClosed != lastReedClosed)
  {
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
  if (newState != doorState)
  {
    Serial.printf("EVENT: Door %s (%s)\n", toString(newState),
                  USE_REED_SWITCH ? "reed-confirmed"
                                  : (newState == DoorState::Closed ? toString(door.closureSource()) : "gyro estimate"));
    doorState = newState;
    snapshotNow = true; // do not wait for the next periodic upload
    if (newState == DoorState::Open)
      oledLog("Door OPENED");
    else if (newState == DoorState::Closed)
      oledLog("Door CLOSED");
  }

  AlarmTimer::Event ev = alarmTimer.update(nowMs, doorState);
  if (ev == AlarmTimer::Event::Started)
  {
    snapshotNow = true;
    oledLog("ALARM: door open!");
    Serial.println("BUZZ!");
  }
  else if (ev == AlarmTimer::Event::Stopped)
  {
    snapshotNow = true;
    oledLog("Alarm cleared");
    Serial.println("Door closed: alarm reset.");
  }
}

// ------------------------------------------------------------
// Temperature + telemetry snapshot (every 5 s)
// ------------------------------------------------------------
void takeSnapshot(uint32_t nowMs)
{
  float temperature = NAN;
  if (bmpReady)
  {
    temperature = bmp.readTemperature();
    bmpBadReading = !isfinite(temperature) || temperature < -40.0f || temperature > 85.0f;
    lastTempC = temperature;
    if (isfinite(temperature))
    {
      if (tempMonitor.update(temperature, nowMs, doorState == DoorState::Closed))
      {
        if (!tempMonitor.alarm())
        {
          Serial.printf("Temperature alert cleared (%.2f C)\n", temperature);
          oledLog("Temp alert cleared");
        }
        else if (tempMonitor.rising())
        {
          Serial.printf("ALERT: Temperature rising with door closed (%+.2f C over 15 min, now %.2f C)\n",
                        tempMonitor.lastChange(), temperature);
          oledLog("ALERT: temp rising");
        }
        else
        {
          Serial.printf("ALERT: Above %.1f C for 1+ hour (%.2f C)\n", cfg::SAFE_MAX_C, temperature);
          oledLog("ALERT: >4C 1 hour");
        }
      }
    }
  }

  telemetrySeq++;

#if !CLOUD_SEND_EXTENDED
  // The original table cannot say "unknown": door_open is a boolean. Rather
  // than upload a misleading "closed", skip door data until we know.
  // (cfg::TEST_TEMP_ONLY bypasses this so bench temperature tests upload.)
  if (doorState == DoorState::Unknown && !cfg::TEST_TEMP_ONLY)
  {
    uploadsPaused = true; // shown in the status line; no per-snapshot spam
    return;
  }
#endif

  uploadsPaused = false;

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
                                                                 )
                                                           : "none";
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
// One status line every STATUS_MS: door, alarm, temperature, Wi-Fi, Supabase.
// Events (door opened, alarm, errors, calibration steps) still print as they happen.
// ------------------------------------------------------------
void printStatus(uint32_t nowMs)
{
  char door_[48];
  if (calState == CalState::Done)
  {
    snprintf(door_, sizeof(door_), "Door %s %.0fdeg%s", toString(doorState), door.angle(),
             alarmTimer.active() ? " ALARM" : "");
  }
  else if (calState == CalState::Fault)
  {
    snprintf(door_, sizeof(door_), "Door sensor FAULT");
  }
  else
  {
    snprintf(door_, sizeof(door_), "Calibrating");
  }

  char temp_[16];
  if (isfinite(lastTempC))
    snprintf(temp_, sizeof(temp_), "%.1fC", lastTempC);
  else
    snprintf(temp_, sizeof(temp_), "--");

  char wifi_[24];
  if (cloudStatus.wifi)
    snprintf(wifi_, sizeof(wifi_), "OK %ddBm", WiFi.RSSI());
  else
    snprintf(wifi_, sizeof(wifi_), "DOWN");

  uint32_t ok = cloudStatus.okCount, fail = cloudStatus.failCount;
  char cloud_[96];
  if (!cloudStatus.wifi)
    snprintf(cloud_, sizeof(cloud_), "waiting for WiFi");
  else if (!cloudStatus.signedIn)
    snprintf(cloud_, sizeof(cloud_), "signing in...");
  else if (uploadsPaused)
    snprintf(cloud_, sizeof(cloud_), "paused (finish calibration)");
  else if (ok + fail == 0)
    snprintf(cloud_, sizeof(cloud_), "connected, nothing sent yet");
  else if (cloudStatus.lastCode == 201)
    snprintf(cloud_, sizeof(cloud_), "OK sent=%lu fail=%lu last=%lums %lus ago", (unsigned long)ok, (unsigned long)fail,
             (unsigned long)cloudStatus.lastTookMs, (unsigned long)((nowMs - cloudStatus.lastOkMs) / 1000));
  else
    snprintf(cloud_, sizeof(cloud_), "FAIL code=%d sent=%lu fail=%lu", cloudStatus.lastCode, (unsigned long)ok,
             (unsigned long)fail);

  Serial.printf("[status] %s | %s | WiFi %s | Supabase %s\n", door_, temp_, wifi_, cloud_);
}

// ------------------------------------------------------------
// OLED: simplified version of what the Serial Monitor shows
// ------------------------------------------------------------
void updateOled(uint32_t nowMs)
{
  // Upload results -> short log lines (only first success / recovery / failures,
  // so a healthy 1 Hz upload does not flicker the log).
  uint32_t ok = cloudStatus.okCount, fail = cloudStatus.failCount;
  static bool wasFailing = false;
  if (fail != seenFailCount)
  {
    char m[22];
    snprintf(m, sizeof(m), "Upload FAIL %d", cloudStatus.lastCode);
    oledLog(m);
    wasFailing = true;
  }
  else if (ok != seenOkCount)
  {
    if (seenOkCount == 0 || wasFailing)
      oledLog("Upload OK");
    wasFailing = false;
  }
  seenOkCount = ok;
  seenFailCount = fail;

  OledView v;
  memset(&v, 0, sizeof(v));
  if (calState == CalState::Bias)
  {
    v.calibrating = true;
    v.calTitle = "CALIBRATION";
    v.calLine1 = "Hold still";
    v.calLine2 = "Door CLOSED, wait...";
  }
  else if (calState == CalState::Moves)
  {
    char title[22];
    snprintf(title, sizeof(title), "CALIBRATE step %d/4",
             (calibration.round() - 1) * 2 + (calibration.expectingOpen() ? 1 : 2));
    static char titleBuf[22];
    strncpy(titleBuf, title, sizeof(titleBuf));
    v.calibrating = true;
    v.calTitle = titleBuf;
    v.calLine1 = calibration.expectingOpen() ? "OPEN door" : "CLOSE door";
    v.calLine2 = "hold still, press btn";
  }
  else if (calState == CalState::Fault)
  {
    v.calibrating = true;
    v.calTitle = "ERROR";
    v.calLine1 = "No MPU";
    v.calLine2 = "check wiring";
  }
  v.tempC = lastTempC;
  v.door = doorState == DoorState::Open ? "OPEN" : doorState == DoorState::Closed ? "CLOSED"
                                                                                  : "UNKNOWN";
  v.openSec = alarmTimer.openMs(nowMs) / 1000;
  v.alarm = alarmTimer.active();
  v.wifi = cloudStatus.wifi;
  v.uploadKnown = (ok + fail) > 0;
  v.uploadOk = cloudStatus.lastCode == 201;
  v.uploadAgeSec = ok > 0 ? (nowMs - cloudStatus.lastOkMs) / 1000 : 0;
  v.uploadCode = cloudStatus.lastCode;
  v.tempAlert = tempMonitor.alarm() ? (tempMonitor.rising() ? "TEMP RISING" : "TEMP >4C 1HR") : nullptr;
  oledDraw(v);
}

// ------------------------------------------------------------
// Setup / loop
// ------------------------------------------------------------
void setup()
{
  Serial.begin(115200);

  bmpPower(true);
  delay(200); // BMP280 power-up time

  Wire.begin(cfg::I2C_SDA, cfg::I2C_SCL);
  Wire.setTimeOut(50);

  pinMode(cfg::BUTTON_PIN, INPUT_PULLUP);
  buttonDebounce.begin(false, millis());

  if (!oledBegin())
    Serial.println(USE_OLED ? "OLED not found (check wiring / address)." : "OLED disabled.");

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

  bmpReady = bmpExpected = beginBmp();
  Serial.println(bmpReady ? "BMP280 ready." : "BMP280 not found.");

  mpuReady = configureMpu();
  if (mpuReady)
  {
    Serial.println("MPU6050 responding.");
    startBiasMeasurement();
  }
  else
  {
    calState = CalState::Fault;
    oledLog("MPU6050 not found");
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

void loop()
{
  uint32_t now = millis();

  bool pressed = buttonDebounce.update(digitalRead(cfg::BUTTON_PIN) == LOW, now);
  bool pressEdge = pressed && !lastButtonPressed;
  lastButtonPressed = pressed;

  handleSerial();
  static uint32_t lastBmpCheckMs = 0;
  if (now - lastBmpCheckMs >= 1000)
  {
    lastBmpCheckMs = now;
    checkBmp(now);
  }
  if (mpuReady)
    sampleMotion();
  if (pressEdge)
    handleButtonPress();

  now = millis();
  updateDoorAndAlarm(now);

  if (now - lastStatusMs >= cfg::STATUS_MS)
  {
    lastStatusMs = now;
    printStatus(now);
  }

  // Upload on every door/alarm change, every second while the door is open,
  // and every TEMP_MS otherwise.
  uint32_t period = (doorState == DoorState::Open || alarmTimer.active()) ? cfg::TEMP_OPEN_MS : cfg::TEMP_MS;
  if (snapshotNow || now - lastTelemetryMs >= period)
  {
    snapshotNow = false;
    lastTelemetryMs = now;
    takeSnapshot(now);
  }

  updateBuzzer(now);
  updateOled(now);
  delay(1); // let the Wi-Fi/TLS tasks and the idle task run; the 20 ms gyro pacing is unaffected
}