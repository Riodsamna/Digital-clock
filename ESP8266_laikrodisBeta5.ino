// =======================================================
// Lauko laikrodis v1.1
// - Laikas + temperatūra + data
// - Web UI (WiFi/AP režimas)
// - Fade perėjimai (konfigūruojami)
// - LDR ryškumas (smoothing)
// - DS18B20 temperatūra su kalibravimo offset (float)
// - DS3231 + NTP + DST
// - LED test / reboot / factory reset (mygtukas + web)
// - Pataisytas: sekundžių taškai mirksi ir su fade
// - Pataisytas: išjungus TEMP, DATA vis tiek rodoma
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

// ====================== PINAI IR KONFIGAS ======================

// LED juosta
#define DATA_PIN       D4
#define NUM_LEDS       118
#define COLOR_ORDER    GRB   // jei spalvos maišosi, pabandyk BRG arba RGB

// DS18B20
#define ONE_WIRE_BUS   D5

// DS3231 I2C – pagal NodeMCU standartą
// SDA – D2, SCL – D1

// LDR
#define LDR_PIN        A0

// Reset/Setup mygtukas
#define RESET_BTN_PIN  D6

// DNS serveris AP režimui
#define DNS_PORT       53

// AP režimo duomenys
const char* AP_SSID     = "Laikrodis-Setup";
const char* AP_PASSWORD = "Laikrodis2026";

// NTP
const long GMT_OFFSET_SEC = 2 * 3600; // Lietuva – UTC+2 žiemos laikas
const int  DST_OFFSET_SEC = 0;        // tikras DST offset koreguosim patys

// EEPROM
#define EEPROM_SIZE    1024
#define EEPROM_MAGIC   0x42

// ====================== BIBLIOTEKOS ======================

CRGB leds[NUM_LEDS];

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
RTC_DS3231 rtc;

ESP8266WebServer server(80);
DNSServer dnsServer;

// ====================== SKAITMENŲ MASYVAS ======================
// digits[skaitmuo][28] – palikta iš tavo kodo

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
  {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},  // *0
  {0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0}   // C
};

// ====================== KONFIGŪRACIJA ======================

struct StoredConfig {
  uint8_t magic;         // EEPROM_MAGIC
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

  float tempOffset;      // temperatūros korekcija (°C), pvz. -2.0, 0.0, +1.5
};

StoredConfig cfg;

// runtime struktūros
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
unsigned long lastReconnectAttempt = 0; // WiFi reconnect watchdog

bool haveNtpTime = false;
bool wifiConnected = false;

int lastTempC = 0;
bool tempInitialized = false;

int lastLdrRaw = 0;
int lastBrightness = 80;

// reset mygtuko būsena
bool btnLastState = HIGH;
unsigned long btnPressStart = 0;
bool btnHandled = false;

// ====================== PAGALBOS FUNKCIJOS ======================

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

  cfg.colorTimeR = 153; cfg.colorTimeG = 50;  cfg.colorTimeB = 204; // violetish
  cfg.colorTempR = 0;   cfg.colorTempG = 255; cfg.colorTempB = 255; // aqua
  cfg.colorDateR = 0;   cfg.colorDateG = 255; cfg.colorDateB = 0;   // green

  cfg.tempOffset = 0.0f; // kaip sutarėm – default 0, kalibruosi pats
}

void loadConfigFromEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != EEPROM_MAGIC) {
    loadDefaults();
    EEPROM.put(0, cfg);
    EEPROM.commit();
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

// ====================== WI-FI IR AP ======================

void startAPMode() {
  Serial.println("Paleidžiamas AP režimas...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(500);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP: ");
  Serial.println(myIP);

  dnsServer.start(DNS_PORT, "*", myIP);
  wifiConnected = false;
}

void connectWiFi() {
  if (strlen(cfg.wifiSsid) == 0) {
    Serial.println("WiFi SSID nesukonfigūruotas – iškart AP režimas.");
    startAPMode();
    return;
  }

  Serial.print("Jungiamasi prie WiFi: ");
  Serial.println(cfg.wifiSsid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print("Prisijungta. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi prisijungti nepavyko – AP režimas.");
    startAPMode();
  }
}

// ====================== NTP, DS3231, DST ======================

bool isDST_EU(const DateTime& dt) {
  int year = dt.year();

  DateTime lastMar(year, 3, 31, 1, 0, 0);
  while (lastMar.dayOfTheWeek() != 0) {
    lastMar = lastMar - TimeSpan(1, 0, 0, 0);
  }

  DateTime lastOct(year, 10, 31, 1, 0, 0);
  while (lastOct.dayOfTheWeek() != 0) {
    lastOct = lastOct - TimeSpan(1,0,0,0);
  }

  return (dt >= lastMar && dt < lastOct);
}

void setupTimeNTP() {
  if (!wifiConnected) {
    Serial.println("NTP negalimas – nėra WiFi.");
    haveNtpTime = false;
    return;
  }

  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  Serial.println("Laukiama NTP laiko...");

  struct tm timeinfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeinfo)) {
      haveNtpTime = true;
      Serial.println("NTP laikas gautas.");

      DateTime dt(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );
      rtc.begin();
      rtc.adjust(dt);
      return;
    }
    delay(500);
  }

  Serial.println("NTP gauti nepavyko, naudosim DS3231.");
  haveNtpTime = false;
}

