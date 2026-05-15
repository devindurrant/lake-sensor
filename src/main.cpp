#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_sleep.h>

#include "secrets.h"

// ============================================================================
// DEBUG LOGGING
// Compile with -DDEBUG (see platformio.ini debug env) to enable serial output.
// Production builds leave DEBUG undefined — zero serial overhead.
// ============================================================================
#ifdef DEBUG
  #define LOG(...)  Serial.print(__VA_ARGS__)
  #define LOGLN(...)  Serial.println(__VA_ARGS__)
  #define LOGF(...)   Serial.printf(__VA_ARGS__)
#else
  #define LOG(...)
  #define LOGLN(...)
  #define LOGF(...)
#endif

// ============================================================================
// HARDWARE CONFIG
// ============================================================================
// Seeed Studio XIAO ESP32-C3 pin mapping
// NOTE: XIAO D-pin labels != GPIO numbers. D0=GPIO2, D4=GPIO6, D5=GPIO7.
static constexpr uint8_t ONE_WIRE_PIN = 2; // D0 = GPIO2
static constexpr uint8_t I2C_SDA_PIN = 6;  // D4 = GPIO6
static constexpr uint8_t I2C_SCL_PIN = 7;  // D5 = GPIO7

// M5Stack U162 settings
static constexpr uint8_t U162_ADDR        = 0x55;
static constexpr uint8_t U162_CURRENT_REG = 0x20; // Pre-scaled: raw = current_mA * 100
static constexpr uint8_t U162_ADC_REG     = 0x00; // Raw 16-bit ADC average (diagnostic use)

// DS18B20 settings
static constexpr size_t MAX_TEMP_SENSORS = 4;
static constexpr uint8_t TEMP_RES_BITS = 12;

// Wake cycle: exactly 15 minutes
static constexpr uint64_t SLEEP_DURATION_US = 15ULL * 60ULL * 1000000ULL;

// ============================================================================
// CURRENT -> DEPTH CONVERSION CONSTANTS
// ============================================================================
// The U162 STM32 firmware computes current internally and stores current_mA*100
// in register 0x20 as a uint16_t (little-endian). No shunt math needed here.
// Example: 4.00 mA -> raw=400, 20.00 mA -> raw=2000.
//
// 4-20mA pressure transducer maps to 0-5 m depth
static constexpr float CURRENT_MIN_MA = 4.0f;
static constexpr float CURRENT_MAX_MA = 20.0f;
static constexpr float DEPTH_MIN_M = 0.0f;
static constexpr float DEPTH_MAX_M = 5.0f;

// Networking behavior
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t HTTP_TIMEOUT_MS = 10000;

// ============================================================================
// TLS: ISRG Root X1 (Let's Encrypt root CA used by nabu.casa)
// Expires: 2035-06-04. Update when rotating.
// ============================================================================
static const char ISRG_ROOT_X1_PEM[] PROGMEM = R"(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)";

// ============================================================================
// DS18B20 ROM TABLE
// Maps each fixed depth label to its sensor's burned-in 64-bit ROM, captured
// during bench testing on 2026-05-14. Wiring/discovery order is irrelevant --
// readings are looked up by ROM so each label always tracks the same physical
// probe. To replace a probe, capture the new ROM (any sketch that prints
// tempBus.getAddress() output) and edit the entry below.
// ============================================================================
struct DepthSensor {
  const char* label;
  DeviceAddress rom;
};

static const DepthSensor TEMP_POSITIONS[MAX_TEMP_SENSORS] = {
  {"surface", {0x28, 0xFC, 0xC1, 0x94, 0x00, 0x00, 0x00, 0x11}},
  {"mid",     {0x28, 0x7E, 0x4A, 0x35, 0x00, 0x00, 0x00, 0x08}},
  {"deep",    {0x28, 0x8D, 0xEE, 0x37, 0x00, 0x00, 0x00, 0x08}},
  {"bottom",  {0x28, 0x06, 0xD5, 0x36, 0x00, 0x00, 0x00, 0x61}},
};

// ============================================================================
// GLOBALS
// ============================================================================

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature tempBus(&oneWire);

float temperaturesC[MAX_TEMP_SENSORS];

uint16_t rawU162 = 0;   // raw value from U162 current register (current_mA * 100)
float currentmA = NAN;
float depthMeters = NAN;

// ============================================================================
// UTILITY
// ============================================================================

float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

// ============================================================================
// POWER
// ============================================================================

void goToDeepSleep() {
  if (WiFi.isConnected()) {
    WiFi.disconnect(true, true);
  }
  WiFi.mode(WIFI_OFF);

#ifndef DEBUG
  esp_deep_sleep(SLEEP_DURATION_US);
  // execution never continues past here in production
#endif
  // In DEBUG builds: fall through to loop(), which handles the retry delay.
}

// ============================================================================
// NETWORKING
// ============================================================================

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(150);
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOGLN("WiFi connected.");
    return true;
  }

  LOGLN("WiFi connect timeout.");
  return false;
}

// ============================================================================
// SENSORS
// ============================================================================

