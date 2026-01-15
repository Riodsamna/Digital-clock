// =======================================================
// Lauko laikrodis v1.5
// - Laikas + temperatūra + data
// - Web UI su realiais duomenimis (lentelė)
// - Fade perėjimai
// - LDR ryškumas (auto/manual)
// - Naktinis režimas + testas
// - DS18B20 temperatūra + offset
// - DS3231 + NTP + DST (EU) + laiko zona
// - Temperatūros grafikas (24h)
// - Spalvų tema pagal temperatūrą (konfigūruojama per Web UI)
// - Temperatūros apsauga nuo perkaitimo
// - RTC baterijos diagnostika
// - LED test / LED test 2 / reboot / factory reset
// - Real-time spalvų preview
// - Web OTA + ArduinoOTA
// - Login su slaptažodžiu + server-side sesija
// - v1.5: tikras sesijos timeout + AJAX heartbeat + konfigūruojama temp spalvų sistema
// =======================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <RTClib.h>
#include <FastLED.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>

// ====================== PINAI ======================

#define DATA_PIN       D4
#define NUM_LEDS       118
#define COLOR_ORDER    GRB

#define ONE_WIRE_BUS   D5
#define LDR_PIN        A0
#define RESET_BTN_PIN  D6

#define DNS_PORT       53

const char* AP_SSID     = "Laikrodis-Setup";
const char* AP_PASSWORD = "Laikrodis2026";

#define EEPROM_SIZE    4096
#define EEPROM_MAGIC   0x43   // v1.5 naujas formatas

const int  DST_OFFSET_SEC = 2;

// ====================== OBJEKTAI ======================

CRGB leds[NUM_LEDS];

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
RTC_DS3231 rtc;

ESP8266WebServer server(80);
DNSServer dnsServer;
ESP8266HTTPUpdateServer httpUpdater;

// ====================== SKAITMENŲ MASYVAS ======================

byte digits[12][28] = {
  {0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},  // 0
  {0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},  // 1
  {1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},  // 2
  {1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},  // 3
  {1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1},  // 4
  {1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1},  // 5
  {1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},  // 6
  {0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},  // 7
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},  // 8
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,1,1},  // 9
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},  // -
  {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0}   // C
};

// ====================== KONFIGŪRACIJA ======================

// v1.5 – temperatūros spalvų sistema
const uint8_t TEMP_COLOR_ZONES = 6;

struct TempColorZone {
  int8_t fromC;
  int8_t toC;
  uint8_t r, g, b;
};

struct StoredConfig {
  uint8_t magic;
  char wifiSsid[32];
  char wifiPass[64];

  bool useDST;
  bool useFade;

  bool showTime;
  bool showTemp;
  bool showDate;

  uint16_t tempPeriodSec;
  uint8_t  tempDurationSec;
  uint8_t  dateDurationSec;

  uint8_t colorTimeR, colorTimeG, colorTimeB;
  uint8_t colorTempR, colorTempG, colorTempB;
  uint8_t colorDateR, colorDateG, colorDateB;

  float tempOffset;

  bool nightModeEnabled;
  uint8_t nightStartHour;
  uint8_t nightStartMinute;
  uint8_t nightEndHour;
  uint8_t nightEndMinute;

  bool autoBrightness;
  uint8_t manualBrightness;

  bool tempColorMode;      // ar naudoti temp spalvų sistemą
  int8_t timezoneOffset;

  char webPassword[32];

  // v1.5 – temperatūros spalvų sistema
  bool tempColorAdvancedEnabled;   // ar naudoti pažangią sistemą
  bool tempColorSmooth;            // sklandus perėjimas ar zoninis
  uint8_t tempColorStep;           // žingsnis (1,2,5,10)
  TempColorZone tempZones[TEMP_COLOR_ZONES];
};

StoredConfig cfg;

CRGB colorTime;
CRGB colorTemp;
CRGB colorDate;

enum DisplayMode {
  MODE_TIME,
  MODE_TEMP,
  MODE_DATE
};

DisplayMode currentMode = MODE_TIME;

unsigned long modeStartMillis = 0;
unsigned long lastTempCycleMillis = 0;

bool haveNtpTime = false;
bool wifiConnected = false;

int lastTempC = 0;
bool tempInitialized = false;

int lastLdrRaw = 0;
int lastBrightness = 80;

bool btnLastState = HIGH;
unsigned long btnPressStart = 0;
bool btnHandled = false;

// temperatūros istorija
const int TEMP_HISTORY_SIZE = 144;
int16_t tempHistory[TEMP_HISTORY_SIZE];
uint8_t tempHistoryIndex = 0;
unsigned long lastTempHistorySave = 0;

// naktinio režimo testas
bool forceNightMode = false;
unsigned long forceNightModeUntil = 0;

// v1.5 – server-side sesija + heartbeat
bool sessionActive = false;
unsigned long sessionExpiresAt = 0;
unsigned long lastRequestAt = 0;
const unsigned long SESSION_TIMEOUT_MS = 1800000UL; // 30 min

// ====================== PAGALBINĖS FUNKCIJOS ======================

bool isAPMode() {
  return (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) && !wifiConnected;
}

bool isSessionValid() {
  if (isAPMode()) return true; // AP režime login nereikalingas
  if (!sessionActive) return false;
  unsigned long nowMs = millis();
  if (nowMs - lastRequestAt > SESSION_TIMEOUT_MS) return false;
  if (nowMs > sessionExpiresAt) return false;
  return true;
}

void refreshSession() {
  if (!isAPMode() && sessionActive) {
    unsigned long nowMs = millis();
    lastRequestAt = nowMs;
    sessionExpiresAt = nowMs + SESSION_TIMEOUT_MS;
  }
}

bool checkAuth() {
  if (isAPMode()) return true;
  if (!isSessionValid()) return false;
  refreshSession();
  return true;
}