bool getCurrentDateTime(DateTime &out) {
  if (haveNtpTime && wifiConnected) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      out = DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );
    } else {
      haveNtpTime = false;
      if (!rtc.begin()) return false;
      out = rtc.now();
    }
  } else {
    if (!rtc.begin()) return false;
    out = rtc.now();
  }

  if (cfg.useDST) {
    if (isDST_EU(out)) {
      out = out + TimeSpan(0, 1, 0, 0);
    }
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
    // be smoothing – kaip prašei, gryna reikšmė
    lastTempC = newTemp;
  }
}

// ====================== RYŠKUMAS ======================

void updateBrightness() {
  lastLdrRaw = analogRead(LDR_PIN); // 0–1023
  int mapped = map(lastLdrRaw, 0, 1023, 10, 255);
  mapped = constrain(mapped, 10, 255);

  int smooth = (lastBrightness * 7 + mapped) / 8; // LDR smoothing
  lastBrightness = smooth;
  FastLED.setBrightness(smooth);
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
  if (dotOn) {
    leds[60] = colorTime;
    leds[61] = colorTime;
  } else {
    leds[60] = CRGB::Black;
    leds[61] = CRGB::Black;
  }

  for (int i = 0; i < 4; i++) leds[i] = CRGB::Black;

  int full = hour * 100 + minute;
  int cursor;
  int digit;

  for (int i = 1; i <= 4; i++) {
    digit = full % 10;
    if (i == 1) cursor = 90;
    else if (i == 2) cursor = 62;
    else if (i == 3) cursor = 32;
    else cursor = 4;

    for (int k = 0; k <= 27; k++) {
      if (digits[digit][k] == 1) leds[cursor] = colorTime;
      else leds[cursor] = CRGB::Black;
      cursor++;
    }
    full /= 10;
  }
}

void drawTemperature() {
  clearAll();
  leds[60] = CRGB::Black;
  leds[61] = CRGB::Black;

  // pritaikom kalibravimo offset
  float corrected = (float)lastTempC + cfg.tempOffset;
  int celsius = (int)round(corrected);

  int cursor;

  // 4 skaitmuo – "C"
  cursor = 90;
  for (int k = 0; k <= 27; k++) {
    if (digits[11][k] == 1) leds[cursor] = colorTemp;
    else leds[cursor] = CRGB::Black;
    cursor++;
  }

  // 3 skaitmuo – "*0"
  cursor = 62;
  for (int k = 0; k <= 27; k++) {
    if (digits[10][k] == 1) leds[cursor] = colorTemp;
    else leds[cursor] = CRGB::Black;
    cursor++;
  }

  // 2 skaitmuo – vienetai
  int digit = abs(celsius % 10);
  cursor = 32;
  for (int k = 0; k <= 27; k++) {
    if (digits[digit][k] == 1) leds[cursor] = colorTemp;
    else leds[cursor] = CRGB::Black;
    cursor++;
  }

  // 1 skaitmuo – dešimtys
  digit = abs(celsius / 10);
  cursor = 4;
  for (int k = 0; k <= 27; k++) {
    if (digits[digit][k] == 1) leds[cursor] = colorTemp;
    else leds[cursor] = CRGB::Black;
    cursor++;
  }

  // minusas – LED 0–3, jei temp < 0
  for (int k = 0; k < 4; k++) {
    if (celsius < 0) leds[k] = colorTemp;
    else leds[k] = CRGB::Black;
  }
}

