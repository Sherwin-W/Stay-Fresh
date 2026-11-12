#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "config.h"
#include "secrets.h"

// PlatformIO embeds certs/roots.pem in the firmware.
extern const char cloudRootCertificates[]
    asm("_binary_certs_roots_pem_start");

// A copy of the sensor values, passed safely to the network task.
struct CloudReading {
  uint32_t uptimeMs;       // device clock when the snapshot was taken
  float temperatureC;      // NAN when unavailable
  float angleDeg;
  bool doorOpen;           // false for "unknown" too: use doorState
  bool alarmActive;
  uint32_t openDurationMs;

  // Extended fields: sent only when CLOUD_SEND_EXTENDED == 1 and the SQL
  // migration has been applied. The strings point at string literals.
  uint32_t seq;            // snapshot counter since boot
  int64_t measuredAtUnix;  // 0 when the clock is not NTP-synchronized yet
  const char* doorState;       // closed | open | unknown
  const char* closureSource;   // none | estimated | manual | reed
  const char* calibrationState;// warmup | bias | awaiting_moves | calibrated | fault
  const char* angleQuality;    // none | estimated | degraded
  bool sensorValid;
  uint32_t sinceReferenceMs;   // time since the angle was last zeroed
  uint32_t missedSamples;
  uint32_t biasAdjustments;
  bool reedEnabled;
  bool reedKnown;
  bool reedClosed;
};

// Random per-boot identifier so readings from different power cycles can be
// told apart (uptime_ms restarts at zero on every boot).
static char cloudBootId[9] = "";

static QueueHandle_t cloudQueue = nullptr;

static String cloudAccessToken;
static String cloudRefreshToken;
static uint32_t cloudTokenStarted = 0;
static uint32_t cloudTokenLifetimeMs = 0;

// HTTP requests run only in the network task.
// Never print the response from the authentication endpoint:
// it contains access and refresh tokens.
static int cloudPost(
    const String& path,
    const String& body,
    bool authenticated,
    String& response) {

  WiFiClientSecure client;
  client.setCACert(cloudRootCertificates);
  client.setHandshakeTimeout(12);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  if (!http.begin(client, String(SUPABASE_URL) + path)) {
    return -1000;
  }

  http.addHeader("apikey", SUPABASE_PUBLISHABLE_KEY);
  http.addHeader("Content-Type", "application/json");

  if (authenticated) {
    http.addHeader(
      "Authorization",
      String("Bearer ") + cloudAccessToken
    );
    http.addHeader("Prefer", "return=minimal");
  }

  int code = http.POST(body);

  if (code > 0) {
    response = http.getString();
  }

  http.end();
  return code;
}

static bool cloudAuthenticate() {
  bool refreshing = cloudRefreshToken.length() > 0;

  DynamicJsonDocument request(2048);

  if (refreshing) {
    request["refresh_token"] = cloudRefreshToken;
  } else {
    request["email"] = DEVICE_EMAIL;
    request["password"] = DEVICE_PASSWORD;
  }

  String body;
  serializeJson(request, body);

  String path = refreshing
      ? "/auth/v1/token?grant_type=refresh_token"
      : "/auth/v1/token?grant_type=password";

  String response;
  int code = cloudPost(path, body, false, response);

  if (code != 200) {
    Serial.printf("Supabase sign-in/refresh failed: %d\n", code);

    // An invalid refresh token requires a fresh password login.
    if (refreshing && (code == 400 || code == 401)) {
      cloudRefreshToken = "";
    }

    cloudAccessToken = "";
    return false;
  }

  DynamicJsonDocument result(16384);

  if (deserializeJson(result, response)) {
    Serial.println("Supabase: could not parse sign-in response.");
    return false;
  }

  String access = result["access_token"].as<String>();
  String refresh = result["refresh_token"].as<String>();
  uint32_t expiresIn = result["expires_in"] | 3600UL;

  if (access.length() == 0 || refresh.length() == 0) {
    Serial.println("Supabase: sign-in response missing tokens.");
    return false;
  }

  cloudAccessToken = access;
  cloudRefreshToken = refresh;
  cloudTokenStarted = millis();

  // Refresh before expiry. Cap interval to keep millis arithmetic safe.
  uint32_t refreshAfterSeconds =
      expiresIn > 60 ? expiresIn - 60 : 1;

  if (refreshAfterSeconds > 86400UL) {
    refreshAfterSeconds = 86400UL;
  }

  cloudTokenLifetimeMs = refreshAfterSeconds * 1000UL;

  Serial.println(refreshing
      ? "Supabase session refreshed."
      : "Supabase signed in.");

  return true;
}

