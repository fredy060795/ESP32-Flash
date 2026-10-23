// ESP32-CAM (AI-Thinker / ARCELI, OV2640)
//
// - Eigener WLAN-Hotspot (immer an): Handy verbindet sich direkt mit der Kamera,
//   kein Router nötig. Live-Bild unter http://192.168.4.1
// - Optional zusätzlich Heim-WLAN (per Bluetooth einstellbar)
// - Bluetooth Low Energy: Status lesen (inkl. Hotspot-Passwort), WLAN einstellen,
//   Blitz-LED, Auflösung, Einzelbilder direkt über Bluetooth übertragen
//
// Alle Einstellungen bleiben im Flash gespeichert (Preferences / NVS).

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "web_page.h"

#define FW_VERSION "1.0.0"

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

// ---------- Bluetooth UUIDs (müssen zu web/app.html passen) ----------
#define SERVICE_UUID "7b3c0001-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define STATUS_UUID "7b3c0002-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define COMMAND_UUID "7b3c0003-6b1a-4d6e-9c55-3a2f1e0c8a01"
#define IMAGE_UUID "7b3c0004-6b1a-4d6e-9c55-3a2f1e0c8a01"

// Bildübertragung per BLE-Notification, jedes Paket beginnt mit einem Typ-Byte:
//   0x01 Start: u32 Länge, u16 Breite, u16 Höhe (little endian)
//   0x02 Daten: u32 Offset, danach JPEG-Bytes
//   0x03 Ende
//   0x04 Fehler: Text
enum : uint8_t { IMG_START = 1, IMG_DATA = 2, IMG_END = 3, IMG_ERROR = 4 };

struct Settings {
  String name;
  String apSsid;
  String apPass;
  String staSsid;
  String staPass;
  int framesize;
  int quality;
};

static Settings cfg;
static Preferences prefs;
static bool cameraOk = false;
static int flashLevel = 0;

static httpd_handle_t webServer = nullptr;
static httpd_handle_t streamServer = nullptr;

static NimBLEServer* bleServer = nullptr;
static NimBLECharacteristic* statusChr = nullptr;
static NimBLECharacteristic* imageChr = nullptr;
static volatile uint16_t bleConn = BLE_HS_CONN_HANDLE_NONE;
static QueueHandle_t cmdQueue = nullptr;
static const size_t CMD_MAX = 256;

static volatile bool wifiRestart = false;
static volatile bool statusDirty = false;
static uint32_t rebootAt = 0;

// ======================================================================
// Einstellungen
// ======================================================================

static String chipSuffix() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(buf);
}

static void loadSettings() {
  prefs.begin("cam", true);
  String def = "ESP32-CAM-" + chipSuffix();
  cfg.name = prefs.getString("name", def);
  cfg.apSsid = prefs.getString("apSsid", def);
  cfg.apPass = prefs.getString("apPass", "kamera123");
  cfg.staSsid = prefs.getString("staSsid", "");
  cfg.staPass = prefs.getString("staPass", "");
  cfg.framesize = prefs.getInt("fs", FRAMESIZE_VGA);
  cfg.quality = prefs.getInt("q", 12);
  prefs.end();
}

static void saveSettings() {
  prefs.begin("cam", false);
  prefs.putString("name", cfg.name);
  prefs.putString("apSsid", cfg.apSsid);
  prefs.putString("apPass", cfg.apPass);
  prefs.putString("staSsid", cfg.staSsid);
  prefs.putString("staPass", cfg.staPass);
  prefs.putInt("fs", cfg.framesize);
  prefs.putInt("q", cfg.quality);
  prefs.end();
}

// ======================================================================
// Kamera
// ======================================================================

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
  sensor_t* s = esp_camera_sensor_get();
  if (!psramFound() && cfg.framesize > FRAMESIZE_SVGA) cfg.framesize = FRAMESIZE_SVGA;
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
// WLAN
// ======================================================================

