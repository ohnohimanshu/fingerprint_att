#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── WiFi Credentials ────────────────────────────────────────────────────────
const char* WIFI_SSID     = "Himanshu";
const char* WIFI_PASSWORD = "himanshu";

// ─── Flask Server URL ─────────────────────────────────────────────────────────
const char* SERVER_URL = "http://10.17.5.13:5000";  // Change to your Flask server IP

// ─── Pin Definitions ─────────────────────────────────────────────────────────
#define BUZZER_PIN   25
#define OLED_SDA     21
#define OLED_SCL     22
#define FP_RX_PIN    16
#define FP_TX_PIN    17

// ─── OLED Setup ───────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── Fingerprint Sensor Setup ─────────────────────────────────────────────────
HardwareSerial fpSerial(2);
Adafruit_Fingerprint finger(&fpSerial);

// ─── State Tracking ───────────────────────────────────────────────────────────
bool enrollMode    = false;
int  enrollID      = -1;
unsigned long lastActivityTime = 0;
const unsigned long IDLE_TIMEOUT   = 30000; // 30s back to idle
const unsigned long ENROLL_TIMEOUT = 30000; // 30s per scan step

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
    while (true) delay(10);
  }
  showMessage("Booting...", "", false);

  // Fingerprint sensor
  fpSerial.begin(57600, SERIAL_8N1, FP_RX_PIN, FP_TX_PIN);
  finger.begin(57600);
  delay(100);
  if (!finger.verifyPassword()) {
    showMessage("Sensor Error!", "Check wiring", false);
    while (true) delay(10);
  }
  Serial.println("Fingerprint sensor OK");

  // WiFi
  showMessage("Connecting WiFi", WIFI_SSID, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    showMessage("WiFi Connected", WiFi.localIP().toString().c_str(), false);
    beep(1, 100);
  } else {
    showMessage("WiFi Failed", "Offline mode", false);
  }
  delay(1500);
  showIdle();
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Check for serial commands from Flask (enrollment trigger)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("ENROLL:")) {
      int id = cmd.substring(7).toInt();
      startEnrollMode(id);
    }
  }

  // HTTP polling for enroll commands when WiFi is available
  static unsigned long lastPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > 2000) {
    lastPoll = millis();
    pollForCommands();
  }

  if (enrollMode) {
    handleEnrollment();
  } else {
    handleAttendance();
  }

  // Return to idle after timeout
  if (millis() - lastActivityTime > IDLE_TIMEOUT) {
    showIdle();
    lastActivityTime = millis();
  }
}

// ─── Attendance Scan Mode ─────────────────────────────────────────────────────
void handleAttendance() {
  uint8_t result = finger.getImage();
  if (result != FINGERPRINT_OK) return;

  result = finger.image2Tz();
  if (result != FINGERPRINT_OK) return;

  result = finger.fingerSearch();
  if (result == FINGERPRINT_OK) {
    int fpID       = finger.fingerID;
    int confidence = finger.confidence;
    Serial.printf("Found ID #%d with confidence %d\n", fpID, confidence);
    markAttendance(fpID, confidence);
    lastActivityTime = millis();
  } else if (result == FINGERPRINT_NOTFOUND) {
    showMessage("Not Registered", "Try again", false);
    beep(3, 100);
    delay(2000);
    showIdle();
  }
}

// ─── Send Attendance to Flask ─────────────────────────────────────────────────
void markAttendance(int fpID, int confidence) {
  showMessage("Verifying...", "Please wait", false);

  if (WiFi.status() != WL_CONNECTED) {
    showMessage("No WiFi!", "Cannot mark", false);
    beep(2, 200);
    delay(2000);
    showIdle();
    return;
  }

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/mark-attendance";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["fingerprint_id"] = fpID;
  doc["confidence"]     = confidence;

  String body;
  serializeJson(doc, body);

  int statusCode = http.POST(body);

  if (statusCode == 200) {
    String response = http.getString();
    StaticJsonDocument<256> res;
    deserializeJson(res, response);

    String name   = res["name"]   | "Unknown";
    String action = res["action"] | "Marked";  // "IN" or "OUT"
    String time   = res["time"]   | "";

    String line1 = action == "IN" ? "Welcome IN" : "Goodbye OUT";
    showMessage(line1.c_str(), name.c_str(), true);
    beep(action == "IN" ? 1 : 2, 150);
  } else if (statusCode == 404) {
    showMessage("Not Registered", "Contact admin", false);
    beep(3, 100);
  } else {
    showMessage("Server Error", String(statusCode).c_str(), false);
    beep(2, 300);
  }

  http.end();
  delay(3000);
  showIdle();
}