void redirectToLogin() {
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

// ====================== KONFIGŪRACIJA ======================

void loadDefaultTempZones() {
  // 6 zonos: [-50;-10], [-10;0], [0;10], [10;20], [20;30], [30;50]
  cfg.tempZones[0] = { -50, -10, 180, 220, 255 }; // šaltas melsvas
  cfg.tempZones[1] = { -10, 0,   0,   200, 255 }; // mėlynas
  cfg.tempZones[2] = { 0,   10,  0,   255, 180 }; // melsvai žalias
  cfg.tempZones[3] = { 10,  20,  0,   255, 0   }; // žalias
  cfg.tempZones[4] = { 20,  30,  255, 200, 0   }; // geltonas
  cfg.tempZones[5] = { 30,  50,  255, 0,   0   }; // raudonas
}

void loadDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = EEPROM_MAGIC;

  strcpy(cfg.wifiSsid, "");
  strcpy(cfg.wifiPass, "");

  cfg.useDST  = true;
  cfg.useFade = true;

  cfg.showTime = true;
  cfg.showTemp = true;
  cfg.showDate = true;

  cfg.tempPeriodSec   = 20;
  cfg.tempDurationSec = 4;
  cfg.dateDurationSec = 4;

  cfg.colorTimeR = 153; cfg.colorTimeG = 50;  cfg.colorTimeB = 204;
  cfg.colorTempR = 0;   cfg.colorTempG = 255; cfg.colorTempB = 255;
  cfg.colorDateR = 0;   cfg.colorDateG = 255; cfg.colorDateB = 0;

  cfg.tempOffset = 0.0f;

  cfg.nightModeEnabled = true;
  cfg.nightStartHour   = 23;
  cfg.nightStartMinute = 0;
  cfg.nightEndHour     = 6;
  cfg.nightEndMinute   = 0;

  cfg.autoBrightness   = true;
  cfg.manualBrightness = 80;

  cfg.tempColorMode    = false;
  cfg.timezoneOffset   = 2;

  strcpy(cfg.webPassword, "admin");

  // v1.5 – temp spalvų sistema
  cfg.tempColorAdvancedEnabled = true;
  cfg.tempColorSmooth = true;
  cfg.tempColorStep = 1;
  loadDefaultTempZones();
}

void loadConfigFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);

  if (cfg.magic != EEPROM_MAGIC) {
    loadDefaults();
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }

  if (cfg.webPassword[0] == '\0') {
    strcpy(cfg.webPassword, "admin");
  }

  colorTime = CRGB(cfg.colorTimeR, cfg.colorTimeG, cfg.colorTimeB);
  colorTemp = CRGB(cfg.colorTempR, cfg.colorTempG, cfg.colorTempB);
  colorDate = CRGB(cfg.colorDateR, cfg.colorDateG, cfg.colorDateB);
}

void saveConfigToEEPROM() {
  cfg.colorTimeR = colorTime.r;
  cfg.colorTimeG = colorTime.g;
  cfg.colorTimeB = colorTime.b;

  cfg.colorTempR = colorTemp.r;
  cfg.colorTempG = colorTemp.g;
  cfg.colorTempB = colorTemp.b;

  cfg.colorDateR = colorDate.r;
  cfg.colorDateG = colorDate.g;
  cfg.colorDateB = colorDate.b;

  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void factoryReset() {
  loadDefaults();
  saveConfigToEEPROM();
}

// ====================== WI-FI ======================

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(300);

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  wifiConnected = false;
}

void connectWiFi() {
  if (strlen(cfg.wifiSsid) == 0) {
    startAPMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (!wifiConnected) startAPMode();
}

// ====================== NTP / DS3231 ======================

bool isDST_EU(const DateTime& dt) {
  int year = dt.year();

  DateTime lastMar(year, 3, 31, 1, 0, 0);
  while (lastMar.dayOfTheWeek() != 0) lastMar = lastMar - TimeSpan(1,0,0,0);

  DateTime lastOct(year, 10, 31, 1, 0, 0);
  while (lastOct.dayOfTheWeek() != 0) lastOct = lastOct - TimeSpan(1,0,0,0);

  return (dt >= lastMar && dt < lastOct);
}

void setupTimeNTP() {
  if (!wifiConnected) {
    haveNtpTime = false;
    return;
  }

  configTime(cfg.timezoneOffset * 3600, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

  struct tm ti;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&ti)) {
      haveNtpTime = true;

      rtc.adjust(DateTime(
        ti.tm_year + 1900,
        ti.tm_mon + 1,
        ti.tm_mday,
        ti.tm_hour,
        ti.tm_min,
        ti.tm_sec
      ));
      return;
    }
    delay(500);
  }

  haveNtpTime = false;
}

bool getCurrentDateTime(DateTime &out) {
  if (haveNtpTime && wifiConnected) {
    struct tm ti;
    if (getLocalTime(&ti)) {
      out = DateTime(
        ti.tm_year + 1900,
        ti.tm_mon + 1,
        ti.tm_mday,
        ti.tm_hour,
        ti.tm_min,
        ti.tm_sec
      );
    } else {
      haveNtpTime = false;
      out = rtc.now();
    }
  } else {
    out = rtc.now();
  }

  if (cfg.useDST && isDST_EU(out)) {
    out = out + TimeSpan(0, 1, 0, 0);
  }

  return true;
}

// ====================== TEMPERATŪRA ======================

void updateTemperature() {
  sensors.requestTemperatures();
  int newTemp = (int)sensors.getTempCByIndex(0);

  if (!tempInitialized) {
    lastTempC = newTemp;
    tempInitialized = true;
  } else {
    lastTempC = newTemp;
  }
}

