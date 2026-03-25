#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "credentials.h"

// ============================================================
//  SMART CLASSROOM ENERGY MANAGEMENT SYSTEM
//  Occupancy-based lighting control with IR + PIR sensing
//  Web dashboard with manual override
// ============================================================


// --- Web Server ---
WebServer server(80);

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Pin Definitions ---
const int IR1_PIN  = 4;   // IR obstacle sensor 1 (outside, entry side)
const int IR2_PIN  = 5;   // IR obstacle sensor 2 (inside, exit side)
const int PIR_PIN  = 27;   // PIR motion sensor

const int LED_LOW  = 25;
const int LED_MED  = 26;
const int LED_HIGH = 18;

// --- Deployment Configuration ---
// Adjust these thresholds before flashing for each classroom
const int THRESH_MED  = 3;   // occupancy at which MED mode activates
const int THRESH_HIGH = 5;   // occupancy at which HIGH mode activates

// --- Timing Constants ---
const int           DEBOUNCE_MS   = 200;
const unsigned long SEQ_WINDOW_MS = 1200;  // max ms between IR1 and IR2 for valid crossing
const unsigned long FLASH_MS      = 1500; // ms to show ENTRY/EXIT message on LCD
const unsigned long ALERT_LCD_MS  = 5000; // ms to show untracked presence alert on LCD
const unsigned long LCD_UPDATE_MS = 200;  // ms between normal LCD refreshes
const unsigned long WIFI_LCD_MS   = 3000; // ms to show WiFi connected message

// --- Occupancy & Sensor State ---
int           occupancy      = 0;
int           pendingSensor  = 0;
unsigned long pendingTime    = 0;

bool lastIR1 = HIGH;
bool lastIR2 = HIGH;
bool lastPIR = LOW;
bool lastWiFiConnected = false;

unsigned long lastIR1Trigger = 0;
unsigned long lastIR2Trigger = 0;
unsigned long lastLCDUpdate  = 0;
unsigned long pirHighStart   = 0;

// --- Override & Control State ---
bool overrideActive = false;
int  overrideMode   = -1;
bool pirDisabled    = false;
bool systemOff      = false;

// --- LCD Flash State ---
bool          flashActive = false;
unsigned long flashStart  = 0;
unsigned long flashDuration = FLASH_MS;
String        flashLine1  = "";
String        flashLine2  = "";

// --- Event Log ---
struct LogEntry {
  String        message;
  unsigned long timestamp;
};
const int MAX_LOG = 50;
LogEntry  eventLog[MAX_LOG];
int       logCount = 0;

// ============================================================
//  LOGGING
// ============================================================

String formatUptime(unsigned long ms) {
  unsigned long s = ms / 1000;
  unsigned long m = s / 60;
  unsigned long h = m / 60;
  s %= 60; m %= 60;
  char buf[12];
  sprintf(buf, "%02lu:%02lu:%02lu", h, m, s);
  return String(buf);
}

void addLog(String msg) {
  if (logCount < MAX_LOG) {
    eventLog[logCount++] = { msg, millis() };
  } else {
    for (int i = 1; i < MAX_LOG; i++) eventLog[i - 1] = eventLog[i];
    eventLog[MAX_LOG - 1] = { msg, millis() };
  }
  Serial.println("[LOG] " + msg);
}

// ============================================================
//  LIGHTING CONTROL
// ============================================================

void setLights(int mode) {
  if (systemOff) {
    digitalWrite(LED_LOW,  LOW);
    digitalWrite(LED_MED,  LOW);
    digitalWrite(LED_HIGH, LOW);
    return;
  }
  digitalWrite(LED_LOW,  mode >= 1 ? HIGH : LOW);
  digitalWrite(LED_MED,  mode >= 2 ? HIGH : LOW);
  digitalWrite(LED_HIGH, mode >= 3 ? HIGH : LOW);
}

// ============================================================
//  MODE HELPERS
// ============================================================

String getModeName(int mode) {
  switch (mode) {
    case 0:  return "OFF";
    case 1:  return "LOW";
    case 2:  return "MED";
    default: return "HIGH";
  }
}

int calculateMode() {
  if (overrideActive) return overrideMode;
  if (systemOff)      return 0;

  if (occupancy == 0) {
    bool pirNow = !pirDisabled && (bool)digitalRead(PIR_PIN);
    return pirNow ? 1 : 0;
  }
  if (occupancy <= THRESH_MED)  return 1;
  if (occupancy <= THRESH_HIGH) return 2;
  return 3;
}

// ============================================================
//  LCD DISPLAY
// ============================================================

String padTo16(String s) {
  if (s.length() > 16) s = s.substring(0, 16);
  while (s.length() < 16) s += " ";
  return s;
}

void triggerFlash(String line1, String line2, unsigned long durationMs = FLASH_MS) {
  flashLine1  = padTo16(line1);
  flashLine2  = padTo16(line2);
  flashActive = true;
  flashStart  = millis();
  flashDuration = durationMs;
}