void drawDate(const DateTime &now) {
  clearAll();
  leds[60] = CRGB::Black;
  leds[61] = CRGB::Black;

  int month = now.month();
  int day   = now.day();
  int value = month * 100 + day;

  for (int i = 0; i < 4; i++) leds[i] = CRGB::Black;

  int cursor;
  int digit;

  for (int i = 1; i <= 4; i++) {
    digit = value % 10;
    if (i == 1) cursor = 90;
    else if (i == 2) cursor = 62;
    else if (i == 3) cursor = 32;
    else cursor = 4;

    for (int k = 0; k <= 27; k++) {
      if (digits[digit][k] == 1) leds[cursor] = colorDate;
      else leds[cursor] = CRGB::Black;
      cursor++;
    }
    value /= 10;
  }
}

// ====================== FADE PERĖJIMAI ======================

void fadeToMode(DisplayMode newMode, const DateTime &now) {
  for (int b = lastBrightness; b >= 10; b -= 15) {
    FastLED.setBrightness(b);
    FastLED.show();
    delay(10);
  }

  currentMode = newMode;

  switch (currentMode) {
    case MODE_TIME:
      drawTime(now);
      break;
    case MODE_TEMP:
      drawTemperature();
      break;
    case MODE_DATE:
      drawDate(now);
      break;
  }

  for (int b = 10; b <= lastBrightness; b += 15) {
    FastLED.setBrightness(b);
    FastLED.show();
    delay(10);
  }
}

// ====================== RESET MYGTUKAS ======================