void updateTempHistory() {
  unsigned long nowMs = millis();
  if (nowMs - lastTempHistorySave >= 600000UL) {
    lastTempHistorySave = nowMs;
    int16_t t = (int16_t)(lastTempC * 10);
    tempHistory[tempHistoryIndex] = t;
    tempHistoryIndex++;
    if (tempHistoryIndex >= TEMP_HISTORY_SIZE) tempHistoryIndex = 0;
  }
}

// ====================== TEMP SPALVOS v1.5 ======================

CRGB colorFromZone(const TempColorZone &z) {
  return CRGB(z.r, z.g, z.b);
}

CRGB getTempColorAdvanced(int tempC) {
  // jei išjungta – fallback į default
  if (!cfg.tempColorMode || !cfg.tempColorAdvancedEnabled) {
    // senoji logika
    if (tempC <= -10)      return CRGB(180, 220, 255);
    else if (tempC < 0)    return CRGB(0, 200, 255);
    else if (tempC < 10)   return CRGB(0, 255, 180);
    else if (tempC < 20)   return CRGB(0, 255, 0);
    else if (tempC < 30)   return CRGB(255, 200, 0);
    else                   return CRGB(255, 0, 0);
  }

  // surandam zoną
  for (uint8_t i = 0; i < TEMP_COLOR_ZONES; i++) {
    TempColorZone &z = cfg.tempZones[i];
    if (tempC >= z.fromC && tempC < z.toC) {
      if (!cfg.tempColorSmooth) {
        return colorFromZone(z);
      } else {
        // sklandus perėjimas tarp šios ir kitos zonos (jei yra)
        if (i == TEMP_COLOR_ZONES - 1) {
          return colorFromZone(z);
        }
        TempColorZone &z2 = cfg.tempZones[i+1];
        int range = z.toC - z.fromC;
        if (range <= 0) return colorFromZone(z);
        float t = (float)(tempC - z.fromC) / (float)range;
        uint8_t r = z.r + (uint8_t)((z2.r - z.r) * t);
        uint8_t g = z.g + (uint8_t)((z2.g - z.g) * t);
        uint8_t b = z.b + (uint8_t)((z2.b - z.b) * t);
        return CRGB(r, g, b);
      }
    }
  }

  // jei nepataikė į jokį intervalą – naudoti kraštines
  if (tempC < cfg.tempZones[0].fromC) {
    return colorFromZone(cfg.tempZones[0]);
  }
  return colorFromZone(cfg.tempZones[TEMP_COLOR_ZONES-1]);
}

void updateTempColorsIfNeeded() {
  if (cfg.tempColorMode) {
    CRGB tc = getTempColorAdvanced(lastTempC);
    colorTime = tc;
    colorTemp = tc;
    colorDate = tc;
  }
}

// ====================== LDR / RYŠKUMAS ======================

void updateBrightness() {
  if (cfg.autoBrightness) {
    lastLdrRaw = analogRead(LDR_PIN);

    int mapped = map(lastLdrRaw, 0, 1023, 10, 255);
    mapped = constrain(mapped, 10, 255);

    lastBrightness = (lastBrightness * 7 + mapped) / 8;
  } else {
    lastLdrRaw = analogRead(LDR_PIN);
    lastBrightness = cfg.manualBrightness;
  }

  if (lastTempC > 60)      lastBrightness = min(lastBrightness, 40);
  else if (lastTempC > 50) lastBrightness = min(lastBrightness, 80);
  else if (lastTempC > 40) lastBrightness = min(lastBrightness, 150);

  if (forceNightMode) {
    if (millis() < forceNightModeUntil) {
      lastBrightness = min(lastBrightness, 15);
    } else {
      forceNightMode = false;
    }
  }

  DateTime now;
  getCurrentDateTime(now);

  if (cfg.nightModeEnabled) {
    int curMinutes = now.hour() * 60 + now.minute();
    int startM = cfg.nightStartHour * 60 + cfg.nightStartMinute;
    int endM   = cfg.nightEndHour   * 60 + cfg.nightEndMinute;

    bool inNight = false;
    if (startM < endM) {
      inNight = (curMinutes >= startM || curMinutes < endM);
    } else {
      inNight = (curMinutes >= startM && curMinutes < endM);
    }

    if (inNight) {
      lastBrightness = min(lastBrightness, 15);
    }
  }

  FastLED.setBrightness(lastBrightness);
}

// ====================== LED RODYMAS ======================

void clearAll() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}

void drawTime(const DateTime &now) {
  clearAll();

  int hour   = now.hour();
  int minute = now.minute();
  int second = now.second();

  bool dotOn = (second % 2 == 0);

  CRGB dotColor = (haveNtpTime && wifiConnected) ? colorTime : CRGB::Red;

  leds[60] = dotOn ? dotColor : CRGB::Black;
  leds[61] = dotOn ? dotColor : CRGB::Black;

  int full = hour * 100 + minute;
  int cursor;
  int digit;

  for (int i = 1; i <= 4; i++) {
    digit = full % 10;

    cursor = (i == 1 ? 90 :
             (i == 2 ? 62 :
             (i == 3 ? 32 : 4)));

    for (int k = 0; k < 28; k++)
      leds[cursor++] = digits[digit][k] ? colorTime : CRGB::Black;

    full /= 10;
  }
}

void drawTemperature() {
  clearAll();

  float corrected = (float)lastTempC + cfg.tempOffset;
  int celsius = (int)round(corrected);

  int cursor;

  cursor = 90;
  for (int k = 0; k < 28; k++)
    leds[cursor++] = digits[11][k] ? colorTemp : CRGB::Black;

  cursor = 62;
  for (int k = 0; k < 28; k++)
    leds[cursor++] = digits[10][k] ? colorTemp : CRGB::Black;

  int digit = abs(celsius % 10);
  cursor = 32;
  for (int k = 0; k < 28; k++)
    leds[cursor++] = digits[digit][k] ? colorTemp : CRGB::Black;

  digit = abs(celsius / 10);
  cursor = 4;
  for (int k = 0; k < 28; k++)
    leds[cursor++] = digits[digit][k] ? colorTemp : CRGB::Black;

  for (int k = 0; k < 4; k++)
    leds[k] = (celsius < 0) ? colorTemp : CRGB::Black;
}

