/*
 * House Water Management System - Eco Friendly House
 * Supports: ESP32 (Unit Garden & Unit Source Manager)
 * 
 * Select device by uncommenting ONE define below:
 */

// #define DEVICE_GARDEN
#define DEVICE_SOURCE_MANAGER

/*===============================================
 * LIBRARY INCLUDES
 *===============================================*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiProvisioner.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <FirebaseClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

/*===============================================
 * DEVICE CONFIGURATION
 *===============================================*/
#ifdef DEVICE_GARDEN
  #define DEVICE_NAME "unit1_garden"
  #define FIRMWARE_VERSION "v1.3.0"
#elif defined(DEVICE_SOURCE_MANAGER)
  #define DEVICE_NAME "unit2_source_manager"
  #define FIRMWARE_VERSION "v1.3.0"
#else
  #error "Please define either DEVICE_GARDEN or DEVICE_SOURCE_MANAGER"
#endif

/*===============================================
 * PIN DEFINITIONS
 *===============================================*/
#ifdef DEVICE_GARDEN
  #define PIN_DHT22 25
  #define PIN_SOIL_MOISTURE 34
  #define PIN_SR04T_TRIG 26
  #define PIN_SR04T_ECHO 27
  #define PIN_RELAY_PUMP 32
  #define PIN_SDA 21
  #define PIN_SCL 22
#endif

#ifdef DEVICE_SOURCE_MANAGER
  #define PIN_DHT22 25
  #define PIN_RAIN_SENSOR 33
  #define PIN_RAIN_ANALOG 35
  #define PIN_SR04T_RAIN_TRIG 26
  #define PIN_SR04T_RAIN_ECHO 27
  #define PIN_SR04T_WELL_TRIG 14
  #define PIN_SR04T_WELL_ECHO 12
  #define PIN_FLOW_SENSOR 36
  #define PIN_RELAY_PUMP_RAIN 32
  #define PIN_RELAY_PUMP_WELL 33
  #define PIN_RELAY_VALVE_RAIN 25
  #define PIN_RELAY_VALVE_WELL 26
  #define PIN_SDA 21
  #define PIN_SCL 22
#endif

/*===============================================
 * CONSTANTS
 *===============================================*/
#define FIREBASE_HOST "your-project.firebaseio.com"
#define FIREBASE_API_KEY "your-api-key"
#define FIREBASE_USER_EMAIL "your-email@example.com"
#define FIREBASE_USER_PASSWORD "your-password"

#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 25200  // GMT+7 (Indonesia)
#define DAYLIGHT_OFFSET_SEC 0

#define SENSOR_READ_INTERVAL 5000
#define FIREBASE_SYNC_INTERVAL 10000
#define RTC_SYNC_INTERVAL 3600000
#define OFFLINE_LOG_INTERVAL 60000

/*===============================================
 * GLOBAL OBJECTS
 *===============================================*/
DHTesp dht;
RTC_DS3231 rtc;
Preferences prefs;
WiFiClientSecure sslClient;
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult result;

/*===============================================
 * GLOBAL VARIABLES
 *===============================================*/
bool wifiConnected = false;
bool firebaseReady = false;
unsigned long lastSensorRead = 0;
unsigned long lastFirebaseSync = 0;
unsigned long lastRTCSync = 0;
unsigned long lastOfflineLog = 0;
int offlineLogCount = 0;

// Sensor Data Structure
struct SensorData {
#ifdef DEVICE_GARDEN
  float temperature;
  float humidity;
  int soilMoisture;
  float bufferLevelCm;
  bool pumpState;
  String process;
  String request;
  String event;
  String error;
  String activeSource;
  float flowLMin;
  unsigned long totalLiters;
  String pumpOverride;
  String sourceForce;
#endif

#ifdef DEVICE_SOURCE_MANAGER
  bool rainDetected;
  int rainIntensity;
  float tankRainCm;
  float tankWellCm;
  float flowLMin;
  bool pumpRain;
  bool pumpWell;
  bool valveRain;
  bool valveWell;
  String process;
  String request;
  String event;
  String error;
  String activeSource;
  String lastSwitchReason;
  String pumpRainOverride;
  String pumpWellOverride;
  String valveRainOverride;
  String valveWellOverride;
#endif
};

