#pragma once
// ------------------------------------------------------------------
// Tiny status display (SSD1306 128x64, 21 columns x 8 rows at text size 1).
// Shows what you would otherwise read on the Serial Monitor, simplified:
// calibration steps, door/alarm, temperature, Wi-Fi and upload results.
// Hardware glue only; compiled out when USE_OLED == 0.
// ------------------------------------------------------------------
#include <Arduino.h>
#include "config.h"

#if USE_OLED
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 oled(128, 64, &SPI, cfg::OLED_DC, cfg::OLED_RST, cfg::OLED_CS);
static bool oledReady = false;
static char oledMsg[22] = "";
static uint32_t oledMsgMs = 0;
static uint32_t oledLastDraw = 0;

// What the screen needs to know. Filled in by main.cpp.
struct OledView {
  bool calibrating;       // bias / movement steps
  const char* calTitle;   // e.g. "CALIBRATE 1/4"
  const char* calLine1;   // e.g. "OPEN the door"
  const char* calLine2;   // e.g. "then press button"
  float tempC;            // NAN = no reading
  const char* door;       // "OPEN" | "CLOSED" | "UNKNOWN"
  uint32_t openSec;
  bool alarm;
  bool wifi;
  bool uploadKnown;       // false until the first upload attempt
  bool uploadOk;
  uint32_t uploadAgeSec;  // seconds since last successful upload
  int uploadCode;         // last HTTP code (<=0 network error)
  const char* tempAlert;  // nullptr = none; else e.g. "TEMP RISING" (stays until cleared)
};

// One short log line (max 21 chars), shown for OLED_MSG_MS.
static void oledLog(const char* msg) {
  strncpy(oledMsg, msg, sizeof(oledMsg) - 1);
  oledMsg[sizeof(oledMsg) - 1] = 0;
  oledMsgMs = millis();
}

static bool oledBegin() {
  SPI.begin();  // default VSPI pins: SCK 18, MOSI 23
  oledReady = oled.begin(SSD1306_SWITCHCAPVCC);
  if (!oledReady) return false;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("Stay-Fresh");
  oled.println(STAYFRESH_FW_VERSION);
  oled.display();
  return true;
}

static void oledDraw(const OledView& v, bool force = false) {
  if (!oledReady) return;
  uint32_t now = millis();
  if (!force && now - oledLastDraw < cfg::OLED_REFRESH_MS) return;
  oledLastDraw = now;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  char line[24];

  if (v.calibrating) {
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.print(v.calTitle);
    oled.setTextSize(2);
    oled.setCursor(0, 16);
    oled.print(v.calLine1);
    oled.setTextSize(1);
    oled.setCursor(0, 38);
    oled.print(v.calLine2);
  } else {
    // Row 0: connectivity
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    if (!v.wifi) snprintf(line, sizeof(line), "WiFi --  no upload");
    else if (!v.uploadKnown) snprintf(line, sizeof(line), "WiFi OK  connecting");
    else if (v.uploadOk) snprintf(line, sizeof(line), "WiFi OK  Sent %lus ago", (unsigned long)v.uploadAgeSec);
    else snprintf(line, sizeof(line), "WiFi OK  Send FAIL %d", v.uploadCode);
    oled.print(line);

    // Row 1: temperature
    oled.setTextSize(2);
    oled.setCursor(0, 14);
    if (isfinite(v.tempC)) { snprintf(line, sizeof(line), "%.1fC", v.tempC); oled.print(line); }
    else oled.print("-- C");

    // Row 2: door / alarm (inverted block when the alarm is on)
    oled.setCursor(0, 32);
    if (v.alarm) {
      oled.fillRect(0, 31, 128, 17, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
      snprintf(line, sizeof(line), "ALARM %lus", (unsigned long)v.openSec);
    } else if (strcmp(v.door, "OPEN") == 0) {
      snprintf(line, sizeof(line), "OPEN %lus", (unsigned long)v.openSec);
    } else {
      snprintf(line, sizeof(line), "%s", v.door);
    }
    oled.print(line);
    oled.setTextColor(SSD1306_WHITE);

  }

  // Bottom row: last log line (calibration result, upload event, door event...)
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 56);
  if (oledMsg[0] && now - oledMsgMs < cfg::OLED_MSG_MS) oled.print(oledMsg);
  else if (v.tempAlert) oled.print(v.tempAlert);

  // Sending the frame over I2C blocks for ~90 ms and would delay the gyro
  // sampling, so only do it when the picture actually changed.
  static uint32_t lastHash = 0;
  const uint8_t* buf = oled.getBuffer();
  uint32_t h = 2166136261u;
  for (int i = 0; i < 128 * 64 / 8; i++) h = (h ^ buf[i]) * 16777619u;
  if (h != lastHash || force) {
    lastHash = h;
    oled.display();
  }
}
#else
struct OledView {
  bool calibrating; const char* calTitle; const char* calLine1; const char* calLine2;
  float tempC; const char* door; uint32_t openSec; bool alarm; bool wifi;
  bool uploadKnown; bool uploadOk; uint32_t uploadAgeSec; int uploadCode; const char* tempAlert;
};
static void oledLog(const char*) {}
static bool oledBegin() { return false; }
static void oledDraw(const OledView&, bool = false) {}
#endif