void drawDate(const DateTime &now) {
  clearAll();

  int value = now.month() * 100 + now.day();
  int cursor;
  int digit;

  for (int i = 1; i <= 4; i++) {
    digit = value % 10;

    cursor = (i == 1 ? 90 :
             (i == 2 ? 62 :
             (i == 3 ? 32 : 4)));

    for (int k = 0; k < 28; k++)
      leds[cursor++] = digits[digit][k] ? colorDate : CRGB::Black;

    value /= 10;
  }
}

// ====================== FADE ======================

void fadeToMode(DisplayMode newMode, const DateTime &now) {
  int target = lastBrightness;

  clearAll();
  FastLED.show();

  for (int b = 255; b >= 0; b -= 5) {
    FastLED.setBrightness(b);
    FastLED.show();
    delay(8);
  }

  currentMode = newMode;

  if (newMode == MODE_TIME)      drawTime(now);
  else if (newMode == MODE_TEMP) drawTemperature();
  else                           drawDate(now);

  for (int b = 0; b <= target; b += 5) {
    FastLED.setBrightness(b);
    FastLED.show();
    delay(8);
  }
}

// ====================== RESET MYGTUKAS ======================

void handleResetButton() {
  bool state = digitalRead(RESET_BTN_PIN);
  unsigned long nowMs = millis();

  if (state == LOW && btnLastState == HIGH) {
    btnPressStart = nowMs;
    btnHandled = false;
  }
  else if (state == LOW && btnLastState == LOW) {
    unsigned long held = nowMs - btnPressStart;

    if (!btnHandled && held > 10000) {
      btnHandled = true;
      for (int i = 0; i < 10; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Red);
        FastLED.show();
        delay(100);
        clearAll();
        FastLED.show();
        delay(100);
      }
      factoryReset();
      ESP.restart();
    }
  }
  else if (state == HIGH && btnLastState == LOW) {
    unsigned long held = nowMs - btnPressStart;

    if (!btnHandled && held > 3000 && held <= 5000) {
      for (int i = 0; i < 3; i++) {
        fill_solid(leds, NUM_LEDS, CRGB::Green);
        FastLED.show();
        delay(100);
        clearAll();
        FastLED.show();
        delay(100);
      }
      startAPMode();
    }
  }

  btnLastState = state;
}

// ====================== WEB UI PAGALBINIAI ======================

String colorToHex(const CRGB &c) {
  char buf[8];
  sprintf(buf, "#%02X%02X%02X", c.r, c.g, c.b);
  return String(buf);
}

CRGB hexToColor(const String &hex) {
  String h = hex;
  if (h.startsWith("#")) h.remove(0, 1);
  if (h.length() != 6) return CRGB::White;

  long num = strtol(h.c_str(), NULL, 16);
  return CRGB((num >> 16) & 0xFF, (num >> 8) & 0xFF, num & 0xFF);
}

// ====================== LED TEST ANIMACIJOS ======================

void rainbowFill(uint8_t startHue) {
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(startHue + i * 2, 255, 255);
  }
}

void wipeColor(const CRGB &c) {
  clearAll();
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = c;
    FastLED.show();
    delay(5);
  }
}

void sparkle(const CRGB &c) {
  clearAll();
  for (int i = 0; i < 200; i++) {
    int idx = random16(NUM_LEDS);
    leds[idx] = c;
    FastLED.show();
    delay(20);
    leds[idx] = CRGB::Black;
  }
}

void fadeWave(const CRGB &c) {
  clearAll();
  for (int b = 0; b <= 255; b += 5) {
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] = c;
    }
    FastLED.setBrightness(b);
    FastLED.show();
    delay(10);
  }
  for (int b = 255; b >= 0; b -= 5) {
    FastLED.setBrightness(b);
    FastLED.show();
    delay(10);
  }
}

// ====================== LOGIN ======================

