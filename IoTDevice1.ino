#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include "DHT.h"
#include "time.h"

// ==================== WiFi / Firebase ====================
#define WIFI_SSID "I Need Wif"
#define WIFI_PASSWORD "Dontbeedwi"

#define API_KEY "REPLACE API KEY"
#define FIREBASE_PROJECT_ID "REPLACE PROJECT ID"
#define USER_EMAIL "esp32@node1.com"
#define USER_PASSWORD "esp32passwordnode1"

FirebaseAuth auth;
FirebaseConfig config;
FirebaseData fbdo;

// ==================== Device Info ====================
String deviceId = "device001";

// ==================== DHT11 Sensor ====================
#define DHTPIN 4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ==================== Other Sensors ====================
#define SOIL_PIN 34
#define MIC_PIN 35
#define RAIN_PIN 27

// ==================== Corner Buttons ====================
const int NUM_CORNERS = 4;
const int CORNER_PINS[NUM_CORNERS] = {19, 21, 18, 26};
int lastReading[NUM_CORNERS];
int stableState[NUM_CORNERS];
unsigned long lastChange[NUM_CORNERS];
bool longPressHandled[NUM_CORNERS];

const unsigned long DEBOUNCE_DELAY = 50;
const unsigned long LONG_PRESS_TIME = 2000;

// ==================== Thresholds ====================
const int SOIL_HIGH_THRESHOLD = 1100;
const int MIC_THRESHOLD = 800;
int lastRainState = HIGH;


// ==================== Timing ====================
unsigned long lastLogTime = 0;
unsigned long lastEventCheck = 0;
const unsigned long LOG_INTERVAL = 600000;   // 11 s self reading upload
const unsigned long EVENT_CHECK_INTERVAL = 60000; // 1 min threshold checks
bool uploadingNow = false;

// ==================== ESP-NOW Packet ====================
typedef struct SensorPacket {
  char deviceId[16];
  float temperature;
  float humidity;
  int lightDigital;
  uint32_t timestamp;
  uint8_t isEvent;
} SensorPacket;

SensorPacket latestPacket;
volatile bool newDataAvailable = false;

// ==================== Timestamp Helper ====================

const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;  // Malaysia time (UTC+8)
const int daylightOffset_sec = 0;
bool timeSynced = false;
unsigned long timeSyncStart = 0;

// Try NTP sync once at startup (wait max 20s)
void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Syncing NTP time...");

  timeSyncStart = millis();
  struct tm timeinfo;
  while (millis() - timeSyncStart < 20000) {  // wait up to 20 seconds
    if (getLocalTime(&timeinfo)) {
      Serial.println("Time synchronized!");
      timeSynced = true;
      return;
    }
    delay(500);
  }

  Serial.println("⚠️ NTP sync failed, continuing without accurate time.");
  timeSynced = false;
}

String getTimestampISO() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return String(buffer);
  }
}



// ==================== Firestore Helpers ====================
// (uploadToFirestore() and resendCachedUploads() unchanged)
void uploadToFirestore(String collection, String docId, FirebaseJson &json) {
  String path = collection + "/" + docId;
  String jsonStr;
  json.toString(jsonStr);

  if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID,
                                       "(default)", path.c_str(),
                                       jsonStr.c_str(), "")) {
    Serial.println("Uploaded to " + path);
  } else {
    Serial.print("Firestore error: ");
    Serial.println(fbdo.errorReason());

    if (LittleFS.exists(CACHE_PATH)) {
      File check = LittleFS.open(CACHE_PATH, FILE_READ);
      if (check && check.size() > MAX_CACHE_SIZE_BYTES) {
        Serial.println("Cache full — skipping new save");
        check.close();
        return;
      }
      if (check) check.close();
    }

    File file = LittleFS.open(CACHE_PATH, FILE_APPEND);
    if (!file) return;

    DynamicJsonDocument wrapper(JSON_DOC_CAP);
    wrapper["collection"] = collection;
    wrapper["docId"] = docId;

    DeserializationError derr = deserializeJson(wrapper["json"], jsonStr);
    if (derr) {
      wrapper["json_str"] = jsonStr;
      serializeJson(wrapper, file);
      file.println();
      file.close();
      return;
    }

    serializeJson(wrapper, file);
    file.println();
    file.close();
  }
}

