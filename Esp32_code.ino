#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

/* ─────────────────────────────────────────
   WIFI
───────────────────────────────────────── */
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

/* ─────────────────────────────────────────
   N8N WEBHOOKS
───────────────────────────────────────── */
#define INVENTORY_URL  "http://YOUR_PC_IP:5678/webhook/esp32"
#define RFID_SCAN_URL  "http://YOUR_PC_IP:5678/webhook/rfid-scan"

/* ─────────────────────────────────────────
   RFID
───────────────────────────────────────── */
#define SS_PIN  5
#define RST_PIN 27
MFRC522 rfid(SS_PIN, RST_PIN);

/* ─────────────────────────────────────────
   LED PINS FOR BOXES
───────────────────────────────────────── */
int ledPins[5] = {13, 14, 25, 33, 32};  // Box 5 on pin 32 — change if needed

/* ─────────────────────────────────────────
   BUZZER
───────────────────────────────────────── */
#define BUZZER 26

/* ─────────────────────────────────────────
   SCHEDULE STORAGE  (unchanged)
───────────────────────────────────────── */
String scheduleTimes[5][10];
int    scheduleCount[5] = {0};

/* ─────────────────────────────────────────
   STATE VARIABLES  (unchanged)
───────────────────────────────────────── */
bool          windowActive[5]   = {false, false, false, false, false};
bool          doseTaken[5]      = {false, false, false, false, false};
unsigned long windowStart[5]    = {0, 0, 0, 0, 0};
unsigned long buzzCycleStart[5] = {0, 0, 0, 0, 0};

const unsigned long windowDuration = 3600000;  // 1 hour
const unsigned long buzzOnTime     = 120000;   // 2 minutes
const unsigned long buzzOffTime    = 120000;   // 2 minutes

/* ─────────────────────────────────────────
   REGISTRATION MODE
   When NO dispensing window is active and
   the user taps the card, the UID is sent
   to n8n /rfid-scan so the web UI can
   detect it and unlock the Confirm button.
───────────────────────────────────────── */
unsigned long lastRegistrationSend = 0;
const unsigned long regCooldown    = 5000;  // 5 s between sends (debounce)


/* ─────────────────────────────────────────
   WIFI CONNECTION
───────────────────────────────────────── */
void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("WiFi Connected");
  Serial.println(WiFi.localIP());
}

/* ─────────────────────────────────────────
   TIME SYNC
───────────────────────────────────────── */
void syncTime() {
  configTime(19800, 0, "pool.ntp.org");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) delay(500);
  Serial.println("Time Synced");
}

/* ─────────────────────────────────────────
   FETCH SCHEDULE FROM N8N  (unchanged)
───────────────────────────────────────── */
void fetchSchedule() {
  HTTPClient http;
  http.begin(INVENTORY_URL);
  int code = http.GET();

  if (code != 200) {
    Serial.println("Schedule fetch failed");
    http.end();
    return;
  }

  String payload = http.getString();
  DynamicJsonDocument doc(4096);
  deserializeJson(doc, payload);

  for (int i = 0; i < 5; i++) scheduleCount[i] = 0;

  Serial.println("------ Schedule Received ------");

  for (JsonObject item : doc.as<JsonArray>()) {
    int box = item["box"];
    if (box < 1 || box > 5) continue;  // skip invalid box numbers
    JsonArray times = item["times"];

    Serial.print("Box ");
    Serial.println(box);

    int index = 0;
    for (String t : times) {
      if (index >= 10) break;  // max 10 times per box
      scheduleTimes[box - 1][index] = t;
      Serial.print("  Time: ");
      Serial.println(t);
      index++;
    }
    scheduleCount[box - 1] = index;
  }

  Serial.println("-------------------------------");
  http.end();
}

/* ─────────────────────────────────────────
   READ RFID UID AS HEX STRING
   Returns "" if no card present.
───────────────────────────────────────── */
String readRFIDUid() {
  if (!rfid.PICC_IsNewCardPresent()) return "";
  if (!rfid.PICC_ReadCardSerial())   return "";

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.print("RFID UID: ");
  Serial.println(uid);
  return uid;
}