void handleLoginPage() {
  if (isAPMode()) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Lauko laikrodis – prisijungimas</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#111;color:#eee;padding:20px;}";
  html += "input{padding:8px;margin:6px 0;width:220px;}";
  html += "button{padding:8px 16px;margin-top:10px;}";
  html += "form{max-width:300px;margin:auto;margin-top:80px;background:#222;padding:20px;border-radius:6px;}";
  html += "h2{text-align:center;}";
  html += "</style>";
  html += "</head><body>";
  html += "<form method='POST' action='/login'>";
  html += "<h2>Lauko laikrodis</h2>";
  html += "<label>Slaptažodis:</label><br>";
  html += "<input type='password' name='password'><br>";
  html += "<button type='submit'>Prisijungti</button>";
  html += "</form>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleLoginPost() {
  if (isAPMode()) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  if (!server.hasArg("password")) {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return;
  }

  String pass = server.arg("password");

  if (pass == String(cfg.webPassword)) {
    sessionActive = true;
    unsigned long nowMs = millis();
    lastRequestAt = nowMs;
    sessionExpiresAt = nowMs + SESSION_TIMEOUT_MS;

    String html = "<!DOCTYPE html><html><head>"
                  "<meta charset='UTF-8'>"
                  "<script>window.location='/'</script>"
                  "</head><body>Jungiamasi...</body></html>";

    server.send(200, "text/html", html);
    return;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                "<title>Neteisingas slaptažodis</title>"
                "<style>body{font-family:Arial;background:#111;color:#eee;text-align:center;padding:40px;}"
                "a{color:#0af;}</style></head><body>"
                "<h2>Neteisingas slaptažodis</h2>"
                "<p><a href='/login'>Bandyti dar kartą</a></p>"
                "</body></html>";

  server.send(200, "text/html", html);
}

void handleLogout() {
  sessionActive = false;
  sessionExpiresAt = 0;
  lastRequestAt = 0;
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

// ====================== HEARTBEAT ======================

void handleHeartbeat() {
  if (!sessionActive || !isSessionValid()) {
    server.send(401, "text/plain", "expired");
    return;
  }
  refreshSession();
  server.send(200, "text/plain", "ok");
}

// ====================== WEB UI – PAGRINDINIS PUSLAPIS ======================

void handleRoot() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  DateTime now;
  getCurrentDateTime(now);

  float correctedTemp = (float)lastTempC + cfg.tempOffset;

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Lauko laikrodis v1.5</title>";

  html += "<style>";
  html += "body{font-family:Arial;background:#111;color:#eee;padding:20px;}";
  html += "table{border-collapse:collapse;margin-top:15px;}";
  html += "td,th{border:1px solid #444;padding:8px 12px;}";
  html += "th{background:#222;}";
  html += "input,select{padding:6px;margin:4px;width:200px;}";
  html += "label{display:block;margin-top:10px;}";
  html += "button{padding:10px 20px;margin-top:15px;}";
  html += ".section{margin-top:25px;padding-top:10px;border-top:1px solid #333;}";
  html += "</style>";

  // v1.5 – heartbeat + AJAX
  html += "<script>"
          "function heartbeat(){"
          "var x=new XMLHttpRequest();"
          "x.open('GET','/heartbeat',true);"
          "x.onreadystatechange=function(){"
          "if(x.readyState==4){"
          "if(x.status==401){alert('Sesija baigėsi. Prisijunkite iš naujo.');window.location='/login';}"
          "}"
          "};"
          "x.send();"
          "}"
          "setInterval(heartbeat,60000);"
          "function previewColor(type,input){"
          "var x=new XMLHttpRequest();"
          "x.open('GET','/previewColor?type='+type+'&hex='+encodeURIComponent(input.value),true);"
          "x.send();"
          "}"
          "</script>";

  html += "</head><body>";

  html += "<h2>Lauko laikrodis v1.5</h2>";
  html += "<p>Prisijungta &nbsp; <a href='/logout' style='color:#0af;'>Atsijungti</a></p>";

  html += "<div class='section'><h3>Realūs duomenys</h3>";
  html += "<table>";
  html += "<tr><th>Parametras</th><th>Reikšmė</th></tr>";

  char buf[32];

  sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  html += String("<tr><td>Laikas</td><td>") + buf + "</td></tr>";

  sprintf(buf, "%04d-%02d-%02d", now.year(), now.month(), now.day());
  html += String("<tr><td>Data</td><td>") + buf + "</td></tr>";

  html += "<tr><td>Galutinė temperatūra</td><td>" + String(correctedTemp, 1) + " °C</td></tr>";
  html += "<tr><td>Tikroji temperatūra (sensorius)</td><td>" + String(lastTempC) + " °C</td></tr>";

  html += "<tr><td>LDR RAW</td><td>" + String(lastLdrRaw) + "</td></tr>";
  html += "<tr><td>LDR Brightness</td><td>" + String(lastBrightness) + "</td></tr>";

  html += String("<tr><td>NTP statusas</td><td>") + (haveNtpTime && wifiConnected ? "Online" : "Offline") + "</td></tr>";

  bool lostPower = rtc.lostPower();
  html += String("<tr><td>RTC baterija</td><td>") + (lostPower ? "LOW (patikrink bateriją)" : "OK") + "</td></tr>";

  String flags = "";
  flags += cfg.showTime ? "Laikas: ON<br>" : "Laikas: OFF<br>";
  flags += cfg.showTemp ? "Temperatūra: ON<br>" : "Temperatūra: OFF<br>";
  flags += cfg.showDate ? "Data: ON<br>" : "Data: OFF<br>";
  flags += cfg.useFade  ? "Fade: ON<br>" : "Fade: OFF<br>";
  flags += cfg.useDST   ? "DST: ON<br>" : "DST: OFF<br>";
  flags += cfg.nightModeEnabled ? "Night mode: ON<br>" : "Night mode: OFF<br>";
  flags += cfg.autoBrightness ? "Auto brightness: ON<br>" : "Auto brightness: OFF<br>";
  flags += cfg.tempColorMode ? "Temp colors: ON<br>" : "Temp colors: OFF<br>";

  html += "<tr><td>Įjungtos funkcijos</td><td>" + flags + "</td></tr>";

  html += "</table></div>";

  html += "<div class='section'><h3>Temperatūros grafikas (paskutinių 24h)</h3>";
  html += "<svg width='300' height='100' style='background:#000;border:1px solid #444;'>";

  int16_t minT =  10000;
  int16_t maxT = -10000;

  for (int i = 0; i < TEMP_HISTORY_SIZE; i++) {
    int16_t v = tempHistory[i];
    if (v < minT) minT = v;
    if (v > maxT) maxT = v;
  }
  if (minT == 10000) { minT = 0; maxT = 0; }

  int range = maxT - minT;
  if (range == 0) range = 1;

  html += "<polyline fill='none' stroke='#00FFFF' stroke-width='2' points='";

  for (int i = 0; i < TEMP_HISTORY_SIZE; i++) {
    int idx = (tempHistoryIndex + i) % TEMP_HISTORY_SIZE;
    int16_t v = tempHistory[idx];
    float norm = (float)(v - minT) / (float)range;
    int x = (i * 300) / TEMP_HISTORY_SIZE;
    int y = 100 - (int)(norm * 90.0f) - 5;
    html += String(x) + "," + String(y) + " ";
  }

  html += "' /></svg></div>";

  html += "<div class='section'><h3>Nustatymai</h3>";
  html += "<form method='POST' action='/save'>";

  html += "<label>WiFi SSID:</label><input name='ssid' value='" + String(cfg.wifiSsid) + "'>";
  html += "<label>WiFi slaptažodis:</label><input name='pass' value='" + String(cfg.wifiPass) + "'>";

  html += "<label><input type='checkbox' name='showTime' " + String(cfg.showTime ? "checked" : "") + "> Rodyti laiką</label>";
  html += "<label><input type='checkbox' name='showTemp' " + String(cfg.showTemp ? "checked" : "") + "> Rodyti temperatūrą</label>";
  html += "<label><input type='checkbox' name='showDate' " + String(cfg.showDate ? "checked" : "") + "> Rodyti datą</label>";

  html += "<label>Temperatūros rodymo periodas (sek.):</label>";
  html += "<input name='tempPeriod' type='number' value='" + String(cfg.tempPeriodSec) + "'>";

  html += "<label>Temperatūros rodymo trukmė (sek.):</label>";
  html += "<input name='tempDuration' type='number' value='" + String(cfg.tempDurationSec) + "'>";

  html += "<label>Datos rodymo trukmė (sek.):</label>";
  html += "<input name='dateDuration' type='number' value='" + String(cfg.dateDurationSec) + "'>";

  html += "<h3>Temperatūros korekcija</h3>";
  html += "<label>Offset (°C):</label>";
  html += "<input name='tempOffset' type='text' value='" + String(cfg.tempOffset, 2) + "'>";

  html += "<h3>Fade ir DST</h3>";
  html += "<label><input type='checkbox' name='useFade' " + String(cfg.useFade ? "checked" : "") + "> Fade perėjimai</label>";
  html += "<label><input type='checkbox' name='useDST' " + String(cfg.useDST ? "checked" : "") + "> Vasaros laikas (DST)</label>";

  html += "<h3>Naktinis režimas</h3>";
  html += "<label><input type='checkbox' name='nightMode' " + String(cfg.nightModeEnabled ? "checked" : "") + "> Naktinis režimas</label>";
  html += "<label>Nakties pradžia (val:min):</label>";
  html += "<input name='nightStartH' type='number' min='0' max='23' value='" + String(cfg.nightStartHour) + "'>";
  html += "<input name='nightStartM' type='number' min='0' max='59' value='" + String(cfg.nightStartMinute) + "'>";
  html += "<label>Nakties pabaiga (val:min):</label>";
  html += "<input name='nightEndH' type='number' min='0' max='23' value='" + String(cfg.nightEndHour) + "'>";
  html += "<input name='nightEndM' type='number' min='0' max='59' value='" + String(cfg.nightEndMinute) + "'>";

  html += "<h3>Ryškumas</h3>";
  html += "<label><input type='checkbox' name='autoBrightness' " + String(cfg.autoBrightness ? "checked" : "") + "> Auto brightness (LDR)</label>";
  html += "<label>Manual brightness (1–255, kai auto OFF):</label>";
  html += "<input name='manualBrightness' type='number' min='1' max='255' value='" + String(cfg.manualBrightness) + "'>";

  html += "<h3>Laiko zona</h3>";
  html += "<label>Laiko zona (GMT offset valandomis):</label>";
  html += "<input name='timezoneOffset' type='number' min='-12' max='14' value='" + String(cfg.timezoneOffset) + "'>";

  html += "<h3>Spalvos pagal temperatūrą</h3>";
  html += "<label><input type='checkbox' name='tempColorMode' " + String(cfg.tempColorMode ? "checked" : "") + "> Įjungti spalvas pagal temperatūrą</label>";
  html += "<label><input type='checkbox' name='tempColorAdvanced' " + String(cfg.tempColorAdvancedEnabled ? "checked" : "") + "> Naudoti pažangią spalvų sistemą</label>";
  html += "<label>Perėjimo tipas:</label>";
  html += "<select name='tempColorSmooth'>";
  html += String("<option value='0' ") + (cfg.tempColorSmooth ? "" : "selected") + ">Zoninis</option>";
  html += String("<option value='1' ") + (cfg.tempColorSmooth ? "selected" : "") + ">Sklandus</option>";
  html += "</select>";

  html += "<label>Temperatūros žingsnis (°C):</label>";
  html += "<select name='tempColorStep'>";
  html += String("<option value='1' ")  + (cfg.tempColorStep==1  ? "selected" : "") + ">1</option>";
  html += String("<option value='2' ")  + (cfg.tempColorStep==2  ? "selected" : "") + ">2</option>";
  html += String("<option value='5' ")  + (cfg.tempColorStep==5  ? "selected" : "") + ">5</option>";
  html += String("<option value='10' ") + (cfg.tempColorStep==10 ? "selected" : "") + ">10</option>";
  html += "</select>";

  html += "<h4>Temperatūros zonos</h4>";
  html += "<table><tr><th>Nuo (°C)</th><th>Iki (°C)</th><th>Spalva</th></tr>";
  for (uint8_t i = 0; i < TEMP_COLOR_ZONES; i++) {
    TempColorZone &z = cfg.tempZones[i];
    html += "<tr>";
    html += "<td><input name='z_from_" + String(i) + "' type='number' value='" + String(z.fromC) + "'></td>";
    html += "<td><input name='z_to_"   + String(i) + "' type='number' value='" + String(z.toC)   + "'></td>";
    CRGB cz(z.r, z.g, z.b);
    html += "<td><input name='z_color_" + String(i) + "' type='color' value='" + colorToHex(cz) + "'></td>";
    html += "</tr>";
  }
  html += "</table>";

  html += "<h3>Spalvos (kai tempColorMode OFF)</h3>";
  html += "<label>Laiko spalva:</label><input type='color' name='colorTime' value='" + colorToHex(colorTime) + "' onchange=\"previewColor('time',this)\">";
  html += "<label>Temperatūros spalva:</label><input type='color' name='colorTemp' value='" + colorToHex(colorTemp) + "' onchange=\"previewColor('temp',this)\">";
  html += "<label>Datos spalva:</label><input type='color' name='colorDate' value='" + colorToHex(colorDate) + "' onchange=\"previewColor('date',this)\">";

  html += "<h3>Web slaptažodis</h3>";
  html += "<label>Naujas slaptažodis (palik tuščią, jei nekeiti):</label>";
  html += "<input name='webPassword' type='password' value=''>";

  html += "<button type='submit'>Išsaugoti</button>";
  html += "</form></div>";

  html += "<div class='section'><h3>Valdymas</h3>";
  html += "<form action='/ledtest'><button>LED test</button></form>";
  html += "<form action='/ledtest2'><button>LED test 2 (animacijos)</button></form>";
  html += "<form action='/testnight'><button>Testuoti naktinį režimą (10s)</button></form>";
  html += "<form action='/reboot'><button>Perkrauti</button></form>";
  html += "<form action='/factory'><button>Factory reset</button></form>";
  html += "</div>";

  html += "<div class='section'><h3>Firmware atnaujinimas (Web OTA)</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware'><br>";
  html += "<button type='submit'>Įkelti naują firmware</button>";
  html += "</form>";
  html += "<p>Sesijos galiojimo laikas: 30 min nuo paskutinio veiksmo. Jei sesija baigsis – būsite automatiškai nukreiptas į prisijungimo langą.</p>";
  html += "</div>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ====================== NUSTATYMŲ IŠSAUGOJIMAS ======================

void handleSave() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  if (server.hasArg("ssid")) strncpy(cfg.wifiSsid, server.arg("ssid").c_str(), sizeof(cfg.wifiSsid));
  if (server.hasArg("pass")) strncpy(cfg.wifiPass, server.arg("pass").c_str(), sizeof(cfg.wifiPass));

  cfg.showTime = server.hasArg("showTime");
  cfg.showTemp = server.hasArg("showTemp");
  cfg.showDate = server.hasArg("showDate");

  if (server.hasArg("tempPeriod"))   cfg.tempPeriodSec   = server.arg("tempPeriod").toInt();
  if (server.hasArg("tempDuration")) cfg.tempDurationSec = server.arg("tempDuration").toInt();
  if (server.hasArg("dateDuration")) cfg.dateDurationSec = server.arg("dateDuration").toInt();

  if (server.hasArg("tempOffset"))   cfg.tempOffset = server.arg("tempOffset").toFloat();

  cfg.useFade = server.hasArg("useFade");
  cfg.useDST  = server.hasArg("useDST");

  cfg.nightModeEnabled = server.hasArg("nightMode");
  if (server.hasArg("nightStartH")) cfg.nightStartHour   = server.arg("nightStartH").toInt();
  if (server.hasArg("nightStartM")) cfg.nightStartMinute = server.arg("nightStartM").toInt();
  if (server.hasArg("nightEndH"))   cfg.nightEndHour     = server.arg("nightEndH").toInt();
  if (server.hasArg("nightEndM"))   cfg.nightEndMinute   = server.arg("nightEndM").toInt();

  cfg.autoBrightness = server.hasArg("autoBrightness");
  if (server.hasArg("manualBrightness")) {
    int mb = server.arg("manualBrightness").toInt();
    if (mb < 1) mb = 1;
    if (mb > 255) mb = 255;
    cfg.manualBrightness = (uint8_t)mb;
  }

  if (server.hasArg("timezoneOffset")) {
    int tz = server.arg("timezoneOffset").toInt();
    if (tz < -12) tz = -12;
    if (tz > 14)  tz = 14;
    cfg.timezoneOffset = (int8_t)tz;
  }

  cfg.tempColorMode = server.hasArg("tempColorMode");
  cfg.tempColorAdvancedEnabled = server.hasArg("tempColorAdvanced");

  if (server.hasArg("tempColorSmooth")) {
    cfg.tempColorSmooth = (server.arg("tempColorSmooth").toInt() == 1);
  }

  if (server.hasArg("tempColorStep")) {
    int st = server.arg("tempColorStep").toInt();
    if (st == 1 || st == 2 || st == 5 || st == 10) cfg.tempColorStep = st;
  }

  // zonos
  for (uint8_t i = 0; i < TEMP_COLOR_ZONES; i++) {
    String sf = "z_from_" + String(i);
    String st = "z_to_"   + String(i);
    String sc = "z_color_" + String(i);

    if (server.hasArg(sf)) cfg.tempZones[i].fromC = (int8_t)server.arg(sf).toInt();
    if (server.hasArg(st)) cfg.tempZones[i].toC   = (int8_t)server.arg(st).toInt();
    if (server.hasArg(sc)) {
      CRGB c = hexToColor(server.arg(sc));
      cfg.tempZones[i].r = c.r;
      cfg.tempZones[i].g = c.g;
      cfg.tempZones[i].b = c.b;
    }
  }

  if (!cfg.tempColorMode) {
    if (server.hasArg("colorTime")) colorTime = hexToColor(server.arg("colorTime"));
    if (server.hasArg("colorTemp")) colorTemp = hexToColor(server.arg("colorTemp"));
    if (server.hasArg("colorDate")) colorDate = hexToColor(server.arg("colorDate"));
  }

  if (server.hasArg("webPassword")) {
    String np = server.arg("webPassword");
    np.trim();
    if (np.length() > 0 && np.length() < (int)sizeof(cfg.webPassword)) {
      strncpy(cfg.webPassword, np.c_str(), sizeof(cfg.webPassword));
      cfg.webPassword[sizeof(cfg.webPassword)-1] = '\0';
    }
  }

  saveConfigToEEPROM();

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

// ====================== LED TEST / REBOOT / FACTORY / TEST NIGHT / PREVIEW ======================

void handleLedTest() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  fill_solid(leds, NUM_LEDS, CRGB::Red);
  FastLED.show();
  delay(400);

  fill_solid(leds, NUM_LEDS, CRGB::Green);
  FastLED.show();
  delay(400);

  fill_solid(leds, NUM_LEDS, CRGB::Blue);
  FastLED.show();
  delay(400);

  clearAll();
  FastLED.show();

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleLedTest2() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  for (uint8_t h = 0; h < 255; h += 3) {
    rainbowFill(h);
    FastLED.show();
    delay(20);
  }

  wipeColor(CRGB::Red);
  wipeColor(CRGB::Green);
  wipeColor(CRGB::Blue);

  sparkle(CRGB::White);

  fadeWave(CRGB::Orange);

  clearAll();
  FastLED.show();

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleReboot() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  server.send(200, "text/plain", "Perkraunama...");
  delay(300);
  ESP.restart();
}

void handleFactory() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  factoryReset();
  server.send(200, "text/plain", "Factory reset atliktas. Perkraunama...");
  delay(300);
  ESP.restart();
}

