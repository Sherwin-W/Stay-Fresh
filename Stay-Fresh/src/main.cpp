#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <math.h>

constexpr uint8_t MPU_ADDR = 0x68;
constexpr int BUTTON_PIN = 32;
constexpr int BUZZER_PIN = 27;
// Drive the buzzer with a PWM tone so passive buzzers/speakers beep
// instead of just clicking. Also works with active buzzers.
constexpr int BUZZER_CHANNEL = 0;
constexpr uint32_t BUZZER_FREQ_HZ = 2000;

constexpr uint32_t SAMPLE_MS = 20;
constexpr uint32_t PRINT_MS = 500;
constexpr uint32_t TEMP_MS = 5000;
constexpr uint32_t DEBOUNCE_MS = 30;

// Sound the buzzer if the door stays open this long.
constexpr uint32_t OPEN_ALARM_MS = 10UL * 1000UL;
constexpr uint32_t BEEP_PERIOD_MS = 500; // On/off beep cycle.

// Temperature monitoring: two checks.
//
// 1. Direction check: while the door stays closed, average readings into
//    5-minute buckets and compare the newest bucket to the one 15 minutes
//    earlier. A rise of more than 0.3 C means the fridge is warming with
//    the door shut (bad seal, power outage, dying compressor).
// 2. Warm backstop: alert if the temperature stays above the safe limit
//    for an hour, even if it's flat (thermostat bumped, fridge dead).
//
// How much warmer the door reads than the fridge interior. Measure it by
// comparing against a fridge thermometer and set it here.
constexpr float DOOR_OFFSET_C = 0.0f;
constexpr float SAFE_MAX_C = 4.0f + DOOR_OFFSET_C;
constexpr float WARM_CLEAR_MARGIN_C = 0.3f; // Must drop this far below to reset.
constexpr uint32_t WARM_ALARM_MS = 60UL * 60UL * 1000UL; // 1 hour.

constexpr uint32_t TREND_BUCKET_MS = 5UL * 60UL * 1000UL; // 5-minute averages.
constexpr int TREND_POINTS = 4;          // 4 buckets = 15-minute span.
constexpr float RISE_THRESHOLD_C = 0.3f; // Above BMP280 jitter.
constexpr uint32_t TEMP_BEEP_PERIOD_MS = 2000; // Slow beep for temp alarm.
constexpr uint32_t TEMP_BEEP_ON_MS = 200;

// Estimated closed-position tolerance.
constexpr float CLOSED_TOLERANCE_DEG = 5.0f;

// Stillness detection: when the gyro reads nearly zero for a while,
// the door is not moving. Stop integrating and re-learn the gyro offset
// (it drifts with temperature, especially inside a fridge).
constexpr float STILL_RATE_DPS = 1.5f;
constexpr uint32_t STILL_MS = 1000;
constexpr float BIAS_ALPHA = 0.005f;

struct Vector3
{
  float x, y, z;
};

Adafruit_BMP280 bmp;
bool bmpReady = false;
bool mpuReady = false;

Vector3 gyroBias = {0, 0, 0};
Vector3 biasSum = {0, 0, 0};
int biasSamples = 0;
bool biasReady = false;

// Four calibration movements: open, close, open, close.
Vector3 calibrationMoves[4];
Vector3 currentRotation = {0, 0, 0};
int calibrationStage = 0;
bool calibrated = false;

// Unit vector along the door's rotation axis.
// Positive rotation means opening.
Vector3 openingAxis = {0, 0, 0};

float doorAngle = 0;
float calibratedOpenAngle = 0;
bool estimatedClosed = true;
uint32_t openedAt = 0;
bool alarmActive = false;

// Direction check state.
float trendHistory[TREND_POINTS];
int trendCount = 0;
float bucketSum = 0;
int bucketReadings = 0;
uint32_t bucketStart = 0;
bool tempRising = false;

// Warm backstop state.
bool warmTiming = false;
uint32_t warmSince = 0;
bool tempTooWarm = false;

bool tempAlarm = false;

bool moving = true;
uint32_t stillSince = 0;

uint32_t lastSample = 0;
uint32_t lastPrint = 0;
uint32_t lastTemperature = 0;

int lastRawButton = HIGH;
int stableButton = HIGH;
uint32_t rawChangedAt = 0;

