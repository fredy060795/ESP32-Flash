// ESP32-CAM Helmkamera (AI-Thinker / ARCELI, OV2640) – nur Bluetooth Low Energy
//
// - Kein WLAN: Das Handy bleibt im normalen Internet und holt die Bilder per BLE ab.
// - Keine Passwörter, kein Pairing: BLE ist ab dem Start aktiv.
// - Gerätename (Standard "Helm Camera") wird beim Flashen über USB gesetzt
//   (Befehl "NAME=..." über die serielle Schnittstelle) oder per BLE-Befehl.
// - Bilder als JPEG, einzeln ("snap") oder fortlaufend ("stream").
//
// Protokoll: siehe ../../PROTOCOL.md

#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define FW_VERSION "2.0.0"
#define DEFAULT_NAME "Helm Camera"
#define NAME_MAX_BYTES 29  // passt in die BLE-Scan-Antwort

// ---------- AI-Thinker ESP32-CAM Pinbelegung ----------
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_LED_PIN 4
#define STATUS_LED_PIN 33  // rote LED auf der Rückseite, active low

// ---------- Bluetooth UUIDs (siehe PROTOCOL.md) ----------
#define SERVICE_UUID "7b3c0001-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define STATUS_UUID "7b3c0002-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define COMMAND_UUID "7b3c0003-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define IMAGE_UUID "7b3c0004-6b1a-4d6e-9c55-3a2f1e0c8a01"

// Bildübertragung per BLE-Notification, jedes Paket beginnt mit einem Typ-Byte:
//   0x01 Start: u32 Länge, u16 Breite, u16 Höhe, u32 Bildnummer (little endian)
//   0x02 Daten: u32 Offset, danach JPEG-Bytes
//   0x03 Ende:  u32 Bildnummer
//   0x04 Fehler: Text
enum : uint8_t { IMG_START = 1, IMG_DATA = 2, IMG_END = 3, IMG_ERROR = 4 };

struct Settings {
  String name;
  int framesize;
  int quality;
  int interval;  // Mindestabstand zwischen Bildern im Stream-Modus (ms)
};

static Settings cfg;
static Preferences prefs;
static bool cameraOk = false;
static int flashLevel = 0;
static bool streaming = false;
static uint32_t frameNo = 0;
static uint32_t lastFrameAt = 0;

static NimBLEServer* bleServer = nullptr;
static NimBLECharacteristic* statusChr = nullptr;
static NimBLECharacteristic* imageChr = nullptr;
static volatile uint16_t bleConn = BLE_HS_CONN_HANDLE_NONE;
static QueueHandle_t cmdQueue = nullptr;
static const size_t CMD_MAX = 256;

static volatile bool statusDirty = false;
static volatile bool stopStream = false;
static uint32_t rebootAt = 0;

// ======================================================================
// Einstellungen
// ======================================================================

static void loadSettings() {
  prefs.begin("cam", true);
  cfg.name = prefs.getString("name", DEFAULT_NAME);
  cfg.framesize = prefs.getInt("fs", FRAMESIZE_QVGA);
  cfg.quality = prefs.getInt("q", 12);
  cfg.interval = prefs.getInt("iv", 0);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("cam", false);
  prefs.putString("name", cfg.name);
  prefs.putInt("fs", cfg.framesize);
  prefs.putInt("q", cfg.quality);
  prefs.putInt("iv", cfg.interval);
  prefs.end();
}

static bool validName(const String& n) {
  return n.length() > 0 && n.length() <= NAME_MAX_BYTES;
}

// ======================================================================
// Kamera
// ======================================================================

static int maxFramesize() {
  return psramFound() ? (int)FRAMESIZE_UXGA : (int)FRAMESIZE_SVGA;
}

static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    // Puffer für die größte Auflösung anlegen, damit man später umschalten kann
    c.frame_size = FRAMESIZE_UXGA;
    c.jpeg_quality = 10;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_SVGA;
    c.jpeg_quality = 12;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Kamera-Fehler 0x%x\n", err);
    return false;
  }
  cfg.framesize = constrain(cfg.framesize, 0, maxFramesize());
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)cfg.framesize);
  s->set_quality(s, cfg.quality);
  return true;
}