SensorData sensorData;

/*===============================================
 * FUNCTION PROTOTYPES
 *===============================================*/
void setupWiFi();
void setupFirebase();
void setupSensors();
void setupRTC();
void readSensors();
void syncToFirebase();
void syncNTPToRTC();
void saveOfflineData();
void loadOfflineData();
void processLogic();
void asyncCB(AsyncResult &aResult);
void printResult(AsyncResult &aResult);
float readUltrasonic(int trigPin, int echoPin);
void logHourlyData();
void logEvent(String eventType, String detail = "");

/*===============================================
 * SETUP
 *===============================================*/
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========================================");
  Serial.println("House Water Management System");
  Serial.println("Device: " + String(DEVICE_NAME));
  Serial.println("Firmware: " + String(FIRMWARE_VERSION));
  Serial.println("========================================\n");

  // Initialize preferences
  prefs.begin("water-mgmt", false);
  
  // Setup hardware
  setupSensors();
  setupRTC();
  
  // Setup WiFi with provisioner
  setupWiFi();
  
  // Setup Firebase if WiFi connected
  if (wifiConnected) {
    syncNTPToRTC();
    setupFirebase();
  } else {
    Serial.println("Starting in offline mode...");
  }
  
  // Load offline data if any
  loadOfflineData();
  
  Serial.println("\nSystem ready!\n");
}

/*===============================================
 * MAIN LOOP
 *===============================================*/
void loop() {
  unsigned long currentMillis = millis();
  
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    firebaseReady = false;
    Serial.println("WiFi disconnected!");
  } else if (WiFi.status() == WL_CONNECTED && !wifiConnected) {
    wifiConnected = true;
    setupFirebase();
    syncNTPToRTC();
  }
  
  // Read sensors
  if (currentMillis - lastSensorRead >= SENSOR_READ_INTERVAL) {
    lastSensorRead = currentMillis;
    readSensors();
    processLogic();
  }
  
  // Sync to Firebase
  if (wifiConnected && firebaseReady) {
    if (currentMillis - lastFirebaseSync >= FIREBASE_SYNC_INTERVAL) {
      lastFirebaseSync = currentMillis;
      syncToFirebase();
    }
  } else {
    // Save offline data
    if (currentMillis - lastOfflineLog >= OFFLINE_LOG_INTERVAL) {
      lastOfflineLog = currentMillis;
      saveOfflineData();
    }
  }
  
  // Sync RTC with NTP
  if (wifiConnected && currentMillis - lastRTCSync >= RTC_SYNC_INTERVAL) {
    lastRTCSync = currentMillis;
    syncNTPToRTC();
  }
  
  delay(100);
}

/*===============================================
 * WIFI SETUP
 *===============================================*/
