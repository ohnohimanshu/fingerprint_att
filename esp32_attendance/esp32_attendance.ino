/*
 * FingerAttend — ESP32 Firmware
 * Cloud-ready version for AWS EC2 deployment
 *
 * Architecture:
 *   ESP32 always initiates outbound HTTP requests to the EC2 server.
 *   EC2 never connects back to the ESP32 (NAT-safe, works from any network).
 *
 * Hardware:
 *   R307 Fingerprint  TX→GPIO16, RX→GPIO17, VCC→3.3V, GND→GND
 *   SSD1306 OLED      SDA→GPIO21, SCL→GPIO22, VCC→3.3V, GND→GND
 *   Active Buzzer     IO→GPIO25, VCC→VIN(5V), GND→GND
 *
 * Libraries (install via Arduino Library Manager):
 *   Adafruit Fingerprint Sensor Library
 *   Adafruit SSD1306 + Adafruit GFX Library
 *   ArduinoJson
 */

#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ═══════════════════════════════════════════════════════════════════════════════
// CONFIGURATION — edit these before flashing
// ═══════════════════════════════════════════════════════════════════════════════

const char* WIFI_SSID     = "YourWiFiName";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// EC2 public IP or domain — no trailing slash
// Use https:// if you have SSL configured, http:// otherwise
const char* SERVER_URL    = "http://YOUR_EC2_PUBLIC_IP_OR_DOMAIN";

// Must match ESP32_API_KEY in your .env file on the server
const char* API_KEY       = "your-secret-api-key-here";

// ═══════════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════════

#define BUZZER_PIN      25
#define OLED_SDA        21
#define OLED_SCL        22
#define FP_RX_PIN       16
#define FP_TX_PIN       17
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1

const unsigned long IDLE_TIMEOUT      = 30000;  // ms before returning to idle screen
const unsigned long ENROLL_TIMEOUT    = 30000;  // ms to wait for each finger placement
const unsigned long HTTP_TIMEOUT_MS   = 8000;   // ms for HTTP requests
const unsigned long POLL_INTERVAL_MS  = 3000;   // ms between command polls
const unsigned long WIFI_CHECK_MS     = 10000;  // ms between WiFi reconnect attempts
const int           HTTP_MAX_RETRIES  = 3;      // retry count for failed HTTP requests

// ═══════════════════════════════════════════════════════════════════════════════
// GLOBALS
// ═══════════════════════════════════════════════════════════════════════════════

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
HardwareSerial   fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

bool          enrollMode       = false;
int           enrollID         = -1;
unsigned long lastActivityTime = 0;
unsigned long lastWifiCheck    = 0;

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] SSD1306 not found — check wiring");
    while (true) delay(10);
  }
  showMessage("Booting...", "", false);

  // Fingerprint sensor
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(100);
  if (!finger.verifyPassword()) {
    showMessage("Sensor Error!", "Check wiring", false);
    Serial.println("[ERROR] R307 password verification failed");
    while (true) delay(10);
  }
  Serial.println("[OK] Fingerprint sensor ready");

  // WiFi
  connectWiFi();
  showIdle();
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════════════════════

void loop() {
  // Maintain WiFi connection
  maintainWiFi();

  // Poll server for pending enrollment commands (every POLL_INTERVAL_MS)
  static unsigned long lastPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > POLL_INTERVAL_MS) {
    lastPoll = millis();
    pollForCommands();
  }

  // Run the appropriate mode
  if (enrollMode) {
    handleEnrollment();
  } else {
    handleAttendance();
  }

  // Return to idle screen after inactivity
  if (!enrollMode && millis() - lastActivityTime > IDLE_TIMEOUT) {
    showIdle();
    lastActivityTime = millis();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// WIFI MANAGEMENT
// ═══════════════════════════════════════════════════════════════════════════════

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
  showMessage("Connecting WiFi", WIFI_SSID, false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    showMessage("WiFi Connected", WiFi.localIP().toString().c_str(), false);
    beep(1, 100);
    delay(1000);
  } else {
    Serial.println("\n[WiFi] Failed — running in offline mode");
    showMessage("WiFi Failed", "Offline mode", false);
    delay(2000);
  }
}

