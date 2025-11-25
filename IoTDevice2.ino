#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "esp_wifi.h"
#include "time.h"

Adafruit_SHT31 sht30 = Adafruit_SHT31();
#define LIGHT_DIGITAL_PIN 26

typedef struct SensorPacket {
  char deviceId[16];
  float temperature;
  float humidity;
  int lightDigital;
  uint32_t timestamp;
  uint8_t isEvent;
} SensorPacket;

SensorPacket dataPacket;

uint8_t masterAddress[] = {0x08, 0x3A, 0xF2, 0x8F, 0xAA, 0x08};

#define MAX_RETRY_QUEUE 10
SensorPacket retryQueue[MAX_RETRY_QUEUE];
int retryCount = 0;

String lastPeerStatus = "";
String lastSendStatus = "";
unsigned long lastDataSend = 0;
unsigned long lastLightCheck = 0;
unsigned long lastRetryCheck = 0;
#define DATA_INTERVAL 600000
#define LIGHT_INTERVAL 60000
#define RETRY_INTERVAL 60000

bool timeSynced = false;

// =================== Timestamp ====================
String getTimestampISO() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    return String(buffer);
  } else {
    return String(millis());
  }
}

// =================== ESP-NOW ====================
void onSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  if (!info) return;
  const uint8_t *mac = info->des_addr;
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (status == ESP_NOW_SEND_SUCCESS){
    Serial.printf("Sent to %s -> OK\n", macStr);
    lastSendStatus = "Last send OK";
  } else {
    Serial.printf("Sent to %s -> FAIL (will retry)\n", macStr);
    lastSendStatus = "Last send failed (queued)";
  }
}

void queueForRetry(SensorPacket &packet) {
  if (retryCount < MAX_RETRY_QUEUE) retryQueue[retryCount++] = packet;
  else {
    for (int i = 1; i < MAX_RETRY_QUEUE; i++) retryQueue[i - 1] = retryQueue[i];
    retryQueue[MAX_RETRY_QUEUE - 1] = packet;
  }
  Serial.printf("⚠️ Queued packet (total=%d)\n", retryCount);
}

void processRetryQueue() {
  if (retryCount == 0) return;
  Serial.printf("Retrying %d queued packets...\n", retryCount);
  for (int i = 0; i < retryCount; i++) {
    esp_err_t result = esp_now_send(masterAddress, (uint8_t *)&retryQueue[i], sizeof(SensorPacket));
    if (result == ESP_OK) Serial.println("Retry sent successfully");
    else Serial.printf("Retry failed (code %d)\n", result);
    delay(50);
  }
  retryCount = 0;
}

void sendPacket(SensorPacket &packet) {
  esp_err_t result = esp_now_send(masterAddress, (uint8_t *)&packet, sizeof(packet));
  if (result != ESP_OK) queueForRetry(packet);
}

void sendLightEvent(int value) {
  SensorPacket eventPacket;
  strcpy(eventPacket.deviceId, "device002");
  eventPacket.temperature = 0;
  eventPacket.humidity = 0;
  eventPacket.lightDigital = value;
  eventPacket.timestamp = millis();
  eventPacket.isEvent = 1;
  Serial.printf("⚡ Light event triggered! D:%d\n", value);
  sendPacket(eventPacket);
}

// =================== Time Sync ====================
void syncTimeOnce() {
  WiFi.begin("your_wifi_ssid", "your_wifi_password");
  Serial.println("⏳ Connecting to WiFi for NTP...");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n🌐 WiFi connected, syncing time...");
    configTime(28800, 0, "pool.ntp.org", "time.nist.gov");

    bool gotTime = false;
    for (int i = 0; i < 40; i++) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        gotTime = true;
        timeSynced = true;
        Serial.println("Time synced successfully!");
        break;
      }
      delay(500);
    }

    if (!gotTime) Serial.println("NTP sync timeout — using millis()");
  } else {
    Serial.println("\nWiFi not connected — skipping NTP.");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// =================== Setup ====================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  M5.Display.setTextFont(2);
  M5.Display.println("=== Device2 Booting ===");

  pinMode(LIGHT_DIGITAL_PIN, INPUT);
  Wire.begin();
  if (!sht30.begin(0x44)) M5.Display.println("❌ SHT31 not found!");
  else M5.Display.println("SHT31 Ready");

  syncTimeOnce();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  strcpy(dataPacket.deviceId, "device002");

  if (esp_now_init() != ESP_OK) {
    M5.Display.println("ESP-NOW init failed!");
    return;
  }

  esp_now_register_send_cb(onSent);
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, masterAddress, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) == ESP_OK) {
    M5.Display.println("Peer added successfully");
    lastPeerStatus = "Peer added successfully";
  } else {
    M5.Display.println("Failed to add peer");
    lastPeerStatus = "Failed to add peer";
  }
}

// =================== Loop ====================
void loop() {
  M5.update();
  unsigned long now = millis();

  if (M5.BtnA.wasPressed()) {
    M5.Display.wakeup();
    M5.Display.fillScreen(BLACK);
    M5.Display.setCursor(0, 20);
    M5.Display.printf(
      "=== DEVICE STATUS ===\n\n"
      "Temp: %.1f°C\n"
      "Humidity: %.1f%%\n"
      "Light: D:%d\n\n"
      "%s\n"
      "%s\n",
      dataPacket.temperature,
      dataPacket.humidity,
      dataPacket.lightDigital,
      lastPeerStatus.c_str(),
      lastSendStatus.c_str()
    );
    Serial.println("Displayed device status");
  }

  if (M5.BtnB.wasPressed()) {
    M5.Display.sleep();
    Serial.println("LCD turned off");
  }



  if (now - lastDataSend >= DATA_INTERVAL) {
    lastDataSend = now;
    float t = sht30.readTemperature();
    float h = sht30.readHumidity();
    if (isnan(t) || isnan(h)) {
      Serial.println("Sensor read failed");
      return;
    }

    dataPacket.temperature = round(t * 10) / 10.0;
    dataPacket.humidity = h;
    dataPacket.lightDigital = digitalRead(LIGHT_DIGITAL_PIN);
    dataPacket.timestamp = now;
    dataPacket.isEvent = 0;

    Serial.printf("%.1f°C %.1f%% D:%d\n",
                  dataPacket.temperature, dataPacket.humidity,
                  dataPacket.lightDigital);

    sendPacket(dataPacket);
  }

  if (now - lastLightCheck >= LIGHT_INTERVAL) {
    lastLightCheck = now;
    int light = digitalRead(LIGHT_DIGITAL_PIN);
    if (light == 1) sendLightEvent(light);
  }

  if (now - lastRetryCheck >= RETRY_INTERVAL) {
    lastRetryCheck = now;
    processRetryQueue();
  }
}