void handleResetButton() {
  bool state = digitalRead(RESET_BTN_PIN); // INPUT_PULLUP: LOW = spaustas
  unsigned long nowMs = millis();

  if (state == LOW && btnLastState == HIGH) {
    btnPressStart = nowMs;
    btnHandled = false;
  } else if (state == LOW && btnLastState == LOW) {
    unsigned long held = nowMs - btnPressStart;

    if (!btnHandled && held > 10000) {
      btnHandled = true;
      Serial.println("Factory RESET per mygtuką!");
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

  } else if (state == HIGH && btnLastState == LOW) {
    unsigned long held = nowMs - btnPressStart;
    if (!btnHandled) {
      if (held > 3000 && held <= 5000) {
        Serial.println("Mygtukas: AP setup režimas.");
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
  long number = strtol(h.c_str(), NULL, 16);
  uint8_t r = (number >> 16) & 0xFF;
  uint8_t g = (number >> 8)  & 0xFF;
  uint8_t b = number & 0xFF;
  return CRGB(r, g, b);
}

String modeToString() {
  switch (currentMode) {
    case MODE_TIME: return "Laikas";
    case MODE_TEMP: return "Temperatūra";
    case MODE_DATE: return "Data";
  }
  return "";
}
// ====================== WEB UI – PAGRINDINIS PUSLAPIS ======================

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Lauko laikrodis</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#111;color:#eee;padding:20px;}";
  html += "input,select{padding:6px;margin:4px;width:200px;}";
  html += "label{display:block;margin-top:10px;}";
  html += "button{padding:10px 20px;margin-top:15px;}";
  html += "</style></head><body>";

  html += "<h2>Lauko laikrodis v1.1</h2>";
  html += "<p>Dabartinis režimas: <b>" + modeToString() + "</b></p>";

  html += "<form method='POST' action='/save'>";

  // WiFi
  html += "<h3>WiFi nustatymai</h3>";
  html += "<label>SSID:</label><input name='ssid' value='" + String(cfg.wifiSsid) + "'>";
  html += "<label>Slaptažodis:</label><input name='pass' value='" + String(cfg.wifiPass) + "'>";

  // Rodymo nustatymai
  html += "<h3>Rodymo nustatymai</h3>";
  html += "<label><input type='checkbox' name='showTime' " + String(cfg.showTime ? "checked" : "") + "> Rodyti laiką</label>";
  html += "<label><input type='checkbox' name='showTemp' " + String(cfg.showTemp ? "checked" : "") + "> Rodyti temperatūrą</label>";
  html += "<label><input type='checkbox' name='showDate' " + String(cfg.showDate ? "checked" : "") + "> Rodyti datą</label>";

  html += "<label>Temperatūros rodymo periodas (sek.):</label>";
  html += "<input name='tempPeriod' type='number' value='" + String(cfg.tempPeriodSec) + "'>";

  html += "<label>Temperatūros rodymo trukmė (sek.):</label>";
  html += "<input name='tempDuration' type='number' value='" + String(cfg.tempDurationSec) + "'>";

  html += "<label>Datos rodymo trukmė (sek.):</label>";
  html += "<input name='dateDuration' type='number' value='" + String(cfg.dateDurationSec) + "'>";

  // Temperatūros korekcija
  html += "<h3>Temperatūros korekcija</h3>";
  html += "<label>Temperatūros korekcija (°C):</label>";
  html += "<input name='tempOffset' type='text' value='" + String(cfg.tempOffset, 2) + "'>";

  // Fade
  html += "<h3>Papildomi nustatymai</h3>";
  html += "<label><input type='checkbox' name='useFade' " + String(cfg.useFade ? "checked" : "") + "> Naudoti fade perėjimus</label>";
  html += "<label><input type='checkbox' name='useDST' " + String(cfg.useDST ? "checked" : "") + "> Vasaros laikas (DST)</label>";

  // Spalvos
  html += "<h3>Spalvos</h3>";
  html += "<label>Laiko spalva:</label><input type='color' name='colorTime' value='" + colorToHex(colorTime) + "'>";
  html += "<label>Temperatūros spalva:</label><input type='color' name='colorTemp' value='" + colorToHex(colorTemp) + "'>";
  html += "<label>Datos spalva:</label><input type='color' name='colorDate' value='" + colorToHex(colorDate) + "'>";

  html += "<button type='submit'>Išsaugoti</button>";
  html += "</form>";

  html += "<hr>";
  html += "<form action='/ledtest'><button>LED test</button></form>";
  html += "<form action='/reboot'><button>Perkrauti</button></form>";
  html += "<form action='/factory'><button>Factory reset</button></form>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ====================== WEB – IŠSAUGOJIMAS ======================

void handleSave() {
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

  if (server.hasArg("colorTime")) colorTime = hexToColor(server.arg("colorTime"));
  if (server.hasArg("colorTemp")) colorTemp = hexToColor(server.arg("colorTemp"));
  if (server.hasArg("colorDate")) colorDate = hexToColor(server.arg("colorDate"));

  saveConfigToEEPROM();

  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

// ====================== LED TEST / REBOOT / FACTORY ======================

void handleLedTest() {
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Red;
  FastLED.show();
  delay(500);
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Green;
  FastLED.show();
  delay(500);
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Blue;
  FastLED.show();
  delay(500);
  clearAll();
  FastLED.show();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleReboot() {
  server.send(200, "text/plain", "Perkraunama...");
  delay(500);
  ESP.restart();
}

void handleFactory() {
  factoryReset();
  server.send(200, "text/plain", "Factory reset atliktas. Perkraunama...");
  delay(500);
  ESP.restart();
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

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ledtest", handleLedTest);
  server.on("/reboot", handleReboot);
  server.on("/factory", handleFactory);

  server.begin();
  Serial.println("Web serveris paleistas.");

  modeStartMillis = millis();
  lastTempCycleMillis = millis();
}

// ====================== LOOP ======================

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleResetButton();

  updateBrightness();

  DateTime now;
  if (!getCurrentDateTime(now)) return;

  unsigned long nowMs = millis();

  // ======================
  // PATAISYTA LOGIKA:
  // TIME → TEMP → DATE
  // Jei TEMP išjungta → TIME → DATE
  // ======================

  if (currentMode == MODE_TIME) {

    // TEMP išjungta → eiti tiesiai į DATE
    if (!cfg.showTemp && cfg.showDate &&
        nowMs - lastTempCycleMillis >= (unsigned long)cfg.tempPeriodSec * 1000UL) {

      if (cfg.useFade) fadeToMode(MODE_DATE, now);
      else currentMode = MODE_DATE;

      modeStartMillis = nowMs;
      lastTempCycleMillis = nowMs;
    }

    // TEMP įjungta → normalus ciklas
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

  // ======================
  // Sekundžių taškai turi mirksėti VISADA
  // ======================
  if (currentMode == MODE_TIME) {
    drawTime(now);
  }
  else if (!cfg.useFade) {
    if (currentMode == MODE_TEMP) drawTemperature();
    else if (currentMode == MODE_DATE) drawDate(now);
  }

  FastLED.show();
}
