/**
 * RFID Attendance - ESP32
 * Setup wizard 3 step + Dashboard monitoring + Live log (SSE)
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

// ── PIN DEFINITIONS ───────────────────────────────────────────
#define SS_PIN      5
#define RST_PIN     22
#define BUTTON_PIN  4
#define LED1_PIN    25
#define LED2_PIN    26

// ── FIXED CONFIG ──────────────────────────────────────────────
const char*  API_KEY       = "80b598e3952b9f8faeec54525e643d8cf80306fae3d87ade01a2e29b3f74e1a4";
const String BASE_URL      = "https://absensi.dharmap.com";
const String TAP_API       = BASE_URL + "/api/rfid/tap";
const String REGISTER_API  = BASE_URL + "/api/rfid/register";
const String HEARTBEAT_API = BASE_URL + "/api/rfid/heartbeat";

// ── DYNAMIC CONFIG ────────────────────────────────────────────
Preferences prefs;
String DEVICE_ID = "";
String WIFI_SSID = "";
String WIFI_PASS = "";
String AP_SSID   = "RFID-Setup";
String AP_PASS   = "";

// ── WEB SERVER ────────────────────────────────────────────────
WebServer server(80);

// ── LIVE LOG (SSE) ────────────────────────────────────────────
#define LOG_BUF_SIZE 30
String logBuffer[LOG_BUF_SIZE];
int    logHead  = 0;
int    logCount = 0;
WiFiClient sseClient;
bool       sseConnected = false;

void addLog(const String& msg) {
  Serial.println(msg);
  logBuffer[logHead] = msg;
  logHead = (logHead + 1) % LOG_BUF_SIZE;
  if (logCount < LOG_BUF_SIZE) logCount++;
  if (sseConnected && sseClient.connected()) {
    String line = msg; line.replace("\n", " ");
    sseClient.print("data: " + line + "\n\n");
  }
}

// ── RFID ──────────────────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);

// ── STATE ─────────────────────────────────────────────────────
bool registerMode = false;

// ── SCAN COOLDOWN ─────────────────────────────────────────────
unsigned long lastScanTime = 0;
String        lastUID      = "";
const unsigned long SCAN_COOLDOWN = 2000;

// ── BUTTON DEBOUNCE ───────────────────────────────────────────
bool          btnRawLast      = HIGH;
bool          btnStable       = HIGH;
unsigned long btnDebounceTime = 0;
const unsigned long DEBOUNCE_MS = 200;

// ── LED ───────────────────────────────────────────────────────
unsigned long lastBlink      = 0;
bool          ledState       = false;
unsigned long ledFlashTime   = 0;
bool          ledFlashActive = false;
const unsigned long BLINK_INTERVAL = 150;
const unsigned long FLASH_DURATION = 300;

// ── HEARTBEAT ─────────────────────────────────────────────────
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 60000;

// ── WIFI RECONNECT ────────────────────────────────────────────
unsigned long lastWifiCheck    = 0;
unsigned long wifiReconnectAt  = 0;
bool          wifiReconnecting = false;
const unsigned long WIFI_CHECK_INTERVAL  = 10000;
const unsigned long WIFI_RECONNECT_DELAY = 5000;

// ── PROTOTYPES ────────────────────────────────────────────────
void startSetupWizard();
void startDashboard();
void connectWiFi();
void checkWiFi(unsigned long now);
void checkButton(unsigned long now);
void handleLED(unsigned long now);
void sendHeartbeat();
void sendToAPI(const String& uid, const String& url);
void setLED(bool on);
void setLED2(bool on);
void startupAnimation();
void stuckWithButton();

// ─────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────
String scanWiFiOptions(String selected = "") {
  int n = WiFi.scanNetworks();
  String opts = "<option value=''>-- Pilih WiFi --</option>";
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    s.replace("&", "&amp;"); s.replace("<", "&lt;");
    String sel = (s == selected) ? " selected" : "";
    opts += "<option value='" + s + "'" + sel + ">" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  return opts;
}

String htmlHead(const String& title) {
  return "<!DOCTYPE html><html><head>"
         "<meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>" + title + "</title>"
         "<style>"
         "body{font-family:sans-serif;max-width:480px;margin:30px auto;padding:0 16px;background:#f5f5f5}"
         "h2{color:#222;margin-bottom:4px}"
         ".card{background:#fff;border-radius:8px;padding:16px;margin-bottom:16px;box-shadow:0 1px 4px rgba(0,0,0,.1)}"
         "label{display:block;margin-top:10px;font-weight:bold;font-size:14px}"
         "select,input[type=text],input[type=password]{width:100%;padding:8px;margin-top:4px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px}"
         ".btn{display:block;width:100%;padding:10px;margin-top:12px;border:none;border-radius:4px;font-size:15px;cursor:pointer;color:#fff}"
         ".btn-blue{background:#0078d4}.btn-red{background:#d43b00}.btn-green{background:#107c10}"
         ".badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:12px;font-weight:bold}"
         ".ok{background:#dff6dd;color:#107c10}.err{background:#fde7e9;color:#d43b00}"
         ".step{color:#888;font-size:13px;margin-bottom:8px}"
         "#log{background:#111;color:#0f0;font-family:monospace;font-size:12px;padding:10px;"
         "border-radius:4px;height:260px;overflow-y:auto;white-space:pre-wrap;word-break:break-all}"
         "</style></head><body>";
}

// ─────────────────────────────────────────────────────────────
// SETUP WIZARD - Step 1: AP credentials
// ─────────────────────────────────────────────────────────────
void handleStep1() {
  String html = htmlHead("Setup - Step 1");
  html += "<div class='card'>"
          "<p class='step'>Langkah 1 dari 3</p>"
          "<h2>Nama &amp; Password ESP</h2>"
          "<form action='/step1save' method='POST'>"
          "<label>Nama WiFi ESP (AP SSID)</label>"
          "<input type='text' name='ap_ssid' value='" + AP_SSID + "' required>"
          "<label>Password WiFi ESP (kosongkan = open)</label>"
          "<input type='password' name='ap_pass'>"
          "<button class='btn btn-blue' type='submit'>Lanjut &rarr;</button>"
          "</form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleStep1Save() {
  String ap_ssid = server.arg("ap_ssid"); ap_ssid.trim();
  String ap_pass = server.arg("ap_pass"); ap_pass.trim();
  if (ap_ssid.isEmpty()) { server.send(400, "text/plain", "AP SSID tidak boleh kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("ap_ssid", ap_ssid);
  prefs.putString("ap_pass", ap_pass);
  prefs.end();
  AP_SSID = ap_ssid; AP_PASS = ap_pass;
  server.sendHeader("Location", "/step2");
  server.send(302, "text/plain", "");
}

// Step 2: Device ID
void handleStep2() {
  String html = htmlHead("Setup - Step 2");
  html += "<div class='card'>"
          "<p class='step'>Langkah 2 dari 3</p>"
          "<h2>Device ID</h2>"
          "<form action='/step2save' method='POST'>"
          "<label>Device ID</label>"
          "<input type='text' name='device_id' value='" + DEVICE_ID + "' placeholder='Contoh: 000001' required>"
          "<p style='font-size:12px;color:#888'>Harus sesuai dengan yang terdaftar di server.</p>"
          "<button class='btn btn-blue' type='submit'>Lanjut &rarr;</button>"
          "</form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleStep2Save() {
  String device_id = server.arg("device_id"); device_id.trim();
  if (device_id.isEmpty()) { server.send(400, "text/plain", "Device ID tidak boleh kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("device_id", device_id);
  prefs.end();
  DEVICE_ID = device_id;
  server.sendHeader("Location", "/step3");
  server.send(302, "text/plain", "");
}

// Step 3: WiFi tujuan
void handleStep3() {
  String opts = scanWiFiOptions(WIFI_SSID);
  String html = htmlHead("Setup - Step 3");
  html += "<div class='card'>"
          "<p class='step'>Langkah 3 dari 3</p>"
          "<h2>WiFi Tujuan</h2>"
          "<form action='/step3save' method='POST'>"
          "<label>Pilih WiFi</label>"
          "<select name='ssid' required>" + opts + "</select>"
          "<label>Password</label>"
          "<input type='password' name='pass'>"
          "<button class='btn btn-green' type='submit'>Simpan &amp; Restart</button>"
          "</form></div></body></html>";
  server.send(200, "text/html", html);
}

void handleStep3Save() {
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass"); pass.trim();
  if (ssid.isEmpty()) { server.send(400, "text/plain", "SSID tidak boleh kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  server.send(200, "text/html",
    htmlHead("Tersimpan") +
    "<div class='card'><h2>&#10003; Tersimpan!</h2>"
    "<p>ESP32 restart dan connect ke <b>" + ssid + "</b>.</p></div></body></html>");
  delay(2000);
  ESP.restart();
}

void startSetupWizard() {
  addLog("=== SETUP WIZARD AKTIF ===");
  addLog("AP: " + AP_SSID + " | Buka 192.168.4.1");

  WiFi.disconnect(true); delay(100);
  WiFi.mode(WIFI_AP); delay(100);
  WiFi.softAP(AP_SSID.c_str(), AP_PASS.isEmpty() ? NULL : AP_PASS.c_str());
  delay(500);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  server.on("/",          HTTP_GET,  handleStep1);
  server.on("/step1save", HTTP_POST, handleStep1Save);
  server.on("/step2",     HTTP_GET,  handleStep2);
  server.on("/step2save", HTTP_POST, handleStep2Save);
  server.on("/step3",     HTTP_GET,  handleStep3);
  server.on("/step3save", HTTP_POST, handleStep3Save);
  server.begin();

  unsigned long lb = 0; bool b = false;
  while (true) {
    server.handleClient();
    unsigned long t = millis();
    if (t - lb >= 500) { lb = t; b = !b; setLED2(b); }
    yield();
  }
}

// ─────────────────────────────────────────────────────────────
// DASHBOARD
// ─────────────────────────────────────────────────────────────
void handleDashboard() {
  String wifiStatus = (WiFi.status() == WL_CONNECTED)
    ? "<span class='badge ok'>&#10003; Terhubung</span> IP: " + WiFi.localIP().toString() + " | RSSI: " + String(WiFi.RSSI()) + " dBm"
    : "<span class='badge err'>&#10007; Tidak Terhubung</span>";

  String html = htmlHead("RFID Monitor");
  html += "<div class='card'>"
          "<h2>&#128268; RFID Monitor</h2>"
          "<p style='font-size:13px;color:#555'>Device ID: <b>" + DEVICE_ID + "</b> | AP: <b>" + AP_SSID + "</b></p>"
          "</div>"
          "<div class='card'><b>Status WiFi</b><br>" + wifiStatus + "</div>"

          "<div class='card'><b>Ganti WiFi Tujuan</b>"
          "<form action='/save-wifi' method='POST'>"
          "<label>SSID</label><select name='ssid'>" + scanWiFiOptions(WIFI_SSID) + "</select>"
          "<label>Password</label><input type='password' name='pass'>"
          "<button class='btn btn-blue' type='submit'>Simpan &amp; Restart</button>"
          "</form></div>"

          "<div class='card'><b>Ganti Nama/Password ESP</b>"
          "<form action='/save-ap' method='POST'>"
          "<label>AP SSID</label><input type='text' name='ap_ssid' value='" + AP_SSID + "'>"
          "<label>AP Password (kosong = open)</label><input type='password' name='ap_pass'>"
          "<button class='btn btn-blue' type='submit'>Simpan &amp; Restart</button>"
          "</form></div>"

          "<div class='card'><b>Ganti Device ID</b>"
          "<form action='/save-device' method='POST'>"
          "<label>Device ID</label><input type='text' name='device_id' value='" + DEVICE_ID + "'>"
          "<button class='btn btn-blue' type='submit'>Simpan &amp; Restart</button>"
          "</form></div>"

          "<div class='card'><b>Reset Device</b>"
          "<p style='font-size:13px;color:#888'>Hapus semua config, kembali ke setup wizard.</p>"
          "<form action='/reset' method='POST'>"
          "<button class='btn btn-red' onclick=\"return confirm('Reset semua config?')\" type='submit'>Reset &amp; Restart</button>"
          "</form></div>"

          "<div class='card'><b>Live Log</b><div id='log'></div></div>"
          "<script>"
          "var log=document.getElementById('log');"
          "var es=new EventSource('/log-stream');"
          "es.onmessage=function(e){log.textContent+=e.data+'\\n';log.scrollTop=log.scrollHeight;};"
          "</script>"
          "</body></html>";

  server.send(200, "text/html", html);
}

void handleSaveWifi() {
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass"); pass.trim();
  if (ssid.isEmpty()) { server.send(400, "text/plain", "SSID kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("ssid", ssid); prefs.putString("pass", pass);
  prefs.end();
  server.send(200, "text/html", htmlHead("OK") + "<div class='card'><h2>Tersimpan</h2><p>Restart...</p></div></body></html>");
  delay(1500); ESP.restart();
}

void handleSaveAP() {
  String ap_ssid = server.arg("ap_ssid"); ap_ssid.trim();
  String ap_pass = server.arg("ap_pass"); ap_pass.trim();
  if (ap_ssid.isEmpty()) { server.send(400, "text/plain", "AP SSID kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("ap_ssid", ap_ssid); prefs.putString("ap_pass", ap_pass);
  prefs.end();
  server.send(200, "text/html", htmlHead("OK") + "<div class='card'><h2>Tersimpan</h2><p>Restart...</p></div></body></html>");
  delay(1500); ESP.restart();
}

void handleSaveDevice() {
  String device_id = server.arg("device_id"); device_id.trim();
  if (device_id.isEmpty()) { server.send(400, "text/plain", "Device ID kosong."); return; }
  prefs.begin("rfid-cfg", false);
  prefs.putString("device_id", device_id);
  prefs.end();
  DEVICE_ID = device_id;
  server.send(200, "text/html", htmlHead("OK") + "<div class='card'><h2>Tersimpan</h2><p>Restart...</p></div></body></html>");
  delay(1500); ESP.restart();
}

void handleReset() {
  prefs.begin("rfid-cfg", false); prefs.clear(); prefs.end();
  server.send(200, "text/html", htmlHead("Reset") + "<div class='card'><h2>Reset!</h2><p>Kembali ke setup wizard...</p></div></body></html>");
  delay(1500); ESP.restart();
}

void handleLogStream() {
  sseClient    = server.client();
  sseConnected = true;
  sseClient.print("HTTP/1.1 200 OK\r\n");
  sseClient.print("Content-Type: text/event-stream\r\n");
  sseClient.print("Cache-Control: no-cache\r\n");
  sseClient.print("Connection: keep-alive\r\n\r\n");
  int start = (logCount < LOG_BUF_SIZE) ? 0 : logHead;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % LOG_BUF_SIZE;
    String line = logBuffer[idx]; line.replace("\n", " ");
    sseClient.print("data: " + line + "\n\n");
  }
}

void startDashboard() {
  server.on("/",            HTTP_GET,  handleDashboard);
  server.on("/save-wifi",   HTTP_POST, handleSaveWifi);
  server.on("/save-ap",     HTTP_POST, handleSaveAP);
  server.on("/save-device", HTTP_POST, handleSaveDevice);
  server.on("/reset",       HTTP_POST, handleReset);
  server.on("/log-stream",  HTTP_GET,  handleLogStream);
  server.begin();
  addLog("Dashboard: http://" + WiFi.softAPIP().toString());
}

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED1_PIN, OUTPUT); pinMode(LED2_PIN, OUTPUT);
  setLED(false); setLED2(false);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  startupAnimation();

  prefs.begin("rfid-cfg", true);
  WIFI_SSID = prefs.getString("ssid",      "");
  WIFI_PASS = prefs.getString("pass",      "");
  DEVICE_ID = prefs.getString("device_id", "");
  AP_SSID   = prefs.getString("ap_ssid",   "RFID-Setup");
  AP_PASS   = prefs.getString("ap_pass",   "");
  prefs.end();

  if (WIFI_SSID.isEmpty() || DEVICE_ID.isEmpty()) {
    addLog("Config belum ada, masuk setup wizard...");
    startSetupWizard();
  }

  connectWiFi();

  SPI.begin(18, 19, 23, SS_PIN);
  delay(50); rfid.PCD_Init(); delay(50);

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  addLog("MFRC522 Version: 0x" + String(ver, HEX));

  if (ver == 0xFF || ver == 0x00) {
    addLog("ERROR: MFRC522 tidak terdeteksi!");
    setLED(true);
    stuckWithButton();
  }

  startDashboard();
  sendHeartbeat();

  addLog("=== RFID Attendance Ready ===");
  addLog("Device ID: " + DEVICE_ID);
  addLog("Mode: TAP IN / TAP OUT");
}

// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  server.handleClient();
  if (sseConnected && !sseClient.connected()) sseConnected = false;

  checkWiFi(now);
  checkButton(now);
  handleLED(now);

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  addLog("UID: " + uid);
  setLED(false); setLED2(false);

  if (registerMode) {
    sendToAPI(uid, REGISTER_API);
  } else {
    bool sameUID  = (uid == lastUID);
    bool cooldown = (now - lastScanTime < SCAN_COOLDOWN);
    if (!sameUID || !cooldown) {
      lastUID = uid; lastScanTime = now;
      sendToAPI(uid, TAP_API);
    } else {
      addLog("Cooldown aktif, scan diabaikan.");
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ─────────────────────────────────────────────────────────────
void stuckWithButton() {
  while (true) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);
      if (digitalRead(BUTTON_PIN) == LOW) ESP.restart();
    }
    yield();
  }
}

void checkButton(unsigned long now) {
  bool raw = digitalRead(BUTTON_PIN);
  if (raw != btnRawLast) { btnDebounceTime = now; btnRawLast = raw; }
  if ((now - btnDebounceTime) >= DEBOUNCE_MS && raw != btnStable) {
    bool prev = btnStable; btnStable = raw;
    if (prev == LOW && btnStable == HIGH) {
      registerMode = !registerMode;
      addLog(registerMode ? ">>> MODE REGISTER RFID <<<" : ">>> MODE TAP IN / TAP OUT <<<");
      if (!registerMode) setLED(false);
    }
  }
}

void handleLED(unsigned long now) {
  if (ledFlashActive) {
    setLED(true);
    if (now - ledFlashTime >= FLASH_DURATION) { ledFlashActive = false; setLED(false); }
    return;
  }
  if (registerMode) {
    if (now - lastBlink >= BLINK_INTERVAL) { lastBlink = now; ledState = !ledState; setLED(ledState); }
  } else { setLED(false); }
}

void setLED(bool on)  { digitalWrite(LED1_PIN, on ? HIGH : LOW); }
void setLED2(bool on) { digitalWrite(LED2_PIN, on ? HIGH : LOW); }

void startupAnimation() {
  for (int i = 0; i < 3; i++) {
    setLED(true); setLED2(false); delay(150);
    setLED(false); setLED2(true); delay(150);
  }
  setLED(false); setLED2(false);
}

void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA); delay(100);
  WiFi.softAP(AP_SSID.c_str(), AP_PASS.isEmpty() ? NULL : AP_PASS.c_str());
  delay(500);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
  addLog("Connecting WiFi: " + WIFI_SSID);

  unsigned long start = millis();
  bool led2State = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    led2State = !led2State; setLED2(led2State);
    unsigned long t = millis();
    while (millis() - t < 500) yield();
  }
  setLED2(false);

  if (WiFi.status() == WL_CONNECTED) {
    addLog("WiFi Connected. IP: " + WiFi.localIP().toString());
    for (int i = 0; i < 2; i++) { setLED(true); delay(200); setLED(false); delay(200); }
  } else {
    addLog("WiFi timeout. Masuk setup wizard...");
    startSetupWizard();
  }
}

void checkWiFi(unsigned long now) {
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;
  if (WiFi.status() == WL_CONNECTED) { wifiReconnecting = false; return; }
  if (!wifiReconnecting) {
    addLog("WiFi putus, jadwal reconnect...");
    WiFi.disconnect(false);
    wifiReconnectAt = now + WIFI_RECONNECT_DELAY;
    wifiReconnecting = true;
    return;
  }
  if (now >= wifiReconnectAt) {
    addLog("Reconnecting WiFi...");
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
    wifiReconnecting = false;
  }
}

void sendToAPI(const String& uid, const String& url) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("WiFi tidak terhubung, request dibatalkan.");
    for (int i = 0; i < 2; i++) { setLED(true); delay(200); setLED(false); delay(200); }
    return;
  }
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  String body = "{\"rfid_uid\":\"" + uid + "\",\"device_id\":\"" + DEVICE_ID + "\"}";
  addLog("POST " + url + " | " + body);
  int code = http.POST(body);
  String resp = http.getString();
  addLog("HTTP " + String(code) + " | " + resp);
  setLED(false); setLED2(false);
  if (code == 200 || code == 201) {
    for (int i = 0; i < 3; i++) { setLED2(true); delay(150); setLED2(false); delay(150); }
  } else if (code == 409) {
    setLED2(true); delay(500); setLED2(false);
  } else {
    for (int i = 0; i < 2; i++) { setLED(true); delay(200); setLED(false); delay(200); }
  }
  setLED(false); setLED2(false);
  http.end();
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) { addLog("Heartbeat skip: WiFi tidak terhubung."); return; }
  HTTPClient http;
  http.begin(HEARTBEAT_API);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  String body = "{\"device_id\":\"" + DEVICE_ID + "\"}";
  addLog("Heartbeat -> " + body);
  int code = http.POST(body);
  addLog("Heartbeat HTTP " + String(code) + " | " + http.getString());
  http.end();
}