void maintainWiFi() {
  if (millis() - lastWifiCheck < WIFI_CHECK_MS) return;
  lastWifiCheck = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting...");
    showMessage("WiFi Lost", "Reconnecting...", false);
    WiFi.disconnect();
    delay(500);
    connectWiFi();
    if (!enrollMode) showIdle();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP HELPER — with retry logic
// ═══════════════════════════════════════════════════════════════════════════════

// Performs an HTTP POST with JSON body. Retries up to HTTP_MAX_RETRIES times.
// Returns the HTTP status code, or -1 on total failure.
// Writes the response body into `responseOut`.
int httpPost(const String& path, const String& body, String& responseOut) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(SERVER_URL) + path;

  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);  // server validates this header

    int code = http.POST(body);

    if (code > 0) {
      responseOut = http.getString();
      http.end();
      Serial.printf("[HTTP] POST %s → %d (attempt %d)\n", path.c_str(), code, attempt);
      return code;
    }

    Serial.printf("[HTTP] POST %s failed (attempt %d/%d): %s\n",
                  path.c_str(), attempt, HTTP_MAX_RETRIES,
                  HTTPClient::errorToString(code).c_str());
    http.end();

    if (attempt < HTTP_MAX_RETRIES) delay(1500 * attempt);  // back-off
  }
  return -1;
}

// Performs an HTTP GET. Returns status code, writes body into responseOut.
int httpGet(const String& path, String& responseOut) {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(SERVER_URL) + path;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("X-API-Key", API_KEY);

  int code = http.GET();
  if (code > 0) responseOut = http.getString();
  http.end();

  Serial.printf("[HTTP] GET %s → %d\n", path.c_str(), code);
  return code;
}

// ═══════════════════════════════════════════════════════════════════════════════
// ATTENDANCE
// ═══════════════════════════════════════════════════════════════════════════════

void handleAttendance() {
  uint8_t result = finger.getImage();
  if (result != FINGERPRINT_OK) return;

  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) return;

  result = finger.fingerSearch();
  if (result == FINGERPRINT_OK) {
    int fpID       = finger.fingerID;
    int confidence = finger.confidence;
    Serial.printf("[FP] Match — ID #%d, confidence %d\n", fpID, confidence);
    markAttendance(fpID, confidence);
    lastActivityTime = millis();
  } else if (result == FINGERPRINT_NOTFOUND) {
    showMessage("Not Registered", "Contact admin", false);
    beep(3, 100);
    delay(2000);
    showIdle();
  }
}

void markAttendance(int fpID, int confidence) {
  showMessage("Verifying...", "Please wait", false);

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("No WiFi!", "Cannot mark", false);
    beep(2, 200);
    delay(2000);
    showIdle();
    return;
  }

  // Build JSON payload
  StaticJsonDocument<128> doc;
  doc["fingerprint_id"] = fpID;
  doc["confidence"]     = confidence;
  String body;
  serializeJson(doc, body);

  String response;
  int statusCode = httpPost("/api/mark-attendance", body, response);

  if (statusCode == 200) {
    StaticJsonDocument<256> res;
    DeserializationError err = deserializeJson(res, response);
    if (err) {
      showMessage("Parse Error", "", false);
      beep(2, 200);
    } else {
      String name   = res["name"]   | "Unknown";
      String action = res["action"] | "Marked";
      String line1  = (action == "IN") ? "Welcome IN" : "Goodbye OUT";
      showMessage(line1.c_str(), name.c_str(), true);
      beep((action == "IN") ? 1 : 2, 150);
    }
  } else if (statusCode == 404) {
    showMessage("Not Registered", "Contact admin", false);
    beep(3, 100);
  } else if (statusCode == -1) {
    showMessage("Server Unreachable", "Check network", false);
    beep(2, 300);
  } else {
    showMessage("Server Error", String(statusCode).c_str(), false);
    beep(2, 300);
  }

  delay(3000);
  showIdle();
}

// ═══════════════════════════════════════════════════════════════════════════════
// ENROLLMENT — poll → two-scan → store → notify
// ═══════════════════════════════════════════════════════════════════════════════