// ─── Poll for Enroll Command ──────────────────────────────────────────────────
void pollForCommands() {
  HTTPClient http;
  String url = String(SERVER_URL) + "/api/esp32/command";
  http.begin(url);
  http.setTimeout(1000);
  int code = http.GET();

  if (code == 200) {
    String resp = http.getString();
    StaticJsonDocument<128> doc;
    deserializeJson(doc, resp);
    String cmd = doc["command"] | "";

    if (cmd == "ENROLL") {
      int id = doc["fingerprint_id"] | -1;
      if (id > 0) startEnrollMode(id);
    }
  }
  http.end();
}

// ─── Start Enrollment Mode ────────────────────────────────────────────────────
void startEnrollMode(int id) {
  enrollMode       = true;
  enrollID         = id;
  lastActivityTime = millis();
  showMessage("Enroll Mode", ("ID: " + String(id)).c_str(), false);
  beep(2, 80);
  delay(1000);
}

// ─── Enrollment Handler ───────────────────────────────────────────────────────
// FIX BUG 2: Use a local startTime for each wait loop instead of comparing
//            against lastActivityTime (which never updated mid-scan).
// FIX BUG 3: Flush the sensor buffer after the "Remove Finger" delay so a
//            stray touch during that window cannot poison the second scan.
void handleEnrollment() {
  showMessage("Place Finger", ("ID: " + String(enrollID)).c_str(), false);

  // ── Step 1: first scan ────────────────────────────────────────────────────
  unsigned long stepStart = millis();  // FIX BUG 2: independent timeout clock
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

  // FIX BUG 3: Flush any stale image left in the sensor buffer.
  // A touch during the delay() above can pre-load an image that makes the
  // second scan return FINGERPRINT_OK immediately on garbage data, causing
  // createModel() to fail with ENROLLMISMATCH → OLED shows "Try again".
  finger.getImage();   // discard any buffered touch
  delay(200);          // brief settle before asking for the real second scan

  // ── Step 2: second scan ───────────────────────────────────────────────────
  showMessage("Place Again", "Same finger", false);
  stepStart = millis();  // FIX BUG 2: reset timeout for second scan step
  while (finger.getImage() != FINGERPRINT_OK) {
    delay(50);
    if (millis() - stepStart > ENROLL_TIMEOUT) { cancelEnroll(); return; }
  }
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    showMessage("Bad scan", "Retry", false);
    delay(1500);
    return;
  }

  // ── Create model ──────────────────────────────────────────────────────────
  if (finger.createModel() != FINGERPRINT_OK) {
    showMessage("Mismatch!", "Try again", false);
    beep(3, 100);
    delay(2000);
    return;
  }

  // ── Store model ───────────────────────────────────────────────────────────
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

// ─── Notify Flask of Enrollment Result ───────────────────────────────────────
void notifyFlaskEnrolled(int fpID, bool success) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/esp32/enroll-result");
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> doc;
  doc["fingerprint_id"] = fpID;
  doc["success"]        = success;
  String body;
  serializeJson(doc, body);
  http.POST(body);
  http.end();
}

// ─── cancelEnroll ─────────────────────────────────────────────────────────────
// FIX BUG 1: Save enrollID into savedID BEFORE zeroing enrollID.
//            Original code set enrollID = -1 first, then called
//            notifyFlaskEnrolled(enrollID, false) — which sent id=-1.
//            Flask found no student with fingerprint_id=-1, so is_enrolled
//            was never cleared, leaving the student in a broken half-enrolled
//            state that showed "Contact admin" on every subsequent scan.
void cancelEnroll() {
  int savedID = enrollID;   // FIX BUG 1: capture before zeroing
  showMessage("Enroll Timeout", "Cancelled", false);
  beep(2, 200);
  delay(2000);
  enrollMode = false;
  enrollID   = -1;
  notifyFlaskEnrolled(savedID, false);  // FIX BUG 1: use saved ID
  showIdle();
}

// ─── OLED Helpers ─────────────────────────────────────────────────────────────
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

// ─── Buzzer Helpers ───────────────────────────────────────────────────────────
void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(80);
  }
}