static void applyCameraSettings() {
  if (!cameraOk) return;
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)cfg.framesize);
  s->set_quality(s, cfg.quality);
}

static void setFlash(int level) {
  flashLevel = constrain(level, 0, 255);
  analogWrite(FLASH_LED_PIN, flashLevel);
}

// ======================================================================
// Status (JSON)
// ======================================================================

static String statusJson() {
  JsonDocument d;
  d["name"] = cfg.name;
  d["fw"] = FW_VERSION;
  d["camera"] = cameraOk;
  d["framesize"] = cfg.framesize;
  d["quality"] = cfg.quality;
  d["interval"] = cfg.interval;
  d["streaming"] = streaming;
  d["flash"] = flashLevel;
  d["uptime"] = millis() / 1000;
  String out;
  serializeJson(d, out);
  return out;
}

static void publishStatus() {
  if (!statusChr) return;
  String s = statusJson();
  statusChr->setValue((const uint8_t*)s.c_str(), s.length());
  // Die App liest nach jeder Notification den vollen Wert neu (Notification
  // kann bei kleiner MTU abgeschnitten sein).
  statusChr->notify();
}

// ======================================================================
// Bluetooth (BLE)
// ======================================================================

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    bleConn = desc->conn_handle;
    // Kurzes Verbindungsintervall = schnellere Bildübertragung
    s->updateConnParams(desc->conn_handle, 6, 12, 0, 400);
    statusDirty = true;
  }
  void onDisconnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    bleConn = BLE_HS_CONN_HANDLE_NONE;
    stopStream = true;
    NimBLEDevice::startAdvertising();
  }
};

class CommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    char buf[CMD_MAX] = {};
    memcpy(buf, v.data(), min(v.size(), CMD_MAX - 1));
    xQueueSend(cmdQueue, buf, 0);
  }
};

static void startBluetooth() {
  NimBLEDevice::init(cfg.name.c_str());
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = bleServer->createService(SERVICE_UUID);
  statusChr = svc->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      COMMAND_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new CommandCallbacks());
  imageChr = svc->createCharacteristic(IMAGE_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  NimBLEDevice::startAdvertising();
  publishStatus();
}

// Sendet ein Paket auf der Bild-Characteristic. Bei vollen Puffern wird
// kurz gewartet und erneut versucht, damit keine Teile verloren gehen.
static bool imageNotify(const uint8_t* data, size_t len) {
  for (int tries = 0; tries < 400; tries++) {
    uint16_t conn = bleConn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return false;
    os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (om && ble_gattc_notify_custom(conn, imageChr->getHandle(), om) == 0) return true;
    delay(5);
  }
  return false;
}

static void imageError(const char* msg) {
  uint8_t buf[64];
  buf[0] = IMG_ERROR;
  size_t n = min(strlen(msg), sizeof(buf) - 1);
  memcpy(buf + 1, msg, n);
  imageNotify(buf, n + 1);
}

static bool sendFrame() {
  if (bleConn == BLE_HS_CONN_HANDLE_NONE) return false;
  if (!cameraOk) {
    imageError("Kamera nicht gefunden");
    return false;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    imageError("Bild fehlgeschlagen");
    return false;
  }

  uint16_t mtu = bleServer->getPeerMTU(bleConn);
  size_t payload = (mtu > 23 ? mtu : 23) - 3;  // ATT-Header
  if (payload > 512) payload = 512;
  size_t chunk = payload - 5;                   // Typ + Offset

  uint32_t no = ++frameNo;
  uint8_t buf[512];
  buf[0] = IMG_START;
  uint32_t len = fb->len;
  uint16_t w = fb->width, h = fb->height;
  memcpy(buf + 1, &len, 4);
  memcpy(buf + 5, &w, 2);
  memcpy(buf + 7, &h, 2);
  memcpy(buf + 9, &no, 4);
  bool ok = imageNotify(buf, 13);

  for (uint32_t off = 0; ok && off < len; off += chunk) {
    size_t n = min((size_t)(len - off), chunk);
    buf[0] = IMG_DATA;
    memcpy(buf + 1, &off, 4);
    memcpy(buf + 5, fb->buf + off, n);
    ok = imageNotify(buf, n + 5);
  }
  esp_camera_fb_return(fb);

  if (ok) {
    buf[0] = IMG_END;
    memcpy(buf + 1, &no, 4);
    ok = imageNotify(buf, 5);
  }
  return ok;
}