static void startWifi() {
  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  delay(100);

  bool useSta = cfg.staSsid.length() > 0;
  WiFi.mode(useSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.length() >= 8 ? cfg.apPass.c_str() : nullptr);
  WiFi.setSleep(false);
  if (useSta) {
    WiFi.setHostname(cfg.name.c_str());
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
  }
  Serial.printf("Hotspot: %s  Passwort: %s  IP: %s\n", cfg.apSsid.c_str(), cfg.apPass.c_str(),
                WiFi.softAPIP().toString().c_str());
}

// ======================================================================
// Status (JSON) – über Bluetooth und /status abrufbar
// ======================================================================

static String statusJson() {
  JsonDocument d;
  d["name"] = cfg.name;
  d["fw"] = FW_VERSION;
  d["camera"] = cameraOk;
  d["ap_ssid"] = cfg.apSsid;
  d["ap_pass"] = cfg.apPass;
  d["ap_ip"] = WiFi.softAPIP().toString();
  d["ap_clients"] = WiFi.softAPgetStationNum();
  d["sta_ssid"] = cfg.staSsid;
  bool staUp = WiFi.status() == WL_CONNECTED;
  d["sta_connected"] = staUp;
  d["sta_ip"] = staUp ? WiFi.localIP().toString() : String("");
  d["framesize"] = cfg.framesize;
  d["quality"] = cfg.quality;
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
// Webserver
// ======================================================================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t indexHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t statusHandler(httpd_req_t* req) {
  String s = statusJson();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, s.c_str(), s.length());
}

static esp_err_t captureHandler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=foto.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t controlHandler(httpd_req_t* req) {
  char query[96], var[32], val[32];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  int v = atoi(val);
  if (!strcmp(var, "framesize")) {
    cfg.framesize = constrain(v, 0, psramFound() ? (int)FRAMESIZE_UXGA : (int)FRAMESIZE_SVGA);
    applyCameraSettings();
    saveSettings();
  } else if (!strcmp(var, "quality")) {
    cfg.quality = constrain(v, 4, 63);
    applyCameraSettings();
    saveSettings();
  } else if (!strcmp(var, "flash")) {
    setFlash(v);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  statusDirty = true;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t streamHandler(httpd_req_t* req) {
  char part[64];
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

static void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;
  config.server_port = 80;
  config.ctrl_port = 32768;

  httpd_uri_t index = {"/", HTTP_GET, indexHandler, nullptr};
  httpd_uri_t status = {"/status", HTTP_GET, statusHandler, nullptr};
  httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, nullptr};
  httpd_uri_t control = {"/control", HTTP_GET, controlHandler, nullptr};
  if (httpd_start(&webServer, &config) == ESP_OK) {
    httpd_register_uri_handler(webServer, &index);
    httpd_register_uri_handler(webServer, &status);
    httpd_register_uri_handler(webServer, &capture);
    httpd_register_uri_handler(webServer, &control);
  }

  // Stream läuft auf eigenem Port, damit die Steuerung parallel funktioniert
  config.server_port = 81;
  config.ctrl_port = 32769;
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, nullptr};
  if (httpd_start(&streamServer, &config) == ESP_OK) {
    httpd_register_uri_handler(streamServer, &stream);
  }
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

static void sendSnapshotBle() {
  if (bleConn == BLE_HS_CONN_HANDLE_NONE) return;
  if (!cameraOk) {
    imageError("Kamera nicht gefunden");
    return;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    imageError("Bild fehlgeschlagen");
    return;
  }

  uint16_t mtu = bleServer->getPeerMTU(bleConn);
  size_t payload = (mtu > 23 ? mtu : 23) - 3;  // ATT-Header
  if (payload > 512) payload = 512;
  size_t chunk = payload - 5;                   // Typ + Offset

  uint8_t buf[512];
  buf[0] = IMG_START;
  uint32_t len = fb->len;
  memcpy(buf + 1, &len, 4);
  uint16_t w = fb->width, h = fb->height;
  memcpy(buf + 5, &w, 2);
  memcpy(buf + 7, &h, 2);
  bool ok = imageNotify(buf, 9);

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
    imageNotify(buf, 1);
  }
}