float magnitude(const Vector3 &v)
{
  return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

float dot(const Vector3 &a, const Vector3 &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

bool writeMpu(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Read angular velocity in degrees per second.
// The MPU is configured for +/-1000 degrees/second so a fast
// door swing does not saturate the sensor.
bool readGyro(Vector3 &g)
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x43); // GYRO_XOUT_H

  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }

  if (Wire.requestFrom(MPU_ADDR, (uint8_t)6) != 6)
  {
    return false;
  }

  uint8_t raw[6];
  for (int i = 0; i < 6; i++)
  {
    raw[i] = Wire.read();
  }

  int16_t x = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
  int16_t y = (int16_t)(((uint16_t)raw[2] << 8) | raw[3]);
  int16_t z = (int16_t)(((uint16_t)raw[4] << 8) | raw[5]);

  g.x = x / 32.8f;
  g.y = y / 32.8f;
  g.z = z / 32.8f;

  return true;
}

bool buttonPressed(uint32_t now)
{
  int raw = digitalRead(BUTTON_PIN);

  if (raw != lastRawButton)
  {
    lastRawButton = raw;
    rawChangedAt = now;
  }

  if (now - rawChangedAt >= DEBOUNCE_MS &&
      raw != stableButton)
  {
    stableButton = raw;
    return stableButton == LOW;
  }

  return false;
}

void printPrompt()
{
  Serial.printf("\nCalibration round %d of 2\n",
                calibrationStage / 2 + 1);

  if (calibrationStage % 2 == 0)
  {
    Serial.println(
        "OPEN the door to your normal open position.");
  }
  else
  {
    Serial.println("CLOSE the door completely.");
  }

  Serial.println("Hold it still, then press Button 1.");
}

bool finishCalibration()
{
  // Combine opening and reversed closing movements.
  Vector3 direction = {
      calibrationMoves[0].x - calibrationMoves[1].x +
          calibrationMoves[2].x - calibrationMoves[3].x,

      calibrationMoves[0].y - calibrationMoves[1].y +
          calibrationMoves[2].y - calibrationMoves[3].y,

      calibrationMoves[0].z - calibrationMoves[1].z +
          calibrationMoves[2].z - calibrationMoves[3].z};

  float length = magnitude(direction);
  if (length < 1.0f)
    return false;

  openingAxis = {
      direction.x / length,
      direction.y / length,
      direction.z / length};

  float open1 = dot(calibrationMoves[0], openingAxis);
  float close1 = -dot(calibrationMoves[1], openingAxis);
  float open2 = dot(calibrationMoves[2], openingAxis);
  float close2 = -dot(calibrationMoves[3], openingAxis);

  // Require meaningful movement in the expected directions.
  if (open1 < 10 || close1 < 10 ||
      open2 < 10 || close2 < 10)
  {
    return false;
  }

  float average = (open1 + close1 + open2 + close2) / 4.0f;

  // Reject very inconsistent calibration movements.
  float values[] = {open1, close1, open2, close2};
  for (float value : values)
  {
    if (fabsf(value - average) > average * 0.40f)
    {
      return false;
    }
  }

  calibratedOpenAngle = (open1 + open2) / 2.0f;

  // The final calibration position is physically closed.
  doorAngle = 0;
  estimatedClosed = true;

  Serial.println("\nCalibration complete!");
  Serial.printf("Normal open angle: %.1f degrees\n",
                calibratedOpenAngle);
  Serial.println("Closed position set to 0 degrees.");
  Serial.println(
      "If the angle drifts, CLOSE the door and press Button 1.");

  return true;
}

void handleCalibrationPress()
{
  calibrationMoves[calibrationStage] = currentRotation;

  Serial.printf("Saved movement: %.1f degrees\n",
                magnitude(currentRotation));

  currentRotation = {0, 0, 0};
  calibrationStage++;

  if (calibrationStage < 4)
  {
    printPrompt();
    return;
  }

  calibrated = finishCalibration();

  if (!calibrated)
  {
    Serial.println("\nCalibration movements were inconsistent.");
    Serial.println(
        "Keep the sensor firmly attached and start with the door CLOSED.");
    calibrationStage = 0;
    printPrompt();
  }
}

