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

// Upload progress, read by the OLED code in the main loop. Written only by
// the network task (single 32-bit writes, so no lock is needed).
struct CloudStatus {
  volatile bool wifi = false;
  volatile uint32_t okCount = 0;
  volatile uint32_t failCount = 0;
  volatile int lastCode = 0;        // HTTP status of the latest attempt (<= 0: network error)
  volatile uint32_t lastOkMs = 0;   // millis() of the latest success
  volatile uint32_t lastTookMs = 0; // how long the latest request took
  volatile bool signedIn = false;   // have a valid access token
  volatile uint32_t connects = 0;   // new TLS connections opened (a kept-alive connection adds none)
};
static CloudStatus cloudStatus;

// Random per-boot identifier so readings from different power cycles can be
// told apart (uptime_ms restarts at zero on every boot).
static char cloudBootId[9] = "";

// Readings waiting to be uploaded. Door/alarm changes must never be lost, so this
// holds several; if it is ever full the OLDEST reading is dropped.
static constexpr int CLOUD_QUEUE_DEPTH = 8;
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

  // One TLS connection is kept open between uploads: a fresh handshake on
  // every request cost 1-3 s of latency on the ESP32.
  static WiFiClientSecure client;
  static bool clientConfigured = false;
  if (!clientConfigured) {
    client.setCACert(cloudRootCertificates);
    client.setHandshakeTimeout(30);
    clientConfigured = true;
  }

  // The HTTPClient object must outlive the request: its destructor closes the
  // socket, which is what made every upload pay a full TLS handshake (~5 s).
  // Keeping it (and the client) static lets the connection stay open.
  static HTTPClient http;
  int code = 0;
  // A kept-open connection may have been closed by the server while idle. If the
  // first attempt on a reused connection fails, reconnect and try once more.
  for (int attempt = 0; attempt < 2; attempt++) {
    const bool reused = client.connected();
    const uint32_t startMs = millis();
    http.setReuse(true);
    http.setConnectTimeout(30000);
    http.setTimeout(8000);

    if (!http.begin(client, String(SUPABASE_URL) + path)) {
      return -1000;
    }

    http.addHeader("apikey", SUPABASE_PUBLISHABLE_KEY);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "keep-alive");

    if (authenticated) {
      http.addHeader(
        "Authorization",
        String("Bearer ") + cloudAccessToken
      );
      http.addHeader("Prefer", "return=minimal");
    }

    code = http.POST(body);

    if (code < 0 && reused && attempt == 0) {
      http.end();
      client.stop();
      continue;
    }
    if (!reused && code > 0) {
      cloudStatus.connects = cloudStatus.connects + 1;
      Serial.printf("[net] opened a new TLS connection (request took %lu ms)\n", (unsigned long)(millis() - startMs));
    }
    break;
  }

  // A successful insert ("Prefer: return=minimal") has no body. Calling
  // getString() on it can block until the 10 s HTTP timeout when the server
  // does not announce a length, which delayed every upload by ~10 s. Only read
  // the body when there is something to read (errors, and the sign-in reply).
  if (code > 0 && !(authenticated && (code == 201 || code == 204))) {
    response = http.getString();
  }

  http.end();
  if (cfg::LOG_EACH_UPLOAD) Serial.printf("[net] connection kept open: %s\n", client.connected() ? "yes" : "NO");
  // A failed request may leave a dead socket: drop it so the next one reconnects.
  if (code < 0 || code >= 400) client.stop();
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
    cloudStatus.signedIn = false;
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
  cloudStatus.signedIn = true;
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

// Fills one JSON object with a reading.
static void cloudFillJson(JsonObject document, const CloudReading& reading) {
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

}

// Uploads 1..n readings in ONE request (PostgREST accepts a JSON array), so a
// slow connection never makes readings pile up or get dropped.
static void cloudUploadReadings(const CloudReading* readings, int count) {
  DynamicJsonDocument array(count * 1100 + 64);
  JsonArray rows = array.to<JsonArray>();
  for (int i = 0; i < count; i++) cloudFillJson(rows.createNestedObject(), readings[i]);

  // owner_id and received_at use their database defaults.
  String body;
  serializeJson(array, body);

  String response;
  uint32_t startedMs = millis();
  int code = cloudPost(
    "/rest/v1/fridge_readings",
    body,
    true,
    response
  );
  uint32_t tookMs = millis() - startedMs;

  cloudStatus.lastCode = code;
  cloudStatus.lastTookMs = tookMs;
  if (code == 201) {
    cloudStatus.lastOkMs = millis();
    cloudStatus.okCount = cloudStatus.okCount + 1;
    if (cfg::LOG_EACH_UPLOAD)
      Serial.printf("Supabase: %d reading%s saved (%lu ms).\n", count, count == 1 ? "" : "s", (unsigned long)tookMs);
  } else {
    cloudStatus.failCount = cloudStatus.failCount + 1;
    Serial.printf("Supabase upload failed: %d (%lu ms)\n", code, (unsigned long)tookMs);
    
    if (code > 0) {
      Serial.print("Database error: ");
      Serial.println(response);
    } else {
      Serial.print("Network error: ");
      Serial.println(HTTPClient::errorToString(code));
    }

    if (code == 401) {
      cloudStatus.signedIn = false;
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
    cloudStatus.wifi = (WiFi.status() == WL_CONNECTED);
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

      cloudStatus.wifi = true;
      Serial.print("WiFi connected. IP: ");
      Serial.println(WiFi.localIP());

      // Correct time is required to verify certificate validity.
      configTime(0, 0, "pool.ntp.org", "time.google.com");
    }

    if (time(nullptr) < 1700000000) {
      static bool saidWaiting = false;
      if (!saidWaiting) { saidWaiting = true; Serial.println("Waiting for clock synchronization..."); }
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    bool tokenExpired =
        millis() - cloudTokenStarted >= cloudTokenLifetimeMs;

    if (cloudAccessToken.length() == 0 || tokenExpired) {
      if (!cloudAuthenticate()) {
        vTaskDelay(pdMS_TO_TICKS(5000));  // handshakes fail now and then on weak Wi-Fi: retry soon
        continue;
      }
    }

    CloudReading batch[CLOUD_QUEUE_DEPTH];
    int count = 0;

    if (xQueueReceive(
          cloudQueue,
          &batch[0],
          pdMS_TO_TICKS(1000)) == pdTRUE) {
      count = 1;
      // Anything that queued up while the last request was running goes in the same request.
      while (count < CLOUD_QUEUE_DEPTH && xQueueReceive(cloudQueue, &batch[count], 0) == pdTRUE) count++;

      cloudUploadReadings(batch, count);

      // Limit request frequency, including during failures: a short pause
      // on success, longer after an error so we do not hammer the server.
      vTaskDelay(pdMS_TO_TICKS(cloudStatus.lastCode == 201 ? cfg::UPLOAD_GAP_MS : 1000));
    }
  }
}

static void startCloudUpload() {
  snprintf(cloudBootId, sizeof(cloudBootId), "%08x", (unsigned)esp_random());

  // Keep the most recent readings if the network falls behind.
  // This is best-effort telemetry, NOT a durable event log: if the network
  // is down for a while, only the newest few readings are kept.
  cloudQueue = xQueueCreate(CLOUD_QUEUE_DEPTH, sizeof(CloudReading));

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
    if (xQueueSend(cloudQueue, &reading, 0) != pdTRUE) {
      CloudReading dropped;
      xQueueReceive(cloudQueue, &dropped, 0);  // full: discard the oldest, keep the newest
      xQueueSend(cloudQueue, &reading, 0);
    }
  }
}