void setupWiFi() {
  Serial.println("Starting WiFi Provisioner...");
  
  WiFiProvisioner provisioner;
  provisioner.setHostname(DEVICE_NAME);
  provisioner.setTimeout(300); // 5 minutes timeout
  
  if (provisioner.begin()) {
    Serial.println("WiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
    wifiConnected = true;
  } else {
    Serial.println("WiFi provisioning timeout - starting in offline mode");
    wifiConnected = false;
  }
}

/*===============================================
 * FIREBASE SETUP
 *===============================================*/
void setupFirebase() {
  Serial.println("Initializing Firebase...");
  
  sslClient.setInsecure(); // For testing only
  
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  
  // Initialize Firebase app
  initializeApp(app, FIREBASE_API_KEY, asyncCB);
  
  // Sign in
  app.getAuth().signIn(FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD);
  
  // Initialize database
  Database.url(FIREBASE_HOST);
  app.getApp<RealtimeDatabase>(Database);
  
  firebaseReady = true;
  Serial.println("Firebase initialized!");
  
  // Update online status
  String path = "/devices/" + String(DEVICE_NAME) + "/info/online";
  Database.set<bool>(app, path, true);
}

/*===============================================
 * SENSOR SETUP
 *===============================================*/
void setupSensors() {
  Serial.println("Initializing sensors...");
  
  // DHT22
  dht.setup(PIN_DHT22, DHTesp::DHT22);
  
  // Soil moisture (Garden only)
#ifdef DEVICE_GARDEN
  pinMode(PIN_SOIL_MOISTURE, INPUT);
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  digitalWrite(PIN_RELAY_PUMP, LOW);
#endif

  // Ultrasonic sensors
#ifdef DEVICE_GARDEN
  pinMode(PIN_SR04T_TRIG, OUTPUT);
  pinMode(PIN_SR04T_ECHO, INPUT);
#endif

#ifdef DEVICE_SOURCE_MANAGER
  pinMode(PIN_RAIN_SENSOR, INPUT);
  pinMode(PIN_RAIN_ANALOG, INPUT);
  pinMode(PIN_SR04T_RAIN_TRIG, OUTPUT);
  pinMode(PIN_SR04T_RAIN_ECHO, INPUT);
  pinMode(PIN_SR04T_WELL_TRIG, OUTPUT);
  pinMode(PIN_SR04T_WELL_ECHO, INPUT);
  pinMode(PIN_FLOW_SENSOR, INPUT);
  
  // Relays
  pinMode(PIN_RELAY_PUMP_RAIN, OUTPUT);
  pinMode(PIN_RELAY_PUMP_WELL, OUTPUT);
  pinMode(PIN_RELAY_VALVE_RAIN, OUTPUT);
  pinMode(PIN_RELAY_VALVE_WELL, OUTPUT);
  
  digitalWrite(PIN_RELAY_PUMP_RAIN, LOW);
  digitalWrite(PIN_RELAY_PUMP_WELL, LOW);
  digitalWrite(PIN_RELAY_VALVE_RAIN, LOW);
  digitalWrite(PIN_RELAY_VALVE_WELL, LOW);
#endif
  
  Serial.println("Sensors initialized!");
}

/*===============================================
 * RTC SETUP
 *===============================================*/
void setupRTC() {
  Serial.println("Initializing RTC...");
  Wire.begin(PIN_SDA, PIN_SCL);
  
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
    return;
  }
  
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  Serial.println("RTC initialized!");
}

/*===============================================
 * READ SENSORS
 *===============================================*/
void readSensors() {
  // Read DHT22
  TempAndHumidity data = dht.getTempAndHumidity();
  
#ifdef DEVICE_GARDEN
  sensorData.temperature = data.temperature;
  sensorData.humidity = data.humidity;
  sensorData.soilMoisture = analogRead(PIN_SOIL_MOISTURE);
  sensorData.bufferLevelCm = readUltrasonic(PIN_SR04T_TRIG, PIN_SR04T_ECHO);
  sensorData.pumpState = digitalRead(PIN_RELAY_PUMP);
  
  Serial.printf("Garden - Temp: %.1f°C, Humidity: %.1f%%, Soil: %d, Buffer: %.1fcm\n",
                sensorData.temperature, sensorData.humidity, 
                sensorData.soilMoisture, sensorData.bufferLevelCm);
#endif

#ifdef DEVICE_SOURCE_MANAGER
  sensorData.rainDetected = !digitalRead(PIN_RAIN_SENSOR);
  sensorData.rainIntensity = analogRead(PIN_RAIN_ANALOG);
  sensorData.tankRainCm = readUltrasonic(PIN_SR04T_RAIN_TRIG, PIN_SR04T_RAIN_ECHO);
  sensorData.tankWellCm = readUltrasonic(PIN_SR04T_WELL_TRIG, PIN_SR04T_WELL_ECHO);
  
  Serial.printf("Source - Rain: %s, Rain Tank: %.1fcm, Well Tank: %.1fcm\n",
                sensorData.rainDetected ? "YES" : "NO",
                sensorData.tankRainCm, sensorData.tankWellCm);
#endif
}

/*===============================================
 * READ ULTRASONIC SENSOR
 *===============================================*/
float readUltrasonic(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000);
  if (duration == 0) return -1;
  
  float distance = duration * 0.034 / 2;
  return distance;
}

/*===============================================
 * PROCESS LOGIC
 *===============================================*/