void pollForCommands() {
  String response;
  int code = httpGet("/api/esp32/command", response);
  if (code != 200) return;

  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, response)) return;

  String cmd = doc["command"] | "";
  if (cmd == "ENROLL") {
    int id = doc["fingerprint_id"] | -1;
    if (id > 0) startEnrollMode(id);
  }
}

void startEnrollMode(int id) {
  enrollMode       = true;
  enrollID         = id;
  lastActivityTime = millis();
  Serial.printf("[ENROLL] Starting enrollment for slot %d\n", id);
  showMessage("Enroll Mode", ("ID: " + String(id)).c_str(), false);
  beep(2, 80);
  delay(1000);
}

void handleEnrollment() {
  showMessage("Place Finger", ("ID: " + String(enrollID)).c_str(), false);

  // ── Scan 1 ────────────────────────────────────────────────────────────────
  unsigned long stepStart = millis();
  while (finger.getImage() != FINGERPRINT_OK) {
    delay(50);
    if (millis() - stepStart > ENROLL_TIMEOUT) { cancelEnroll(); return; }
  }
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    showMessage("Bad scan", "Try again", false);
    delay(1500);
    return;
  }

  showMessage("Remove Finger", "", false);
  delay(1500);
  while (finger.getImage() != FINGERPRINT_NOFINGER) delay(100);
  finger.getImage();  // flush any stale buffer
  delay(200);

  // ── Scan 2 ────────────────────────────────────────────────────────────────
  showMessage("Place Again", "Same finger", false);
  stepStart = millis();
  while (finger.getImage() != FINGERPRINT_OK) {
    delay(50);
    if (millis() - stepStart > ENROLL_TIMEOUT) { cancelEnroll(); return; }
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    showMessage("Bad scan", "Retry", false);
    delay(1500);
    return;
  }

  // ── Create + store model ──────────────────────────────────────────────────
  if (finger.createModel() != FINGERPRINT_OK) {
    showMessage("Mismatch!", "Try again", false);
    beep(3, 100);
    delay(2000);
    return;
  }

  if (finger.storeModel(enrollID) == FINGERPRINT_OK) {
    showMessage("Enrolled!", ("ID: " + String(enrollID)).c_str(), true);
    beep(3, 80);
    notifyFlaskEnrolled(enrollID, true);
  } else {
    showMessage("Store Failed", "Try again", false);
    beep(2, 300);
    notifyFlaskEnrolled(enrollID, false);
  }

  delay(3000);
  enrollMode = false;
  enrollID   = -1;
  showIdle();
}

void notifyFlaskEnrolled(int fpID, bool success) {
  StaticJsonDocument<128> doc;
  doc["fingerprint_id"] = fpID;
  doc["success"]        = success;
  String body;
  serializeJson(doc, body);

  String response;
  httpPost("/api/esp32/enroll-result", body, response);
}

void cancelEnroll() {
  int savedID = enrollID;  // capture before zeroing
  showMessage("Enroll Timeout", "Cancelled", false);
  beep(2, 200);
  delay(2000);
  enrollMode = false;
  enrollID   = -1;
  notifyFlaskEnrolled(savedID, false);
  showIdle();
}

// ═══════════════════════════════════════════════════════════════════════════════
// DISPLAY HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

void showIdle() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 5);
  display.println("Attendance System");
  display.drawLine(0, 15, 127, 15, SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(8, 28);
  display.println("Scan");
  display.setCursor(8, 48);
  display.println("Finger");
  display.display();
  lastActivityTime = millis();
}

void showMessage(const char* line1, const char* line2, bool success) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  if (success) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    display.drawLine(4, 4, 124, 4, SSD1306_WHITE);
  }
  display.setTextSize(1);
  display.setCursor(4, success ? 10 : 4);
  display.println(line1);
  if (strlen(line2) > 0) {
    display.setTextSize(2);
    display.setCursor(4, success ? 30 : 28);
    display.println(line2);
  }
  display.display();
}

// ═══════════════════════════════════════════════════════════════════════════════
// BUZZER HELPER
// ═══════════════════════════════════════════════════════════════════════════════

void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(80);
  }
}