void handleTestNight() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  forceNightMode = true;
  forceNightModeUntil = millis() + 10000UL;
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handlePreviewColor() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  if (!server.hasArg("type") || !server.hasArg("hex")) {
    server.send(400, "text/plain", "Missing args");
    return;
  }
  String type = server.arg("type");
  CRGB c = hexToColor(server.arg("hex"));

  if (type == "time")      colorTime = c;
  else if (type == "temp") colorTemp = c;
  else if (type == "date") colorDate = c;

  DateTime now;
  getCurrentDateTime(now);
  if (currentMode == MODE_TIME)      drawTime(now);
  else if (currentMode == MODE_TEMP) drawTemperature();
  else                               drawDate(now);
  FastLED.show();

  server.send(200, "text/plain", "OK");
}

// ====================== WEB OTA HANDLER ======================

void handleFirmwareUpload() {
  if (!checkAuth()) {
    redirectToLogin();
    return;
  }

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    WiFiUDP::stopAll();
    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "Update OK");
      delay(500);
      ESP.restart();
    } else {
      Update.printError(Serial);
      server.send(500, "text/plain", "Update failed");
    }
  }
}

// ====================== SETUP ======================

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  loadConfigFromEEPROM();

  FastLED.addLeds<WS2812B, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(80);

  sensors.begin();
  rtc.begin();

  connectWiFi();
  setupTimeNTP();

  if (wifiConnected) {
    ArduinoOTA.setHostname("LaukoLaikrodis");
    ArduinoOTA.onStart([]() {
      WiFiUDP::stopAll();
    });
    ArduinoOTA.begin();
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ledtest", handleLedTest);
  server.on("/ledtest2", handleLedTest2);
  server.on("/reboot", handleReboot);
  server.on("/factory", handleFactory);
  server.on("/testnight", handleTestNight);
  server.on("/previewColor", handlePreviewColor);

  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);

  server.on("/heartbeat", HTTP_GET, handleHeartbeat);

  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", "Update started");
  }, handleFirmwareUpload);

  server.begin();

  modeStartMillis = millis();
  lastTempCycleMillis = millis();

  for (int i = 0; i < TEMP_HISTORY_SIZE; i++) {
    tempHistory[i] = 0;
  }
}