void processLogic() {
#ifdef DEVICE_GARDEN
  // Get control settings from Firebase or use defaults
  if (sensorData.pumpOverride == "ON") {
    digitalWrite(PIN_RELAY_PUMP, HIGH);
    sensorData.process = "PROCESS_WATERING";
  } else if (sensorData.pumpOverride == "OFF") {
    digitalWrite(PIN_RELAY_PUMP, LOW);
    sensorData.process = "PROCESS_IDLE";
  } else {
    // AUTO mode logic
    if (sensorData.soilMoisture > 700 && sensorData.bufferLevelCm > 10) {
      if (!sensorData.pumpState) {
        digitalWrite(PIN_RELAY_PUMP, HIGH);
        sensorData.process = "PROCESS_WATERING";
        sensorData.event = "EVENT_WATERING_START";
        logEvent("EVENT_WATERING_START", "Auto schedule");
      }
    } else if (sensorData.soilMoisture < 500) {
      if (sensorData.pumpState) {
        digitalWrite(PIN_RELAY_PUMP, LOW);
        sensorData.process = "PROCESS_IDLE";
        sensorData.event = "EVENT_WATERING_COMPLETE";
        logEvent("EVENT_WATERING_COMPLETE");
      }
    }
  }
#endif

#ifdef DEVICE_SOURCE_MANAGER
  // Source manager logic
  if (sensorData.activeSource.isEmpty()) {
    sensorData.activeSource = "WELL_TANK";
  }
  
  // Switch source logic
  if (sensorData.tankRainCm < 20 && sensorData.activeSource == "RAIN_TANK") {
    sensorData.activeSource = "WELL_TANK";
    sensorData.lastSwitchReason = "RAIN_LOW_LEVEL";
    sensorData.event = "EVENT_SOURCE_CHANGED";
    logEvent("EVENT_SOURCE_CHANGED", "RAIN_LOW_LEVEL");
  } else if (sensorData.rainDetected && sensorData.tankRainCm > 50) {
    sensorData.activeSource = "RAIN_TANK";
    sensorData.lastSwitchReason = "RAIN_AVAILABLE";
    sensorData.event = "EVENT_SOURCE_CHANGED";
    logEvent("EVENT_SOURCE_CHANGED", "RAIN_AVAILABLE");
  }
#endif
}

/*===============================================
 * SYNC TO FIREBASE
 *===============================================*/
void syncToFirebase() {
  if (!firebaseReady) return;
  
  String basePath = "/devices/" + String(DEVICE_NAME);
  
  // Update info
  Database.set<int>(app, basePath + "/info/last_seen", rtc.now().unixtime());
  Database.set<String>(app, basePath + "/info/fw", FIRMWARE_VERSION);
  Database.set<int>(app, basePath + "/info/rssi", WiFi.RSSI());
  
#ifdef DEVICE_GARDEN
  // Update sensor data
  Database.set<float>(app, basePath + "/sensor/temperature", sensorData.temperature);
  Database.set<float>(app, basePath + "/sensor/humidity", sensorData.humidity);
  Database.set<int>(app, basePath + "/sensor/soil", sensorData.soilMoisture);
  Database.set<float>(app, basePath + "/sensor/buffer_level_cm", sensorData.bufferLevelCm);
  
  // Update actuator
  Database.set<bool>(app, basePath + "/actuator/pump", sensorData.pumpState);
  
  // Update status
  Database.set<String>(app, basePath + "/status/process", sensorData.process);
  Database.set<String>(app, basePath + "/status/event", sensorData.event);
#endif

#ifdef DEVICE_SOURCE_MANAGER
  // Update sensor data
  Database.set<bool>(app, basePath + "/sensor/rain", sensorData.rainDetected);
  Database.set<int>(app, basePath + "/sensor/rain_intensity", sensorData.rainIntensity);
  Database.set<float>(app, basePath + "/sensor/tank_rain_cm", sensorData.tankRainCm);
  Database.set<float>(app, basePath + "/sensor/tank_well_cm", sensorData.tankWellCm);
  
  // Update actuators
  Database.set<bool>(app, basePath + "/actuator/pump_rain", sensorData.pumpRain);
  Database.set<bool>(app, basePath + "/actuator/pump_well", sensorData.pumpWell);
  Database.set<bool>(app, basePath + "/actuator/valve_rain", sensorData.valveRain);
  Database.set<bool>(app, basePath + "/actuator/valve_well", sensorData.valveWell);
  
  // Update status
  Database.set<String>(app, basePath + "/status/process", sensorData.process);
  Database.set<String>(app, basePath + "/status/event", sensorData.event);
  Database.set<String>(app, basePath + "/source/active_source", sensorData.activeSource);
#endif
  
  Serial.println("Data synced to Firebase");
}