void sampleMotion(float dt)
{
  Vector3 g;
  if (!readGyro(g))
  {
    // Do not integrate a missing measurement.
    return;
  }

  // Average 100 stationary samples to estimate gyro offset.
  if (!biasReady)
  {
    biasSum.x += g.x;
    biasSum.y += g.y;
    biasSum.z += g.z;
    biasSamples++;

    if (biasSamples >= 100)
    {
      gyroBias = {
          biasSum.x / biasSamples,
          biasSum.y / biasSamples,
          biasSum.z / biasSamples};

      biasReady = true;
      Serial.println("Gyroscope offset calibration complete.");
      printPrompt();
    }

    return;
  }

  Vector3 raw = g;
  g.x -= gyroBias.x;
  g.y -= gyroBias.y;
  g.z -= gyroBias.z;

  // Zero-velocity update: if the sensor has been still for a while,
  // treat the rotation as exactly zero and slowly re-learn the offset.
  uint32_t now = millis();
  if (magnitude(g) < STILL_RATE_DPS)
  {
    if (moving)
    {
      moving = false;
      stillSince = now;
    }

    if (now - stillSince >= STILL_MS)
    {
      gyroBias.x += BIAS_ALPHA * (raw.x - gyroBias.x);
      gyroBias.y += BIAS_ALPHA * (raw.y - gyroBias.y);
      gyroBias.z += BIAS_ALPHA * (raw.z - gyroBias.z);
      g = {0, 0, 0};
    }
  }
  else
  {
    moving = true;
  }

  if (!calibrated)
  {
    currentRotation.x += g.x * dt;
    currentRotation.y += g.y * dt;
    currentRotation.z += g.z * dt;
    return;
  }

  // Project angular velocity onto the learned hinge axis.
  float rotationRate = dot(g, openingAxis);

  // Integrate continuously, including slow movements and pauses.
  // Do not reset the angle when movement stops.
  doorAngle += rotationRate * dt;

  // The door cannot rotate past closed, so a negative angle is only
  // accumulated error. Clamping re-zeros the door every time it shuts.
  if (doorAngle < 0)
  {
    doorAngle = 0;
  }

  // Use different entry/exit thresholds to avoid flickering
  // near the boundary.
  if (estimatedClosed && fabsf(doorAngle) > 7.0f)
  {
    estimatedClosed = false;
    openedAt = millis();
    Serial.println("EVENT: Door opened (estimated)");
  }
  else if (!estimatedClosed &&
           fabsf(doorAngle) <= CLOSED_TOLERANCE_DEG)
  {
    estimatedClosed = true;
    Serial.println("EVENT: Door closed (estimated)");
  }
}

// Direction check. Only door-closed time goes into the trend, so a
// door opening never looks like the fridge warming up.
void updateTrend(float t, uint32_t now)
{
  if (!calibrated || !estimatedClosed)
  {
    // Door open: start a fresh trend after it closes. Keep the last
    // verdict so one door opening doesn't silence a real alarm.
    trendCount = 0;
    bucketSum = 0;
    bucketReadings = 0;
    return;
  }

  if (bucketReadings == 0)
  {
    bucketStart = now;
  }
  bucketSum += t;
  bucketReadings++;

  if (now - bucketStart < TREND_BUCKET_MS)
    return;

  // Bucket complete: store its average.
  float average = bucketSum / bucketReadings;
  bucketSum = 0;
  bucketReadings = 0;

  if (trendCount == TREND_POINTS)
  {
    for (int i = 1; i < TREND_POINTS; i++)
    {
      trendHistory[i - 1] = trendHistory[i];
    }
    trendCount--;
  }
  trendHistory[trendCount++] = average;

  if (trendCount < TREND_POINTS)
    return;

  float change = trendHistory[TREND_POINTS - 1] - trendHistory[0];
  tempRising = change > RISE_THRESHOLD_C;

  Serial.printf("Temperature trend: %+.2f C over %lu min%s\n",
                change,
                (unsigned long)((TREND_POINTS - 1) * TREND_BUCKET_MS / 60000UL),
                tempRising ? " (RISING)" : "");
}

// Warm backstop. Runs regardless of door state: an hour above the limit
// is far past any normal recovery from a door opening.
void updateWarmBackstop(float t, uint32_t now)
{
  if (t > SAFE_MAX_C)
  {
    if (!warmTiming)
    {
      warmTiming = true;
      warmSince = now;
    }
    tempTooWarm = now - warmSince >= WARM_ALARM_MS;
  }
  else if (t < SAFE_MAX_C - WARM_CLEAR_MARGIN_C)
  {
    // Small margin so jitter around the limit doesn't restart the timer.
    warmTiming = false;
    tempTooWarm = false;
  }
}