static void setName(const String& n) {
  cfg.name = n;
  saveSettings();
  rebootAt = millis() + 500;  // BLE-Name wird beim Neustart übernommen
}

// Befehle kommen als JSON, z.B. {"cmd":"stream","on":true}
static void handleCommand(const char* text) {
  JsonDocument d;
  if (deserializeJson(d, text)) {
    Serial.printf("Ungültiger Befehl: %s\n", text);
    return;
  }
  const char* cmd = d["cmd"] | "";
  Serial.printf("BLE-Befehl: %s\n", cmd);

  if (!strcmp(cmd, "snap")) {
    sendFrame();
    return;
  } else if (!strcmp(cmd, "stream")) {
    streaming = d["on"] | true;
    if (!d["interval"].isNull()) {
      cfg.interval = constrain(d["interval"] | 0, 0, 60000);
      saveSettings();
    }
  } else if (!strcmp(cmd, "cam")) {
    cfg.framesize = constrain(d["framesize"] | cfg.framesize, 0, maxFramesize());
    cfg.quality = constrain(d["quality"] | cfg.quality, 4, 63);
    applyCameraSettings();
    saveSettings();
  } else if (!strcmp(cmd, "flash")) {
    setFlash(d["value"] | 0);
  } else if (!strcmp(cmd, "name")) {
    String n = d["name"] | "";
    n.trim();
    if (validName(n)) setName(n);
  } else if (!strcmp(cmd, "reset")) {
    prefs.begin("cam", false);
    prefs.clear();
    prefs.end();
    rebootAt = millis() + 500;
  } else if (!strcmp(cmd, "reboot")) {
    rebootAt = millis() + 500;
  }
  statusDirty = true;
}

// ======================================================================
// USB-Seriell: wird vom Web-Flasher benutzt, um den Namen zu setzen
//   NAME=<name>   ->  OK NAME=<name>   (danach Neustart)
//   INFO          ->  INFO {json}
// ======================================================================

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (line.length() < 128) line += ch;
      continue;
    }
    line.trim();
    if (line.startsWith("NAME=")) {
      String n = line.substring(5);
      n.trim();
      if (validName(n)) {
        Serial.printf("OK NAME=%s\n", n.c_str());
        Serial.flush();
        setName(n);
      } else {
        Serial.printf("ERR NAME (1-%d Bytes)\n", NAME_MAX_BYTES);
      }
    } else if (line == "INFO") {
      Serial.printf("INFO %s\n", statusJson().c_str());
    }
    line = "";
  }
}

// ======================================================================

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // ESP32-CAM bricht bei Stromspitzen sonst oft ein
  Serial.begin(115200);
  Serial.println("\nHelm Camera BLE " FW_VERSION);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);
  pinMode(FLASH_LED_PIN, OUTPUT);
  setFlash(0);

  loadSettings();
  cameraOk = initCamera();
  cmdQueue = xQueueCreate(8, CMD_MAX);
  startBluetooth();
  Serial.printf("READY NAME=%s\n", cfg.name.c_str());
}

void loop() {
  char cmd[CMD_MAX];
  while (xQueueReceive(cmdQueue, cmd, 0) == pdTRUE) handleCommand(cmd);
  handleSerial();

  if (stopStream) {
    stopStream = false;
    streaming = false;
    setFlash(0);
    statusDirty = true;
  }

  if (streaming && bleConn != BLE_HS_CONN_HANDLE_NONE && millis() - lastFrameAt >= (uint32_t)cfg.interval) {
    lastFrameAt = millis();
    sendFrame();
  }

  static uint32_t lastStatus = 0;
  if (statusDirty || millis() - lastStatus > 5000) {
    statusDirty = false;
    lastStatus = millis();
    publishStatus();
  }

  // Rote LED blitzt kurz alle 2 s, solange kein Handy verbunden ist
  digitalWrite(STATUS_LED_PIN, (bleConn == BLE_HS_CONN_HANDLE_NONE && (millis() % 2000) < 80) ? LOW : HIGH);

  if (rebootAt && millis() > rebootAt) ESP.restart();
  delay(streaming ? 1 : 10);
}