void resendCachedUploads() {
  if (!Firebase.ready()) return;
  if (!LittleFS.exists(CACHE_PATH)) return;

  File file = LittleFS.open(CACHE_PATH, FILE_READ);
  if (!file || file.size() == 0) { if (file) file.close(); return; }

  const char *TMP_PATH = "/pending_tmp.jsonl";
  File tmp = LittleFS.open(TMP_PATH, FILE_WRITE);
  if (!tmp) { file.close(); return; }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 3) continue;

    DynamicJsonDocument cachedDoc(JSON_DOC_CAP);
    if (deserializeJson(cachedDoc, line)) { tmp.println(line); continue; }

    String collection = cachedDoc["collection"].as<String>();
    String docId = cachedDoc["docId"].as<String>();
    String payloadStr;

    if (cachedDoc.containsKey("json"))
      serializeJson(cachedDoc["json"], payloadStr);
    else if (cachedDoc.containsKey("json_str"))
      payloadStr = cachedDoc["json_str"].as<String>();
    else { tmp.println(line); continue; }

    if (!Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID,
                                          "(default)",
                                          (collection + "/" + docId).c_str(),
                                          payloadStr.c_str(), "")) {
      tmp.println(line);
    }
  }

  file.close(); tmp.close();
  LittleFS.remove(CACHE_PATH);
  LittleFS.rename(TMP_PATH, CACHE_PATH);
}

// ==================== Event Logging ====================
void logEvent(String type, String priority, String message, int value = -1) {
  FirebaseJson json;
  String timestamp = getTimestampISO();
  json.set("fields/deviceId/stringValue", deviceId);
  json.set("fields/type/stringValue", type);
  json.set("fields/priority/stringValue", priority);
  json.set("fields/timestamp/stringValue", timestamp);
  json.set("fields/message/stringValue", message);
  if (value >= 0) json.set("fields/value/integerValue", value);
  uploadToFirestore("events", timestamp, json);
}

// ==================== Button Check ====================
void checkButtons() {
  for (int i = 0; i < NUM_CORNERS; ++i) {
    int reading = digitalRead(CORNER_PINS[i]);
    if (reading != lastReading[i]) {
      lastChange[i] = millis();
      lastReading[i] = reading;
    }
    if ((millis() - lastChange[i]) > DEBOUNCE_DELAY) {
      if (reading != stableState[i]) {
        stableState[i] = reading;
        if (stableState[i] == LOW) {
          Serial.printf("🔘 Button %d short press\n", i);
          logEvent("button_press", "low", "Corner button pressed", i);
        } else {
          if (!longPressHandled[i]) longPressHandled[i] = false;
        }
      }
      if (stableState[i] == LOW && !longPressHandled[i] &&
          (millis() - lastChange[i] > LONG_PRESS_TIME)) {
        Serial.printf("⏱️ Button %d long press\n", i);
        logEvent("button_longpress", "medium", "Button long press", i);
        longPressHandled[i] = true;
      }
    }
  }
}

// ==================== Device Status Update ====================
void updateDeviceStatus(String id) {
  FirebaseJson json;
  json.set("fields/name/stringValue", id);
  json.set("fields/status/stringValue", "online");
  json.set("fields/lastSeen/stringValue", getTimestampISO());
  uploadToFirestore("devices", id, json);
}