void processTemperature(float t, uint32_t now)
{
  updateTrend(t, now);
  updateWarmBackstop(t, now);

  bool shouldAlarm = tempRising || tempTooWarm;
  if (shouldAlarm != tempAlarm)
  {
    tempAlarm = shouldAlarm;

    if (!tempAlarm)
    {
      Serial.printf("Temperature alert cleared (%.2f C)\n", t);
    }
    else if (tempRising)
    {
      Serial.printf("ALERT: Temperature rising with door closed (%.2f C)\n", t);
    }
    else
    {
      Serial.printf("ALERT: Above %.1f C for 1+ hour (%.2f C)\n",
                    SAFE_MAX_C, t);
    }
  }
}

void setBuzzer(bool on)
{
  static bool buzzerOn = false;
  if (on == buzzerOn)
    return;

  buzzerOn = on;
  ledcWrite(BUZZER_CHANNEL, on ? 128 : 0); // 50% duty = tone, 0 = silent.
}

void updateBuzzer(uint32_t now)
{
  bool shouldAlarm = calibrated && !estimatedClosed &&
                     now - openedAt >= OPEN_ALARM_MS;

  if (shouldAlarm != alarmActive)
  {
    alarmActive = shouldAlarm;
    Serial.println(alarmActive
                       ? "ALERT: Door open for 10+ seconds!"
                       : "Alert cleared.");
  }

  if (alarmActive)
  {
    // Pulse the buzzer on and off.
    bool on = (now - openedAt) % BEEP_PERIOD_MS < BEEP_PERIOD_MS / 2;
    setBuzzer(on);
  }
  else if (tempAlarm)
  {
    // Short chirp every 2 seconds, distinct from the door alarm.
    bool on = now % TEMP_BEEP_PERIOD_MS < TEMP_BEEP_ON_MS;
    setBuzzer(on);
  }
  else
  {
    setBuzzer(false);
  }
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ_HZ, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0);

  bmpReady = bmp.begin(0x76);
  if (!bmpReady)
  {
    bmpReady = bmp.begin(0x77);
  }

  Serial.println(bmpReady ? "BMP280 ready."
                          : "BMP280 not found.");

  mpuReady = writeMpu(0x6B, 0x00); // Wake MPU.
  if (mpuReady)
  {
    mpuReady = writeMpu(0x1A, 0x03); // Low-pass filter.
  }
  if (mpuReady)
  {
    mpuReady = writeMpu(0x1B, 0x10); // Gyro +/-1000 dps.
  }

  if (mpuReady)
  {
    Serial.println("\nKeep the door CLOSED and STILL.");
    Serial.println("Measuring gyro offset for about 2 seconds...");
  }
  else
  {
    Serial.println("MPU6050 not found. Check wiring.");
  }

  lastSample = millis();
  lastPrint = lastSample;
  lastTemperature = lastSample;
}

void loop()
{
  uint32_t now = millis();
  bool pressed = buttonPressed(now);

  if (mpuReady && now - lastSample >= SAMPLE_MS)
  {
    float dt = (now - lastSample) / 1000.0f;
    lastSample = now;
    sampleMotion(dt);
  }

  if (pressed && mpuReady && biasReady)
  {
    if (!calibrated)
    {
      handleCalibrationPress();
    }
    else
    {
      // Only press this when the door is physically CLOSED.
      doorAngle = 0;
      estimatedClosed = true;
      Serial.println("Closed reference reset: 0 degrees.");
    }
  }

  updateBuzzer(now);

  if (calibrated && now - lastPrint >= PRINT_MS)
  {
    lastPrint = now;

    Serial.printf(
        "Door angle: %.1f deg | %s\n",
        doorAngle,
        estimatedClosed ? "CLOSED (estimated)" : "OPEN");
  }

  // Temperature measurement stays at one reading every 5 seconds.
  if (now - lastTemperature >= TEMP_MS)
  {
    lastTemperature = now;

    if (bmpReady)
    {
      float temperature = bmp.readTemperature();

      if (isfinite(temperature))
      {
        Serial.printf("Temperature: %.2f C\n", temperature);
        processTemperature(temperature, now);
      }
      else
      {
        Serial.println("Temperature read failed.");
      }
    }
    else
    {
      Serial.println("Temperature unavailable: BMP280 not found.");
    }
  }
}