/*===============================================
 * NTP TO RTC SYNC
 *===============================================*/
void syncNTPToRTC() {
  Serial.println("Syncing time from NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, 
                        timeinfo.tm_mday, timeinfo.tm_hour, 
                        timeinfo.tm_min, timeinfo.tm_sec));
    Serial.println("RTC synced with NTP");
  } else {
    Serial.println("Failed to get NTP time");
  }
}

/*===============================================
 * SAVE OFFLINE DATA
 *===============================================*/
void saveOfflineData() {
  JsonDocument doc;
  
  doc["timestamp"] = rtc.now().unixtime();
  
#ifdef DEVICE_GARDEN
  doc["temperature"] = sensorData.temperature;
  doc["humidity"] = sensorData.humidity;
  doc["soil"] = sensorData.soilMoisture;
  doc["buffer_level"] = sensorData.bufferLevelCm;
#endif

#ifdef DEVICE_SOURCE_MANAGER
  doc["rain"] = sensorData.rainDetected;
  doc["tank_rain"] = sensorData.tankRainCm;
  doc["tank_well"] = sensorData.tankWellCm;
#endif
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  String key = "offline_" + String(offlineLogCount);
  prefs.putString(key.c_str(), jsonString);
  offlineLogCount++;
  prefs.putInt("log_count", offlineLogCount);
  
  Serial.println("Offline data saved: " + String(offlineLogCount));
}

/*===============================================
 * LOAD OFFLINE DATA
 *===============================================*/
void loadOfflineData() {
  offlineLogCount = prefs.getInt("log_count", 0);
  Serial.println("Offline logs found: " + String(offlineLogCount));
  
  if (wifiConnected && firebaseReady && offlineLogCount > 0) {
    Serial.println("Uploading offline data to Firebase...");
    // Upload offline data here
    // Then clear
    prefs.clear();
    offlineLogCount = 0;
  }
}

/*===============================================
 * LOG HOURLY DATA
 *===============================================*/
void logHourlyData() {
  DateTime now = rtc.now();
  char path[200];
  sprintf(path, "/devices/%s/log_hourly/%04d/%02d/%02d/%02d:00",
          DEVICE_NAME, now.year(), now.month(), now.day(), now.hour());
  
  // Log data to Firebase
  if (firebaseReady) {
#ifdef DEVICE_GARDEN
    Database.set<float>(app, String(path) + "/temperature", sensorData.temperature);
    Database.set<float>(app, String(path) + "/humidity", sensorData.humidity);
    Database.set<int>(app, String(path) + "/soil", sensorData.soilMoisture);
#endif

#ifdef DEVICE_SOURCE_MANAGER
    Database.set<bool>(app, String(path) + "/rain_detected", sensorData.rainDetected);
    Database.set<float>(app, String(path) + "/tank_rain_cm", sensorData.tankRainCm);
    Database.set<float>(app, String(path) + "/tank_well_cm", sensorData.tankWellCm);
#endif
  }
}

/*===============================================
 * LOG EVENT
 *===============================================*/
void logEvent(String eventType, String detail) {
  DateTime now = rtc.now();
  String path = "/devices/" + String(DEVICE_NAME) + "/log_events/" + String(now.unixtime());
  
  if (firebaseReady) {
    Database.set<String>(app, path + "/type", eventType);
    if (!detail.isEmpty()) {
      Database.set<String>(app, path + "/detail", detail);
    }
  }
  
  Serial.println("Event logged: " + eventType);
}

/*===============================================
 * FIREBASE CALLBACKS
 *===============================================*/
void asyncCB(AsyncResult &aResult) {
  if (aResult.error().code() != 0) {
    Serial.printf("Firebase Error: %s, %s\n", 
                  aResult.error().message().c_str(),
                  aResult.error().debug().c_str());
  }
}

void printResult(AsyncResult &aResult) {
  if (aResult.isDebug()) {
    Serial.printf("Debug: %s\n", aResult.c_str());
  }
}