// Befehle kommen als JSON, z.B. {"cmd":"wifi","ssid":"Heim","pass":"geheim"}
static void handleCommand(const char* text) {
  JsonDocument d;
  if (deserializeJson(d, text)) {
    Serial.printf("Ungültiger Befehl: %s\n", text);
    return;
  }
  const char* cmd = d["cmd"] | "";
  Serial.printf("BLE-Befehl: %s\n", cmd);

  if (!strcmp(cmd, "snap")) {
    sendSnapshotBle();
    return;
  } else if (!strcmp(cmd, "wifi")) {
    cfg.staSsid = d["ssid"] | "";
    cfg.staPass = d["pass"] | "";
    saveSettings();
    wifiRestart = true;
  } else if (!strcmp(cmd, "ap")) {
    String ssid = d["ssid"] | cfg.apSsid;
    String pass = d["pass"] | cfg.apPass;
    if (ssid.length() > 0 && ssid.length() <= 32 && (pass.length() == 0 || pass.length() >= 8)) {
      cfg.apSsid = ssid;
      cfg.apPass = pass;
      saveSettings();
      wifiRestart = true;
    }
  } else if (!strcmp(cmd, "cam")) {
    cfg.framesize = constrain(d["framesize"] | cfg.framesize, 0,
                              psramFound() ? (int)FRAMESIZE_UXGA : (int)FRAMESIZE_SVGA);
    cfg.quality = constrain(d["quality"] | cfg.quality, 4, 63);
    applyCameraSettings();
    saveSettings();
  } else if (!strcmp(cmd, "flash")) {
    setFlash(d["value"] | 0);
  } else if (!strcmp(cmd, "name")) {
    String n = d["name"] | "";
    if (n.length() > 0 && n.length() <= 29) {
      cfg.name = n;
      saveSettings();
      rebootAt = millis() + 500;  // BLE-Name wird beim Neustart übernommen
    }
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

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // ESP32-CAM bricht sonst beim WLAN-Start oft ein
  Serial.begin(115200);
  Serial.println("\nESP32-CAM BLE " FW_VERSION);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, HIGH);
  pinMode(FLASH_LED_PIN, OUTPUT);
  setFlash(0);

  WiFi.mode(WIFI_AP);  // nötig, damit die MAC für den Namen gelesen werden kann
  loadSettings();
  cameraOk = initCamera();

  cmdQueue = xQueueCreate(8, CMD_MAX);
  startWifi();
  startWebServer();
  startBluetooth();
}

void loop() {
  char cmd[CMD_MAX];
  while (xQueueReceive(cmdQueue, cmd, 0) == pdTRUE) handleCommand(cmd);

  if (wifiRestart) {
    wifiRestart = false;
    startWifi();
    statusDirty = true;
  }

  static wl_status_t lastSta = WL_IDLE_STATUS;
  static int lastClients = -1;
  wl_status_t sta = WiFi.status();
  int clients = WiFi.softAPgetStationNum();
  if (sta != lastSta || clients != lastClients) {
    lastSta = sta;
    lastClients = clients;
    statusDirty = true;
  }

  static uint32_t lastStatus = 0;
  if (statusDirty || millis() - lastStatus > 5000) {
    statusDirty = false;
    lastStatus = millis();
    publishStatus();
  }

  // Rote LED blinkt kurz, solange kein Handy per Bluetooth verbunden ist
  digitalWrite(STATUS_LED_PIN, (bleConn == BLE_HS_CONN_HANDLE_NONE && (millis() % 2000) < 80) ? LOW : HIGH);

  if (rebootAt && millis() > rebootAt) ESP.restart();
  delay(10);
}