// ====================== LOOP ======================

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleResetButton();

  if (wifiConnected) {
    ArduinoOTA.handle();
  }

  updateBrightness();
  updateTempColorsIfNeeded();
  updateTempHistory();

  DateTime now;
  if (!getCurrentDateTime(now)) return;

  unsigned long nowMs = millis();

  if (currentMode == MODE_TIME) {

    if (!cfg.showTemp && cfg.showDate &&
        nowMs - lastTempCycleMillis >= (unsigned long)cfg.tempPeriodSec * 1000UL) {

      if (cfg.useFade) fadeToMode(MODE_DATE, now);
      else currentMode = MODE_DATE;

      modeStartMillis = nowMs;
      lastTempCycleMillis = nowMs;
    }
    else if (cfg.showTemp &&
             nowMs - lastTempCycleMillis >= (unsigned long)cfg.tempPeriodSec * 1000UL) {

      updateTemperature();

      if (cfg.useFade) fadeToMode(MODE_TEMP, now);
      else currentMode = MODE_TEMP;

      modeStartMillis = nowMs;
      lastTempCycleMillis = nowMs;
    }
  }
  else if (currentMode == MODE_TEMP) {
    if (nowMs - modeStartMillis >= (unsigned long)cfg.tempDurationSec * 1000UL) {
      if (cfg.showDate) {
        if (cfg.useFade) fadeToMode(MODE_DATE, now);
        else currentMode = MODE_DATE;
      } else {
        if (cfg.useFade) fadeToMode(MODE_TIME, now);
        else currentMode = MODE_TIME;
      }
      modeStartMillis = nowMs;
    }
  }
  else if (currentMode == MODE_DATE) {
    if (nowMs - modeStartMillis >= (unsigned long)cfg.dateDurationSec * 1000UL) {
      if (cfg.useFade) fadeToMode(MODE_TIME, now);
      else currentMode = MODE_TIME;
      modeStartMillis = nowMs;
    }
  }

  if (currentMode == MODE_TIME) {
    drawTime(now);
  }
  else if (!cfg.useFade) {
    if (currentMode == MODE_TEMP)      drawTemperature();
    else if (currentMode == MODE_DATE) drawDate(now);
  }

  FastLED.show();
}
