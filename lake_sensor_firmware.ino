/**
 * Lake Level and Temperature Monitor
 * 
 * Hardware: Seeed Studio XIAO ESP32-C3
 * Sensors:
 *   - 4x DS18B20 temperature sensors on 1-Wire bus (Pin D0)
 *   - M5Stack U162 4-20mA I2C unit (SDA=D4, SCL=D5) reading DFRobot pressure transducer
 * 
 * Features:
 *   - Battery-friendly deep sleep operation (15-minute intervals)
 *   - MQTT telemetry to Home Assistant
 *   - Home Assistant MQTT Discovery support
 *   - Robust error handling for sensor failures
 * 
 * @author Embedded C++ Developer
 * @version 1.0
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <esp_sleep.h>

// ============================================================================
// CONFIGURATION - UPDATE THESE VALUES FOR YOUR SETUP
// ============================================================================

// WiFi Configuration
#define WIFI_SSID         "your_wifi_ssid"
#define WIFI_PASSWORD     "your_wifi_password"

// MQTT Configuration
#define MQTT_SERVER       "192.168.1.100"      // Your MQTT broker IP
#define MQTT_PORT         1883
#define MQTT_USER         ""                   // Optional MQTT username
#define MQTT_PASSWORD     ""                   // Optional MQTT password

// Device Configuration
#define DEVICE_NAME       "lake-sensor"
#define TELEMETRY_TOPIC   "lake/telemetry"
#define DISCOVERY_PREFIX  "homeassistant"      // Home Assistant discovery prefix

// Timing Configuration
#define WIFI_CONNECT_TIMEOUT_MS   5000          // 5 seconds to connect to WiFi
#define MQTT_CONNECT_TIMEOUT_MS   5000          // 5 seconds to connect to MQTT
#define SLEEP_DURATION_MS         (15UL * 60UL * 1000UL)  // 15 minutes in milliseconds

// ============================================================================
// HARDWARE PIN DEFINITIONS
// ============================================================================

// Seeed XIAO ESP32-C3 Pin Mapping
#define ONE_WIRE_PIN          0   // D0 - 1-Wire bus for DS18B20 sensors
#define I2C_SDA_PIN           4   // D4 - I2C SDA
#define I2C_SCL_PIN           5   // D5 - I2C SCL

// Sensor Addresses
#define U162_I2C_ADDRESS      0x55    // M5Stack U162 I2C address
#define U162_CURRENT_REGISTER 0x20    // Register to read current value (16-bit LE)

// ============================================================================
// SENSOR CONFIGURATION
// ============================================================================

#define MAX_DS18B20_SENSORS   4       // Number of DS18B20 sensors expected
#define DS18B20_RESOLUTION    12      // 12-bit resolution (~0.0625°C precision)

// Temperature sensor labels (location-based)
const char* TEMP_LABELS[MAX_DS18B20_SENSORS] = {
    "surface",
    "mid",
    "deep",
    "bottom"
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Sensor data storage
float temperatures[MAX_DS18B20_SENSORS];
uint8_t tempAddresses[MAX_DS18B20_SENSORS][8];
int sensorsFound = 0;

float current_mA = 0.0;
float depth_meters = 0.0;
bool i2cDevicePresent = false;

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

void setupWiFi();
bool connectToWiFi();
void setupMQTT();
bool connectToMQTT();
void readTemperatureSensors();
void readPressureSensor();
void publishTelemetry();
void publishDiscoveryConfig();
void goToDeepSleep();
void printHex(const uint8_t* addr, int len);

// ============================================================================
// SETUP
// ============================================================================

void setup() {
    // Initialize serial for debugging (optional, can be removed for production)
    Serial.begin(115200);
    Serial.println("\n=== Lake Sensor Monitor Starting ===");

    // Initialize I2C bus with explicit SDA/SCL pins
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.println("I2C bus initialized");

    // Initialize 1-Wire bus
    sensors.begin();
    Serial.println("1-Wire bus initialized");

    // Read sensor data
    Serial.println("\n--- Reading Sensors ---");
    readTemperatureSensors();
    readPressureSensor();

    // Connect to WiFi
    Serial.println("\n--- Connecting to WiFi ---");
    setupWiFi();
    if (!connectToWiFi()) {
        Serial.println("WiFi connection failed. Going to sleep.");
        goToDeepSleep();
        return;
    }
    Serial.println("WiFi connected successfully");

    // Connect to MQTT
    Serial.println("\n--- Connecting to MQTT ---");
    setupMQTT();
    if (!connectToMQTT()) {
        Serial.println("MQTT connection failed. Going to sleep.");
        goToDeepSleep();
        return;
    }
    Serial.println("MQTT connected successfully");

    // Publish Home Assistant discovery configuration (once per boot)
    publishDiscoveryConfig();

    // Publish telemetry data
    Serial.println("\n--- Publishing Telemetry ---");
    publishTelemetry();

    // Disconnect and go to sleep
    Serial.println("\n--- Going to Deep Sleep ---");
    goToDeepSleep();
}

// ============================================================================
// LOOP (Should never reach here due to deep sleep)
// ============================================================================

void loop() {
    // If we somehow get here, go to sleep
    goToDeepSleep();
}

// ============================================================================
// WIFI FUNCTIONS
// ============================================================================

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // Disable WiFi sleep for faster connection
}

bool connectToWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
        if (millis() - startTime > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("WiFi connection timeout");
            return false;
        }
    }

    Serial.print("Connected! IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
}

// ============================================================================
// MQTT FUNCTIONS
// ============================================================================

void setupMQTT() {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    
    if (strlen(MQTT_USER) > 0) {
        mqttClient.setCredentials(MQTT_USER, MQTT_PASSWORD);
    }
    
    // Set callback (optional, for receiving messages)
    mqttClient.setCallback(mqttCallback);
}

bool connectToMQTT() {
    String clientId = String(DEVICE_NAME) + "_" + String(esp_random() & 0xFFFF);
    
    Serial.print("Connecting to MQTT as: ");
    Serial.println(clientId);

    uint32_t startTime = millis();
    while (!mqttClient.connected()) {
        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("MQTT Connected");
            return true;
        } else {
            Serial.print("MQTT connect failed, rc: ");
            Serial.println(mqttClient.state());
            delay(500);
            
            if (millis() - startTime > MQTT_CONNECT_TIMEOUT_MS) {
                Serial.println("MQTT connection timeout");
                return false;
            }
        }
    }
    return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    // Optional: Handle incoming MQTT messages
    // Currently not used in this firmware
}

// ============================================================================
// TEMPERATURE SENSOR FUNCTIONS
// ============================================================================

void readTemperatureSensors() {
    Serial.println("Scanning for DS18B20 sensors...");
    
    // Clear previous addresses
    for (int i = 0; i < MAX_DS18B20_SENSORS; i++) {
        for (int j = 0; j < 8; j++) {
            tempAddresses[i][j] = 0;
        }
        temperatures[i] = -999.0;  // Invalid temperature marker
    }
    
    // Search for sensors on the bus
    sensorsFound = sensors.getAddressCount();
    Serial.print("Found ");
    Serial.print(sensorsFound);
    Serial.println(" DS18B20 sensor(s)");
    
    if (sensorsFound == 0) {
        Serial.println("WARNING: No DS18B20 sensors found on 1-Wire bus!");
        return;
    }
    
    // Set resolution for all sensors
    Serial.println("Setting sensor resolution to 12-bit...");
    sensors.setResolution(DS18B20_RESOLUTION);
    
    // Get addresses and request temperatures
    int addrCount = sensors.getAddressCount();
    for (int i = 0; i < MAX_DS18B20_SENSORS && i < addrCount; i++) {
        sensors.getAddress(tempAddresses[i], i);
        Serial.print("Sensor ");
        Serial.print(i + 1);
        Serial.print(" (");
        Serial.print(TEMP_LABELS[i]);
        Serial.print("): ");
        printHex(tempAddresses[i], 8);
        
        // Request temperature conversion
        sensors.requestTemperatures();
        break;  // One request converts all sensors
    }
    
    // Read temperatures
    Serial.println("\nReading temperatures...");
    sensors.requestTemperatures();  // Ensure conversion is done
    
    for (int i = 0; i < MAX_DS18B20_SENSORS && i < addrCount; i++) {
        temperatures[i] = sensors.getTempCByIndex(i);
        
        // Check for error conditions (-127 = CRC error, -128 = no device)
        if (temperatures[i] == -127.0 || temperatures[i] == -128.0) {
            Serial.print("ERROR: Sensor ");
            Serial.print(i + 1);
            Serial.print(" (");
            Serial.print(TEMP_LABELS[i]);
            Serial.println(") returned invalid reading (-127 or -128)");
            temperatures[i] = -999.0;  // Mark as invalid
        } else {
            Serial.print("  ");
            Serial.print(TEMP_LABELS[i]);
            Serial.print(": ");
            Serial.print(temperatures[i], 2);
            Serial.println(" °C");
        }
    }
    
    if (sensorsFound < MAX_DS18B20_SENSORS) {
        Serial.print("WARNING: Expected ");
        Serial.print(MAX_DS18B20_SENSORS);
        Serial.print(" sensors, but found only ");
        Serial.println(sensorsFound);
    }
}

// ============================================================================
// PRESSURE SENSOR FUNCTIONS
// ============================================================================

void readPressureSensor() {
    Serial.println("Reading M5Stack U162 4-20mA sensor...");
    
    i2cDevicePresent = false;
    current_mA = -999.0;
    depth_meters = -999.0;
    
    // Check if device is present at expected address
    Wire.beginTransmission(U162_I2C_ADDRESS);
    if (Wire.endTransmission() != 0) {
        Serial.println("WARNING: M5Stack U162 not found at I2C address 0x55!");
        Serial.println("Check wiring or if device is powered.");
        return;
    }
    i2cDevicePresent = true;
    Serial.println("M5Stack U162 detected");
    
    // Read current value from register 0x20 (16-bit little-endian)
    // The U162 returns current * 100 (e.g., 4.00mA = 400, 20.00mA = 2000)
    Wire.beginTransmission(U162_I2C_ADDRESS);
    Wire.write(U162_CURRENT_REGISTER);  // Register for current value
    if (Wire.endTransmission() != 0) {
        Serial.println("ERROR: Failed to write register address to U162");
        return;
    }
    
    Wire.requestFrom(U162_I2C_ADDRESS, (byte)2);  // Read 2 bytes
    
    if (Wire.available() >= 2) {
        uint8_t lsb = Wire.read();
        uint8_t msb = Wire.read();
        
        // Combine bytes (little-endian format)
        uint16_t rawValue = (msb << 8) | lsb;
        
        Serial.print("Raw value from U162: ");
        Serial.println(rawValue);
        
        // Convert to mA: The U162 returns current * 100
        // Example: 4.00mA -> 400, 20.00mA -> 2000
        current_mA = (float)rawValue / 100.0;
        
        Serial.print("Current: ");
        Serial.print(current_mA, 2);
        Serial.println(" mA");
        
        // Convert 4-20mA to 0-5 meter depth
        // Formula: depth = (current - 4) * (5 - 0) / (20 - 4)
        //         depth = (current - 4) * 5 / 16
        //         depth = (current - 4) * 0.3125
        //
        // This is a linear mapping:
        //   4mA  -> 0.0 meters (empty)
        //   12mA -> 2.5 meters (half full)
        //   20mA -> 5.0 meters (full)
        
        if (current_mA >= 4.0 && current_mA <= 20.0) {
            depth_meters = (current_mA - 4.0) * 5.0 / 16.0;
        } else if (current_mA < 4.0) {
            // Below 4mA - could indicate sensor fault or empty below range
            depth_meters = 0.0;
            Serial.println("WARNING: Current below 4mA range, clamping depth to 0");
        } else {
            // Above 20mA - could indicate sensor fault or over-range
            depth_meters = 5.0;
            Serial.println("WARNING: Current above 20mA range, clamping depth to 5");
        }
        
        Serial.print("Depth: ");
        Serial.print(depth_meters, 3);
        Serial.println(" meters");
    } else {
        Serial.println("ERROR: Failed to read data from U162");
    }
}

// ============================================================================
// TELEMETRY PUBLISHING FUNCTIONS
// ============================================================================

void publishTelemetry() {
    // Build JSON payload manually (no ArduinoJson dependency)
    // Format: {"surface":22.5,"mid":21.8,"deep":20.1,"bottom":19.5,"current_mA":12.5,"depth_m":2.344}
    
    String json = "{";
    json += "\"surface\":";
    json += String(temperatures[0], 2);
    json += ",";
    json += "\"mid\":";
    json += String(temperatures[1], 2);
    json += ",";
    json += "\"deep\":";
    json += String(temperatures[2], 2);
    json += ",";
    json += "\"bottom\":";
    json += String(temperatures[3], 2);
    json += ",";
    json += "\"current_mA\":";
    json += String(current_mA, 2);
    json += ",";
    json += "\"depth_m\":";
    json += String(depth_meters, 3);
    json += "}";
    
    Serial.print("Publishing to ");
    Serial.print(TELEMETRY_TOPIC);
    Serial.print(": ");
    Serial.println(json);
    
    mqttClient.publish(TELEMETRY_TOPIC, json.c_str(), false);
}

void publishDiscoveryConfig() {
    // Publish Home Assistant MQTT Discovery configuration
    // This allows automatic sensor discovery in Home Assistant
    
    String discoveryPrefix = String(DISCOVERY_PREFIX) + "/sensor";
    String deviceConfig = "{";
    deviceConfig += "\"ids\":\"";
    deviceConfig += DEVICE_NAME;
    deviceConfig += "\",";
    deviceConfig += "\"name\":\"";
    deviceConfig += DEVICE_NAME;
    deviceConfig += "\",";
    deviceConfig += "\"mf\":\"Custom\",";
    deviceConfig += "\"sw\":\"1.0\",";
    deviceConfig += "\"model\":\"Lake Sensor Monitor\"";
    deviceConfig += "}";
    
    // Temperature sensors discovery
    for (int i = 0; i < MAX_DS18B20_SENSORS; i++) {
        String topic = discoveryPrefix + "/" + DEVICE_NAME + "/" + TEMP_LABELS[i] + "/config";
        String config = "{";
        config += "\"name\":\"";
        config += TEMP_LABELS[i];
        config += " Temperature\",";
        config += "\"unit_of_measurement\":\"°C\",";
        config += "\"device_class\":\"temperature\",";
        config += "\"state_topic\":\"";
        config += TELEMETRY_TOPIC;
        config += "\",";
        config += "\"value_template\":\"{{ value_json.";
        config += TEMP_LABELS[i];
        config += " }}\",";
        config += "\"availability_topic\":\"";
        config += DEVICE_NAME;
        config += "/status\",";
        config += "\"payload_available\":\"online\",";
        config += "\"payload_not_available\":\"offline\",";
        config += "\"device\":";
        config += deviceConfig;
        config += "}";
        
        Serial.print("Publishing HA Discovery for ");
        Serial.print(TEMP_LABELS[i]);
        Serial.println(" temperature");
        mqttClient.publish(topic.c_str(), config.c_str(), false);
    }
    
    // Current sensor discovery
    String currentTopic = discoveryPrefix + "/" + DEVICE_NAME + "/current/config";
    String currentConfig = "{";
    currentConfig += "\"name\":\"Lake Current\",";
    currentConfig += "\"unit_of_measurement\":\"mA\",";
    currentConfig += "\"state_topic\":\"";
    currentConfig += TELEMETRY_TOPIC;
    currentConfig += "\",";
    currentConfig += "\"value_template\":\"{{ value_json.current_mA }}\",";
    currentConfig += "\"availability_topic\":\"";
    currentConfig += DEVICE_NAME;
    currentConfig += "/status\",";
    currentConfig += "\"payload_available\":\"online\",";
    currentConfig += "\"payload_not_available\":\"offline\",";
    currentConfig += "\"device\":";
    currentConfig += deviceConfig;
    currentConfig += "}";
    
    Serial.println("Publishing HA Discovery for current sensor");
    mqttClient.publish(currentTopic.c_str(), currentConfig.c_str(), false);
    
    // Depth sensor discovery
    String depthTopic = discoveryPrefix + "/" + DEVICE_NAME + "/depth/config";
    String depthConfig = "{";
    depthConfig += "\"name\":\"Lake Depth\",";
    depthConfig += "\"unit_of_measurement\":\"m\",";
    depthConfig += "\"device_class\":\"distance\",";
    depthConfig += "\"state_topic\":\"";
    depthConfig += TELEMETRY_TOPIC;
    depthConfig += "\",";
    depthConfig += "\"value_template\":\"{{ value_json.depth_m }}\",";
    depthConfig += "\"availability_topic\":\"";
    depthConfig += DEVICE_NAME;
    depthConfig += "/status\",";
    depthConfig += "\"payload_available\":\"online\",";
    depthConfig += "\"payload_not_available\":\"offline\",";
    depthConfig += "\"device\":";
    depthConfig += deviceConfig;
    depthConfig += "}";
    
    Serial.println("Publishing HA Discovery for depth sensor");
    mqttClient.publish(depthTopic.c_str(), depthConfig.c_str(), false);
}

// ============================================================================
// DEEP SLEEP FUNCTION
// ============================================================================

void goToDeepSleep() {
    // Disconnect from MQTT
    if (mqttClient.connected()) {
        // Publish offline status
        mqttClient.publish(DEVICE_NAME "/status", "offline", true);
        mqttClient.disconnect();
    }
    
    // Disconnect from WiFi
    if (WiFi.isConnected()) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    
    Serial.print("Entering deep sleep for ");
    Serial.print(SLEEP_DURATION_MS / 1000 / 60);
    Serial.println(" minutes");
    
    // Reset the reason for waking up (optional, for debugging)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT0);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    
    // Enter deep sleep
    // esp_deep_sleep takes time in microseconds
    esp_deep_sleep(SLEEP_DURATION_MS * 1000);
}

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void printHex(const uint8_t* addr, int len) {
    for (int i = 0; i < len; i++) {
        if (addr[i] < 0x10) {
            Serial.print("0");
        }
        Serial.print(addr[i], HEX);
        if (i < len - 1) {
            Serial.print(" ");
        }
    }
    Serial.println();
}