void monitorWiFiConnection() {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (!lastWiFiConnected && connected) {
    String ip = WiFi.localIP().toString();
    addLog("WiFi connected | IP: " + ip);
    triggerFlash("WiFi Connected!", "IP: " + ip, WIFI_LCD_MS);
  } else if (lastWiFiConnected && !connected) {
    addLog("WiFi disconnected");
  }

  lastWiFiConnected = connected;
}

void updateLCD(int mode, bool pirState) {

  // Flash screen for ENTRY / EXIT
  if (flashActive) {
    if (millis() - flashStart < flashDuration) {
      lcd.setCursor(0, 0); lcd.print(flashLine1);
      lcd.setCursor(0, 1); lcd.print(flashLine2);
      return;
    }
    flashActive = false;
    lcd.clear();
  }

  // Throttle normal updates to avoid flicker
  if (millis() - lastLCDUpdate < LCD_UPDATE_MS) return;
  lastLCDUpdate = millis();

  String row0, row1;

  if (systemOff) {
    row0 = "  System: OFF   ";
    row1 = "                ";
  } else if (overrideActive) {
    row0 = "Occ:" + String(occupancy) + " Mode:" + getModeName(mode);
    row1 = "OVERRIDE ACTIVE ";
  } else {
    row0 = "Occ:" + String(occupancy) + " Mode:" + getModeName(mode);
    row1 = "Motion: " + String(pirState ? "YES" : "NO ");
  }

  lcd.setCursor(0, 0); lcd.print(padTo16(row0));
  lcd.setCursor(0, 1); lcd.print(padTo16(row1));
}

// ============================================================
//  ENTRY / EXIT LOGIC
// ============================================================

void handleIR(int sensor) {
  unsigned long now = millis();

  if (pendingSensor != 0 && (now - pendingTime) <= SEQ_WINDOW_MS) {

    if (pendingSensor == 1 && sensor == 2) {
      occupancy++;
      addLog("ENTRY detected | Occupancy: " + String(occupancy));
      triggerFlash("  >> ENTRY <<   ", "  Occupancy: " + String(occupancy));
    }
    else if (pendingSensor == 2 && sensor == 1) {
      if (occupancy > 0) occupancy--;
      addLog("EXIT detected | Occupancy: " + String(occupancy));
      triggerFlash("  >> EXIT <<    ", "  Occupancy: " + String(occupancy));
    }

    pendingSensor = 0;

  } else {
    pendingSensor = sensor;
    pendingTime   = now;
  }
}

// ============================================================
//  WEB SERVER ROUTES
// ============================================================

void handleRoot() {
  File f = SPIFFS.open("/dashboard.html", "r");
  if (!f) {
    server.send(500, "text/plain", "dashboard.html not found in SPIFFS");
    return;
  }
  server.streamFile(f, "text/html");
  f.close();
}