/* ─────────────────────────────────────────
   SEND UID TO N8N (registration mode)
   POST { "uid": "<hex>" }
───────────────────────────────────────── */
void sendRFIDToN8N(const String& uid) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(RFID_SCAN_URL);
  http.addHeader("Content-Type", "application/json");

  String body = "{\"uid\":\"" + uid + "\"}";
  int code = http.POST(body);

  Serial.print("RFID-scan webhook response: ");
  Serial.println(code);

  http.end();
}

/* ─────────────────────────────────────────
   DISPENSE MEDICINE  (unchanged)
───────────────────────────────────────── */
void dispenseBox(int i) {
  Serial.print("Dispensing Box ");
  Serial.println(i + 1);
  digitalWrite(ledPins[i], HIGH);
  delay(10000);
  digitalWrite(ledPins[i], LOW);
}

/* ─────────────────────────────────────────
   SETUP
───────────────────────────────────────── */
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 5; i++) {
    pinMode(ledPins[i], OUTPUT);
    digitalWrite(ledPins[i], LOW);
  }

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  SPI.begin();
  rfid.PCD_Init();

  connectWiFi();
  syncTime();
  fetchSchedule();

  Serial.println("System Ready");
}

/* ─────────────────────────────────────────
   LOOP
───────────────────────────────────────── */
void loop() {

  /* ── Periodic schedule refresh (unchanged) ── */
  static unsigned long lastFetch = 0;
  if (millis() - lastFetch > 60000) {
    fetchSchedule();
    lastFetch = millis();
  }

  /* ── Get current time (unchanged) ── */
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  char current[6];
  sprintf(current, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

  /* ── Check whether ANY dispensing window is active ── */
  bool anyWindowActive = false;
  for (int b = 0; b < 5; b++) {
    if (windowActive[b]) { anyWindowActive = true; break; }
  }

  /* ────────────────────────────────────────────────────
     REGISTRATION MODE
     If no dispensing window is open, a card tap sends
     the UID to n8n so the web UI can authenticate the
     user before they click Confirm.
  ──────────────────────────────────────────────────── */
  if (!anyWindowActive) {
    if (millis() - lastRegistrationSend > regCooldown) {
      String uid = readRFIDUid();
      if (uid.length() > 0) {
        Serial.println("Registration mode: sending UID to n8n");
        sendRFIDToN8N(uid);
        lastRegistrationSend = millis();
      }
    }
  }

  /* ────────────────────────────────────────────────────
     DISPENSING LOGIC  (completely unchanged)
  ──────────────────────────────────────────────────── */
  for (int box = 0; box < 5; box++) {

    /* Trigger reminder window */
    for (int t = 0; t < scheduleCount[box]; t++) {
      if (scheduleTimes[box][t] == String(current) &&
          !windowActive[box] &&
          !doseTaken[box]) {

        windowActive[box]    = true;
        windowStart[box]     = millis();
        buzzCycleStart[box]  = millis();

        Serial.print("Reminder started for Box ");
        Serial.println(box + 1);
      }
    }

    if (windowActive[box]) {

      /* Window expiry */
      if (millis() - windowStart[box] > windowDuration) {
        windowActive[box] = false;
        doseTaken[box]    = true;
        digitalWrite(BUZZER, LOW);
        Serial.print("Reminder window expired for Box ");
        Serial.println(box + 1);
        continue;
      }

      /* Buzzer cycle */
      unsigned long elapsed = millis() - buzzCycleStart[box];
      if (elapsed < buzzOnTime) {
        digitalWrite(BUZZER, HIGH);
      } else if (elapsed < buzzOnTime + buzzOffTime) {
        digitalWrite(BUZZER, LOW);
      } else {
        buzzCycleStart[box] = millis();
      }

      /* RFID tap → dispense */
      String uid = readRFIDUid();
      if (uid.length() > 0) {
        digitalWrite(BUZZER, LOW);
        dispenseBox(box);
        windowActive[box] = false;
        doseTaken[box]    = true;
        Serial.print("Dose taken for Box ");
        Serial.println(box + 1);
      }
    }

    /* Daily dose reset at midnight */
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
      doseTaken[box] = false;
    }
  }

  delay(200);
}