static void cloudUploadReading(const CloudReading& reading) {
  StaticJsonDocument<1280> document;

  document["device_id"] = DEVICE_ID;
  document["uptime_ms"] = reading.uptimeMs;
  document["angle_deg"] = reading.angleDeg;
  document["door_open"] = reading.doorOpen;
  document["alarm_active"] = reading.alarmActive;
  document["open_duration_ms"] = reading.openDurationMs;

  if (isfinite(reading.temperatureC)) {
    document["temperature_c"] = reading.temperatureC;
  } else {
    document["temperature_c"] = nullptr;
  }

#if CLOUD_SEND_EXTENDED
  document["boot_id"] = cloudBootId;
  document["seq"] = reading.seq;
  document["firmware_version"] = STAYFRESH_FW_VERSION;
  document["door_state"] = reading.doorState;
  document["closure_source"] = reading.closureSource;
  document["calibration_state"] = reading.calibrationState;
  document["angle_quality"] = reading.angleQuality;
  document["sensor_valid"] = reading.sensorValid;
  document["since_reference_ms"] = reading.sinceReferenceMs;
  document["missed_samples"] = reading.missedSamples;
  document["bias_adjustments"] = reading.biasAdjustments;
  document["reed_enabled"] = reading.reedEnabled;
  if (reading.reedEnabled && reading.reedKnown) {
    document["reed_closed"] = reading.reedClosed;
  } else {
    document["reed_closed"] = nullptr;
  }

  // measured_at only when the clock was synchronized at measurement time.
  // received_at (server arrival time) is always filled in by the database.
  char measuredAt[24];
  if (reading.measuredAtUnix > 1700000000LL) {
    time_t t = (time_t)reading.measuredAtUnix;
    struct tm utc;
    gmtime_r(&t, &utc);
    strftime(measuredAt, sizeof(measuredAt), "%Y-%m-%dT%H:%M:%SZ", &utc);
    document["measured_at"] = measuredAt;
  } else {
    document["measured_at"] = nullptr;
  }
#endif

  // owner_id and received_at use their database defaults.
  String body;
  serializeJson(document, body);

  String response;
  int code = cloudPost(
    "/rest/v1/fridge_readings",
    body,
    true,
    response
  );

  if (code == 201) {
    Serial.println("Supabase: reading saved.");
  } else {
    Serial.printf("Supabase upload failed: %d\n", code);
    
    if (code > 0) {
      Serial.print("Database error: ");
      Serial.println(response);
    } else {
      Serial.print("Network error: ");
      Serial.println(HTTPClient::errorToString(code));
    }

    if (code == 401) {
      cloudAccessToken = "";
    }

    if (code == 403) {
      Serial.println("Check table grants and RLS policies.");
    }
  }
}

static void cloudTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi: connecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

      uint32_t started = millis();

      while (WiFi.status() != WL_CONNECTED &&
             millis() - started < 15000UL) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }

      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi unavailable; sensors keep running.");
        vTaskDelay(pdMS_TO_TICKS(10000));
        continue;
      }

      Serial.print("WiFi connected. IP: ");
      Serial.println(WiFi.localIP());

      // Correct time is required to verify certificate validity.
      configTime(0, 0, "pool.ntp.org", "time.google.com");
    }

    if (time(nullptr) < 1700000000) {
      Serial.println("Waiting for clock synchronization...");
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    bool tokenExpired =
        millis() - cloudTokenStarted >= cloudTokenLifetimeMs;

    if (cloudAccessToken.length() == 0 || tokenExpired) {
      if (!cloudAuthenticate()) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        continue;
      }
    }

    CloudReading reading;

    if (xQueueReceive(
          cloudQueue,
          &reading,
          pdMS_TO_TICKS(1000)) == pdTRUE) {

      cloudUploadReading(reading);

      // Limit request frequency, including during failures.
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void startCloudUpload() {
  snprintf(cloudBootId, sizeof(cloudBootId), "%08x", (unsigned)esp_random());

  // Keep the most recent reading if the network falls behind.
  // This is best-effort telemetry, NOT a durable event log: if the network
  // is down for a minute, the readings in between are not stored or replayed.
  cloudQueue = xQueueCreate(1, sizeof(CloudReading));

  if (cloudQueue == nullptr) {
    Serial.println("Cloud upload: queue allocation failed.");
    return;
  }

  BaseType_t result = xTaskCreate(
    cloudTask,
    "SupabaseUpload",
    16384,
    nullptr,
    1,
    nullptr
  );

  if (result != pdPASS) {
    vQueueDelete(cloudQueue);
    cloudQueue = nullptr;
    Serial.println("Cloud upload: task creation failed.");
  }
}

static void queueCloudReading(const CloudReading& reading) {
  if (cloudQueue != nullptr) {
    xQueueOverwrite(cloudQueue, &reading);
  }
}