void handleStatus() {
  StaticJsonDocument<512> doc;
  doc["occupancy"]      = occupancy;
  doc["mode"]           = getModeName(calculateMode());
  doc["modeIndex"]      = calculateMode();
  doc["motion"]         = !pirDisabled && (bool)digitalRead(PIR_PIN);
  doc["overrideActive"] = overrideActive;
  doc["overrideMode"]   = overrideMode;
  doc["pirDisabled"]    = pirDisabled;
  doc["systemOff"]      = systemOff;
  doc["uptime"]         = formatUptime(millis());

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleLog() {
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.createNestedArray("log");

  for (int i = logCount - 1; i >= 0; i--) {
    JsonObject entry = arr.createNestedObject();
    entry["msg"]  = eventLog[i].message;
    entry["time"] = formatUptime(eventLog[i].timestamp);
  }

  String out;
  serializeJson(doc, out);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", out);
}

void handleOverride() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m <= 3) {
      overrideActive = true;
      overrideMode   = m;
      addLog("OVERRIDE set to: " + getModeName(m));
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleOverrideClear() {
  overrideActive = false;
  overrideMode   = -1;
  addLog("Override cleared — auto mode resumed");
  server.send(200, "text/plain", "OK");
}

void handlePIRToggle() {
  pirDisabled = !pirDisabled;
  addLog(pirDisabled ? "PIR sensor DISABLED" : "PIR sensor ENABLED");
  server.send(200, "text/plain", pirDisabled ? "disabled" : "enabled");
}

void handleOccReset() {
  occupancy = 0;
  addLog("Occupancy manually reset to 0");
  server.send(200, "text/plain", "OK");
}

void handleSystemToggle() {
  systemOff = !systemOff;
  addLog(systemOff ? "System turned OFF manually" : "System turned ON manually");
  server.send(200, "text/plain", systemOff ? "off" : "on");
}

void handleFullReset() {
  occupancy      = 0;
  overrideActive = false;
  overrideMode   = -1;
  pirDisabled    = false;
  systemOff      = false;
  addLog("FULL SYSTEM RESET — all values cleared");
  server.send(200, "text/plain", "OK");
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("============================================");
  Serial.println("  Smart Classroom Energy Management System ");
  Serial.println("============================================");

  // Pin modes
  pinMode(IR1_PIN, INPUT_PULLUP);
  pinMode(IR2_PIN, INPUT_PULLUP);
  pinMode(PIR_PIN,  INPUT_PULLDOWN);
  pinMode(LED_LOW,  OUTPUT);
  pinMode(LED_MED,  OUTPUT);
  pinMode(LED_HIGH, OUTPUT);

  // LCD boot message
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("  Smart System   ");
  lcd.setCursor(0, 1); lcd.print(" Connecting...  ");

  // Mount SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS mount failed — dashboard unavailable");
  } else {
    Serial.println("[SPIFFS] Mounted successfully");
  }

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.println("\n[WIFI] Connected: " + ip);
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("WiFi Connected! ");
    lcd.setCursor(0, 1); lcd.print(ip);
    addLog("System started | IP: " + ip);
    lastWiFiConnected = true;
    delay(3000);   // show IP on LCD long enough to read and note down
  } else {
    Serial.println("\n[WIFI] Failed — running in standalone mode");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("  WiFi FAILED   ");
    lcd.setCursor(0, 1); lcd.print("Standalone mode ");
    addLog("System started — no WiFi");
    lastWiFiConnected = false;
    delay(2000);
  }

  lcd.clear();

  // Register web server routes
  server.on("/",               HTTP_GET,  handleRoot);
  server.on("/status",         HTTP_GET,  handleStatus);
  server.on("/log",            HTTP_GET,  handleLog);
  server.on("/override",       HTTP_POST, handleOverride);
  server.on("/override/clear", HTTP_POST, handleOverrideClear);
  server.on("/pir/toggle",     HTTP_POST, handlePIRToggle);
  server.on("/occ/reset",      HTTP_POST, handleOccReset);
  server.on("/system/toggle",  HTTP_POST, handleSystemToggle);
  server.on("/reset",          HTTP_POST, handleFullReset);

  server.begin();
  Serial.println("[SERVER] Web server started");
  Serial.println("[SYSTEM] Initialisation complete. Ready.");
}

// ============================================================
//  MAIN LOOP
// ============================================================

void loop() {

  // Handle web server requests
  server.handleClient();

  // Show WiFi connection/IP whenever network becomes available later.
  monitorWiFiConnection();

  bool ir1 = digitalRead(IR1_PIN);
  bool ir2 = digitalRead(IR2_PIN);
  bool pir = pirDisabled ? LOW : (bool)digitalRead(PIR_PIN);

  // --- IR Sensor 1 ---
  if (lastIR1 == HIGH && ir1 == LOW && (millis() - lastIR1Trigger > DEBOUNCE_MS)) {
    lastIR1Trigger = millis();
    handleIR(1);
  }

  // --- IR Sensor 2 ---
  if (lastIR2 == HIGH && ir2 == LOW && (millis() - lastIR2Trigger > DEBOUNCE_MS)) {
    lastIR2Trigger = millis();
    handleIR(2);
  }

  // --- PIR Sensor ---
  if (!pirDisabled && lastPIR == LOW && pir == HIGH) {
    if (occupancy == 0) {
      addLog("ALERT: Motion detected with zero occupancy - possible miscounting or unauthorized entry");
      triggerFlash("!! UNTRACKED !!", "Presence Alert!", ALERT_LCD_MS);
    } else {
      addLog("PIR motion detected");
    }
    pirHighStart = millis();
    Serial.print("[DIAG][PIR] HIGH started at ");
    Serial.print(pirHighStart);
    Serial.println(" ms");
  }

  if (lastPIR == HIGH && pir == LOW) {
    unsigned long pirHighMs = (pirHighStart > 0) ? (millis() - pirHighStart) : 0;
    Serial.print("[DIAG][PIR] HIGH ended. Pulse duration: ");
    Serial.print(pirHighMs);
    Serial.println(" ms");
    pirHighStart = 0;
  }

  // --- Reset incomplete IR sequence ---
  if (pendingSensor != 0 && (millis() - pendingTime > SEQ_WINDOW_MS)) {
    pendingSensor = 0;
  }

  // --- Update outputs ---
  int mode = calculateMode();
  setLights(mode);
  updateLCD(mode, pir);

  // --- Serial status ---
  Serial.print("[STATUS] Occ:");
  Serial.print(occupancy);
  Serial.print(" | Mode:");
  Serial.print(getModeName(mode));
  Serial.print(" | Motion:");
  Serial.println(pir ? "YES" : "NO");

  lastIR1 = ir1;
  lastIR2 = ir2;
  lastPIR = pir;

  delay(100);
}