void readTemperatures() {
  for (size_t i = 0; i < MAX_TEMP_SENSORS; i++) {
    temperaturesC[i] = NAN;
  }

  tempBus.begin();
  tempBus.setResolution(TEMP_RES_BITS);

  const uint8_t found = tempBus.getDeviceCount();
  LOGF("DS18B20 bus reports: %u sensor(s)\n", found);
  if (found != MAX_TEMP_SENSORS) {
    LOGF("WARN: expected %u sensors, bus reports %u\n",
         static_cast<unsigned>(MAX_TEMP_SENSORS), found);
  }

  tempBus.requestTemperatures(); // ~750 ms at 12-bit

  for (size_t i = 0; i < MAX_TEMP_SENSORS; i++) {
    const float t = tempBus.getTempC(TEMP_POSITIONS[i].rom);
    if (t == DEVICE_DISCONNECTED_C) {
      temperaturesC[i] = NAN;
      LOGF("%s: MISSING (ROM did not respond)\n", TEMP_POSITIONS[i].label);
    } else {
      temperaturesC[i] = t;
      LOGF("%s: %.2f C\n", TEMP_POSITIONS[i].label, t);
    }
  }
}

// Reads register 0x20 (CURRENT_REG) from the U162.
// The U162 STM32 stores current_mA*100 as a uint16_t little-endian value.
bool readU162Current(uint16_t &outRaw) {
  Wire.beginTransmission(U162_ADDR);
  Wire.write(U162_CURRENT_REG);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t req = 2;
  if (Wire.requestFrom(static_cast<int>(U162_ADDR), static_cast<int>(req)) != req) {
    return false;
  }

  const uint8_t lsb = Wire.read();
  const uint8_t msb = Wire.read();
  outRaw = (static_cast<uint16_t>(msb) << 8) | lsb;
  return true;
}

void readPressureAndDepth() {
  rawU162 = 0;
  currentmA = NAN;
  depthMeters = NAN;

  if (!readU162Current(rawU162)) {
    LOGLN("U162 read failed.");
    return;
  }

  // Register 0x20 holds current_mA * 100 (e.g. 400 = 4.00 mA, 2000 = 20.00 mA)
  currentmA = static_cast<float>(rawU162) / 100.0f;

  const float normalized = (currentmA - CURRENT_MIN_MA) / (CURRENT_MAX_MA - CURRENT_MIN_MA);
  depthMeters = DEPTH_MIN_M + normalized * (DEPTH_MAX_M - DEPTH_MIN_M);
  depthMeters = clampf(depthMeters, DEPTH_MIN_M, DEPTH_MAX_M);

  LOGF("U162 raw=%u (%.2f mA), depth=%.3f m\n", rawU162, currentmA, depthMeters);
}

// ============================================================================
// TELEMETRY
// ============================================================================

String buildJsonPayload() {
  JsonDocument doc;

  for (size_t i = 0; i < MAX_TEMP_SENSORS; i++) {
    const String key = String(TEMP_POSITIONS[i].label) + "_c";
    if (isnan(temperaturesC[i])) {
      doc[key] = nullptr;
    } else {
      doc[key] = temperaturesC[i];
    }
  }

  doc["u162_raw"] = rawU162;
  if (isnan(currentmA)) {
    doc["current_mA"] = nullptr;
  } else {
    doc["current_mA"] = currentmA;
  }

  if (isnan(depthMeters)) {
    doc["depth_m"] = nullptr;
  } else {
    doc["depth_m"] = depthMeters;
  }

  doc["rssi_dbm"] = WiFi.RSSI();
  doc["uptime_ms"] = millis();

  String payload;
  serializeJson(doc, payload);
  return payload;
}

bool postWebhook(const String &payload) {
  WiFiClientSecure secureClient;
  secureClient.setCACert(ISRG_ROOT_X1_PEM);

  HTTPClient http;
  if (!http.begin(secureClient, WEBHOOK_URL)) {
    LOGLN("HTTP begin failed.");
    return false;
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  const int code = http.POST(
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(payload.c_str())),
      payload.length());
  LOGF("Webhook POST response code: %d\n", code);

  http.end();
  return (code >= 200 && code < 300);
}

// ============================================================================
// ENTRY POINTS
// ============================================================================

void setup() {
#ifdef DEBUG
  Serial.begin(115200);
  // Give the host OS time to re-enumerate the USB CDC port before any output.
  // Also prevents the monitor from missing the first lines if opened just after flash.
  delay(2000);
  LOGLN("\n=== Lake Sensor Wake Cycle ===");
#endif

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  readTemperatures();
  readPressureAndDepth();

  if (!connectToWiFi()) {
    goToDeepSleep();
    return;
  }

  const String payload = buildJsonPayload();
  LOGF("Payload: %s\n", payload.c_str());

  const bool ok = postWebhook(payload);
  LOGLN(ok ? "Webhook POST succeeded." : "Webhook POST failed.");

  goToDeepSleep();
}

void loop() {
#ifdef DEBUG
  // In production the device never reaches loop() — it deep sleeps from setup().
  // In debug builds, re-run the full sense→transmit cycle every 15 minutes
  // while keeping USB alive.
  static constexpr uint32_t DEBUG_INTERVAL_MS = (SLEEP_DURATION_US / 1000ULL);
  LOGLN("Waiting 15 min before next cycle (DEBUG — no deep sleep)...");
  delay(DEBUG_INTERVAL_MS);
  LOGLN("\n=== Lake Sensor Wake Cycle ===");
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  readTemperatures();
  readPressureAndDepth();
  if (connectToWiFi()) {
    const String payload = buildJsonPayload();
    LOGF("Payload: %s\n", payload.c_str());
    const bool ok = postWebhook(payload);
    LOGLN(ok ? "Webhook POST succeeded." : "Webhook POST failed.");
  }
  goToDeepSleep(); // no-op in DEBUG, just disconnects WiFi
#else
  // Should never reach here in production.
  esp_deep_sleep(SLEEP_DURATION_US);
#endif
}
