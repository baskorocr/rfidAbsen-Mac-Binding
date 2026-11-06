/**
 * RFID Attendance - ESP32
 *
 * WIRING (sesuai PCB):
 * ┌─────────────────────────────────────────┐
 * │ MFRC522 (J1)  →  ESP32                  │
 * │  SDA/SS       →  GPIO 5                 │
 * │  RST          →  GPIO 22                │
 * │  SCK          →  GPIO 18                │
 * │  MISO         →  GPIO 19                │
 * │  MOSI         →  GPIO 23                │
 * │  3.3V         →  3V3                    │
 * │  GND          →  GND                    │
 * ├─────────────────────────────────────────┤
 * │ PUSH BUTTON (J2)                        │
 * │  IO4          →  GPIO 4                 │
 * │  GND          →  GND                    │
 * │  (INPUT_PULLUP, active LOW)             │
 * ├─────────────────────────────────────────┤
 * │ LED (J3 / pin 33)                       │
 * │  GPIO 33      →  R → LED → GND          │
 * │  (HIGH = OFF karena common anode/aktif) │
 * └─────────────────────────────────────────┘
 *
 * BUTTON BEHAVIOR:
 *  - Tekan 1x  → masuk MODE REGISTER
 *  - Tekan 1x lagi → kembali ke MODE TAP
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ── PIN DEFINITIONS ──────────────────────────────────────────
#define SS_PIN      5
#define RST_PIN     22
#define BUTTON_PIN  4
#define LED1_PIN    25  // D1
#define LED2_PIN    26  // D2
#define LED_PIN     LED1_PIN  // alias untuk kode lama

// ── DEVICE CONFIG ─────────────────────────────────────────────
const String DEVICE_ID = "00000";
const char*  API_KEY   = "80b598e3952b9f8faeec54525e643d8cf80306fae3d87ade01a2e29b3f74e1a4";

// ── WIFI ──────────────────────────────────────────────────────
const char* WIFI_SSID = "AP-BOD2";
const char* WIFI_PASS = "apdharma258";

// ── API ENDPOINTS (local) ─────────────────────────────────────
const String BASE_URL      = "https://stg-absensi.dharmap.com"; // ganti IP PC kamu
const String TAP_API       = BASE_URL + "/api/rfid/tap";
const String REGISTER_API  = BASE_URL + "/api/rfid/register";
const String HEARTBEAT_API = BASE_URL + "/api/rfid/heartbeat";

// ── RFID ──────────────────────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);

// ── STATE ─────────────────────────────────────────────────────
bool registerMode = false;

// ── SCAN COOLDOWN ─────────────────────────────────────────────
unsigned long lastScanTime = 0;
String        lastUID      = "";
const unsigned long SCAN_COOLDOWN = 2000; // ms, cegah scan ganda

// ── BUTTON DEBOUNCE ───────────────────────────────────────────
bool          btnRawLast       = HIGH;  // raw state sebelumnya
bool          btnStable        = HIGH;  // state stabil setelah debounce
unsigned long btnDebounceTime  = 0;
const unsigned long DEBOUNCE_MS = 200;

// ── LED BLINK (register mode) ─────────────────────────────────
unsigned long lastBlink    = 0;
bool          ledState     = false;
const unsigned long BLINK_INTERVAL = 150; // ms

// ── LED TAP FLASH ─────────────────────────────────────────────
unsigned long ledFlashTime   = 0;
bool          ledFlashActive = false;
const unsigned long FLASH_DURATION = 300; // ms

// ── HEARTBEAT ─────────────────────────────────────────────────
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 60000; // 60 detik

// ── WIFI RECONNECT ────────────────────────────────────────────
unsigned long lastWifiCheck    = 0;
unsigned long wifiReconnectAt  = 0;
bool          wifiReconnecting = false;
const unsigned long WIFI_CHECK_INTERVAL    = 10000;
const unsigned long WIFI_RECONNECT_DELAY   = 5000;

// ── PROTOTYPES ────────────────────────────────────────────────
void connectWiFi();
void checkWiFi(unsigned long now);
void checkButton(unsigned long now);
void handleLED(unsigned long now);
void sendHeartbeat();
void sendToAPI(const String& uid, const String& url);
void setLED(bool on);
void setLED2(bool on);
void startupAnimation();

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  setLED(false);
  setLED2(false);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  startupAnimation();
  connectWiFi();

  // Init SPI & RFID SETELAH WiFi (urutan ini yang bekerja)
  SPI.begin(18, 19, 23, SS_PIN);
  delay(50);
  rfid.PCD_Init();
  delay(50);

  byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print("MFRC522 Version: 0x");
  Serial.println(ver, HEX);

  if (ver == 0xFF || ver == 0x00) {
    Serial.println("ERROR: MFRC522 tidak terdeteksi! Cek wiring.");
    setLED(true);
    while (true) {
      if (digitalRead(BUTTON_PIN) == LOW) {
        delay(50);
        if (digitalRead(BUTTON_PIN) == LOW) ESP.restart();
      }
      yield();
    }
  }

  sendHeartbeat();

  Serial.println("=== RFID Attendance Ready ===");
  Serial.println("Mode: TAP IN / TAP OUT");
}

// ─────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  checkWiFi(now);
  checkButton(now);
  handleLED(now);

  // Heartbeat
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // Baca kartu RFID
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  // Susun UID string
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  Serial.print("UID: ");
  Serial.println(uid);

  if (registerMode) {
    sendToAPI(uid, REGISTER_API);
  } else {
    bool sameUID  = (uid == lastUID);
    bool cooldown = (now - lastScanTime < SCAN_COOLDOWN);

    if (!sameUID || !cooldown) {
      lastUID      = uid;
      lastScanTime = now;
      sendToAPI(uid, TAP_API);
    } else {
      Serial.println("Cooldown aktif, scan diabaikan.");
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ─────────────────────────────────────────────────────────────
// BUTTON: 1x tekan = toggle register mode
// ─────────────────────────────────────────────────────────────
void checkButton(unsigned long now) {
  bool raw = digitalRead(BUTTON_PIN);

  // Reset timer debounce setiap ada perubahan sinyal raw
  if (raw != btnRawLast) {
    btnDebounceTime = now;
    btnRawLast = raw;
  }

  // Baru proses setelah sinyal stabil selama DEBOUNCE_MS
  if ((now - btnDebounceTime) >= DEBOUNCE_MS && raw != btnStable) {
    bool prev   = btnStable;
    btnStable   = raw;

    // Rising edge: tombol dilepas (LOW → HIGH) = 1 klik selesai
    if (prev == LOW && btnStable == HIGH) {
      registerMode = !registerMode;
      if (registerMode) {
        Serial.println(">>> MODE REGISTER RFID <<<");
      } else {
        Serial.println(">>> MODE TAP IN / TAP OUT <<<");
        setLED(false);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────
// LED HANDLER
// ─────────────────────────────────────────────────────────────
void handleLED(unsigned long now) {
  // Flash saat scan kartu (prioritas tertinggi)
  if (ledFlashActive) {
    setLED(true);
    if (now - ledFlashTime >= FLASH_DURATION) {
      ledFlashActive = false;
      setLED(false);
    }
    return;
  }

  // Blink cepat saat register mode
  if (registerMode) {
    if (now - lastBlink >= BLINK_INTERVAL) {
      lastBlink = now;
      ledState  = !ledState;
      setLED(ledState);
    }
  } else {
    setLED(false);
  }
}

// ─────────────────────────────────────────────────────────────
// LED HELPERS
// ─────────────────────────────────────────────────────────────
void setLED(bool on)  { digitalWrite(LED1_PIN, on ? HIGH : LOW); }  // active HIGH
void setLED2(bool on) { digitalWrite(LED2_PIN, on ? HIGH : LOW); }  // active HIGH

void startupAnimation() {
  // LED bergantian 3x
  for (int i = 0; i < 3; i++) {
    setLED(true);  setLED2(false); delay(150);
    setLED(false); setLED2(true);  delay(150);
  }
  setLED(false); setLED2(false);
}

// ─────────────────────────────────────────────────────────────
// WIFI
// ─────────────────────────────────────────────────────────────
void connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");

  // LED2 kedip selama proses connecting
  unsigned long start = millis();
  bool led2State = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    led2State = !led2State;
    setLED2(led2State);
    Serial.print(".");
    unsigned long t = millis();
    while (millis() - t < 500) yield();
  }
  setLED2(false);
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi Connected. IP: ");
    Serial.println(WiFi.localIP());
    // LED1 kedip 2x tanda berhasil connect
    for (int i = 0; i < 2; i++) {
      setLED(true);  delay(200);
      setLED(false); delay(200);
    }
  } else {
    Serial.println("WiFi timeout. Tekan button untuk restart.");
    setLED(true);
    while (true) {
      if (digitalRead(BUTTON_PIN) == LOW) {
        delay(50);
        if (digitalRead(BUTTON_PIN) == LOW) ESP.restart();
      }
      yield();
    }
  }
}

void checkWiFi(unsigned long now) {
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    wifiReconnecting = false;
    return;
  }

  // Jika belum dalam proses reconnect, jadwalkan reconnect
  if (!wifiReconnecting) {
    Serial.println("WiFi putus, jadwal reconnect...");
    WiFi.disconnect(false); // jangan hapus kredensial
    wifiReconnectAt  = now + WIFI_RECONNECT_DELAY;
    wifiReconnecting = true;
    return;
  }

  // Eksekusi reconnect setelah delay
  if (wifiReconnecting && now >= wifiReconnectAt) {
    Serial.println("Reconnecting WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifiReconnecting = false; // reset, cek lagi di interval berikutnya
  }
}

// ─────────────────────────────────────────────────────────────
// API CALLS
// ─────────────────────────────────────────────────────────────
void sendToAPI(const String& uid, const String& url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi tidak terhubung, request dibatalkan.");
    // Kedip merah 2x = error
    for (int i = 0; i < 2; i++) { setLED(true); delay(200); setLED(false); delay(200); }
    return;
  }

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  String body = "{\"rfid_uid\":\"" + uid + "\",\"device_id\":\"" + DEVICE_ID + "\"}";

  Serial.print("POST ");
  Serial.print(url);
  Serial.print(" | Body: ");
  Serial.println(body);

  int code = http.POST(body);
  Serial.print("HTTP Response: ");
  Serial.println(code);

  if (code == 200 || code == 201) {
    Serial.println(http.getString());
    // Kedip hijau 3x = sukses
    for (int i = 0; i < 3; i++) { setLED2(true); delay(150); setLED2(false); delay(150); }
  } else if (code == 409) {
    Serial.println(http.getString());
    // Kedip hijau 1x = kartu sudah terdaftar (bukan error)
    setLED2(true); delay(500); setLED2(false);
  } else if (code > 0) {
    Serial.println(http.getString());
    // Kedip merah 2x = error server
    for (int i = 0; i < 2; i++) { setLED(true); delay(200); setLED(false); delay(200); }
  } else {
    Serial.print("Error: ");
    Serial.println(http.errorToString(code));
    // Kedip merah 3x = tidak bisa konek
    for (int i = 0; i < 3; i++) { setLED(true); delay(200); setLED(false); delay(200); }
  }

  http.end();
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Heartbeat skip: WiFi tidak terhubung.");
    return;
  }

  HTTPClient http;
  http.begin(HEARTBEAT_API);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);

  String body = "{\"device_id\":\"" + DEVICE_ID + "\"}";

  Serial.print("Heartbeat POST -> ");
  Serial.println(HEARTBEAT_API);
  Serial.print("Body: ");
  Serial.println(body);

  int code = http.POST(body);
  Serial.print("Heartbeat HTTP: ");
  Serial.println(code);

  String resp = http.getString();
  Serial.print("Heartbeat Response: ");
  Serial.println(resp);

  http.end();
}