// ==================== ESP-NOW Receive ====================
void onDataRecv(const esp_now_recv_info *info, const uint8_t *incomingDataBytes, int len) {
  memcpy(&latestPacket, incomingDataBytes, sizeof(latestPacket));
  newDataAvailable = true;
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Device1 Booting ===");
  dht.begin();

  if (!LittleFS.begin(true)) Serial.println("❌ LittleFS Mount Failed");
  else Serial.println("✅ LittleFS mounted");

  pinMode(SOIL_PIN, INPUT);
  pinMode(MIC_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);
  for (int i = 0; i < NUM_CORNERS; ++i) {
    pinMode(CORNER_PINS[i], INPUT_PULLUP);
    lastReading[i] = digitalRead(CORNER_PINS[i]);
    stableState[i] = lastReading[i];
    lastChange[i] = millis();
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(500); }
  Serial.println("\n✅ WiFi Connected");

  initTime();
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (esp_now_init() != ESP_OK)
    Serial.println("❌ ESP-NOW init failed!");
  else {
    esp_now_register_recv_cb(onDataRecv);
    Serial.println("✅ ESP-NOW Receiver Ready");
  }

  lastLogTime = millis();
  lastEventCheck = millis();
}

// ==================== Upload Helpers ====================
void uploadReading() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int soil = analogRead(SOIL_PIN);
  int mic = analogRead(MIC_PIN);
  int rain = digitalRead(RAIN_PIN);

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("⚠️ DHT11 read error");
    return;
  }

  FirebaseJson json;
  String timestamp = getTimestampISO();
  json.set("fields/deviceId/stringValue", deviceId);
  json.set("fields/timestamp/stringValue", timestamp);
  json.set("fields/temperature/doubleValue", temperature);
  json.set("fields/humidity/doubleValue", humidity);
  json.set("fields/moisture/integerValue", soil);
  json.set("fields/sound/integerValue", mic);
  json.set("fields/rain/integerValue", rain);
  uploadToFirestore("readings", timestamp, json);
  updateDeviceStatus(deviceId);
}

void checkThresholdEvents() {
  int soil = analogRead(SOIL_PIN);
  int mic = analogRead(MIC_PIN);
  int rain = digitalRead(RAIN_PIN);

  if (soil < SOIL_HIGH_THRESHOLD)
    logEvent("moisture_low", "low", "Soil moisture high", soil);
  if (rain == LOW && lastRainState == HIGH)
    logEvent("rain_detected", "low", "Rain detected");
  if (mic > MIC_THRESHOLD)
    logEvent("sound_high", "high", "Loud sound", mic);

  lastRainState = rain;
}

void uploadPacketToFirestore(const SensorPacket &packet) {
  String timestamp = getTimestampISO();
  FirebaseJson json;

  if (packet.isEvent == 1) {
    Serial.println("[EVENT] Light event detected!");

    logEvent("light_detected", "medium",
             "Light event from " + String(packet.deviceId),
             packet.lightDigital);
  } else {
    Serial.println("DATA] Uploading sensor reading to Firestore...");


    json.set("fields/deviceId/stringValue", packet.deviceId);
    json.set("fields/timestamp/stringValue", timestamp);
    json.set("fields/temperature/doubleValue", packet.temperature);
    json.set("fields/humidity/doubleValue", packet.humidity);
    json.set("fields/lightDigital/integerValue", packet.lightDigital);

    uploadToFirestore("readings", timestamp, json);
  }

  updateDeviceStatus(packet.deviceId);
}

// ==================== Loop ====================
void loop() {
  checkButtons();

  if (newDataAvailable && Firebase.ready() && !uploadingNow) {
    uploadingNow = true;
    newDataAvailable = false;
    uploadPacketToFirestore(latestPacket);
    uploadingNow = false;
  }

  if (millis() - lastLogTime > LOG_INTERVAL && Firebase.ready() && !uploadingNow) {
    uploadingNow = true;
    lastLogTime = millis();
    uploadReading();
    uploadingNow = false;
  }

  if (millis() - lastEventCheck > EVENT_CHECK_INTERVAL && Firebase.ready() && !uploadingNow) {
    uploadingNow = true;
    lastEventCheck = millis();
    checkThresholdEvents();   // check soil / mic / rain
    uploadingNow = false;
  }

  static unsigned long lastRetry = 0;
  if (millis() - lastRetry > 30000 && Firebase.ready() && !uploadingNow) {
    uploadingNow = true;
    resendCachedUploads();
    uploadingNow = false;
    lastRetry = millis();
  }
}
