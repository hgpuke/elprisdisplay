#include <SPI.h>
#include <Wire.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>
#include <GxEPD2_BW.h>

#include "PriceSources.h"
#include "EpSettings.h"
#include "EpConfigBle.h"
#include "EpQrData.h"

#if __has_include("BarlowFonts.h")
  #include "BarlowFonts.h"
#else
  #error "BarlowFonts.h saknas. Kör: python3 tools/prepare_barlow_fonts.py"
#endif

// ============================================================================
// Elprisdisplay 2.0 – v0.1.1
//
// Stomme från den senast verifierade v0.9:
// - PCF85063 RTC i UTC
// - kvartssynkad deep sleep
// - wake vid nästa kvart + 2 s
// - cached Wi-Fi channel/BSSID
// - NTP -> RTC endast när Wi-Fi ändå används
// - function-button emergency stay-awake
// - AXP2101/SHTC3 + valfri MQTT-telemetri
// - partial refresh vid normala timer-wakes
//
// Nytt i 2.0:
// - ingen Home Assistant krävs för displayens prisdata
// - publicerade priser: Elpriset just nu
// - prognos: Wolta
// - 72 timmar, timmedel i grafen
// - publicerat pris skriver över prognos per timme
// - ny 800x480-layout
// - Barlow / Barlow Condensed
// ============================================================================

// ============================================================================
// HÅRDVARA
// ============================================================================
constexpr int I2C_SDA = 41;
constexpr int I2C_SCL = 42;

constexpr uint8_t RTC_ADDR     = 0x51;
constexpr uint8_t RTC_TIME_REG = 0x04;

constexpr uint8_t SHTC3_ADDR = 0x70;
constexpr uint8_t AXP2101_ADDR = 0x34;

constexpr int FUNCTION_BUTTON = 5;

constexpr int EPD_BUSY = 3;
constexpr int EPD_DC   = 9;
constexpr int EPD_CS   = 10;
constexpr int EPD_SCLK = 11;
constexpr int EPD_MOSI = 12;
constexpr int EPD_RST  = 46;

GxEPD2_BW<GxEPD2_397_GDEM0397T81,
          GxEPD2_397_GDEM0397T81::HEIGHT>
display(GxEPD2_397_GDEM0397T81(
  EPD_CS,
  EPD_DC,
  EPD_RST,
  EPD_BUSY
));

constexpr const char* TZ_SWEDEN = "CET-1CEST,M3.5.0,M10.5.0/3";

// ============================================================================
// TILLFÄLLIG INTEGRATIONSDIAGNOSTIK
//
// false = normal drift. Efter full render vid kallstart går enheten tillbaka
// till kvartssynkad deep sleep. Serial-diagnostiken behålls vid uppvakning.
// ============================================================================
constexpr bool INTEGRATION_DEBUG_HOLD = false;


// ============================================================================
// RTC-MINNE
// ============================================================================
constexpr uint32_t RTC_STATE_MAGIC = 0xE20A2020;

RTC_DATA_ATTR uint32_t rtcStateMagic = 0;

RTC_DATA_ATTR uint8_t lastWifiChannel = 0;
RTC_DATA_ATTR uint8_t lastWifiBssid[6] = {0};

RTC_DATA_ATTR int lastHandledDay = -1;
RTC_DATA_ATTR int lastHandledQuarter = -1;

RTC_DATA_ATTR QuarterDay actualToday;
RTC_DATA_ATTR QuarterDay actualTomorrow;
RTC_DATA_ATTR QuarterDay forecastDays[FORECAST_CACHE_DAYS];

RTC_DATA_ATTR int lastForecastMidnightYday = -1;
RTC_DATA_ATTR int lastForecastAfternoonYday = -1;
RTC_DATA_ATTR int cachedBatteryLevel = -1;

bool emergencyStayAwake = false;
volatile bool ntpSyncCompleted = false;

// ============================================================================
// ARBETSDATA – byggs om billigt efter varje wake
// ============================================================================
HourPrice graphHours[GRAPH_HOURS];
PriceStats priceStats;
float currentPrice = NAN;
bool currentPriceValid = false;

// Must be declared before Arduino's auto-generated function prototypes.
// Keeping this among the global types avoids the same preprocessor issue that
// can otherwise make calculateYScale() appear before YScale is known.
struct YScale {
  float minValue = 0.0f;
  float maxValue = 3.0f;
  float step = 0.5f;
};

// ============================================================================
// TELEMETRI / MQTT
// ============================================================================
struct TelemetryData {
  bool environmentValid = false;
  bool batteryValid = false;
  float temperature = NAN;
  float humidity = NAN;
  float batteryVoltage = NAN;
  int batteryLevel = -1;
};

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);

constexpr const char* MQTT_CLIENT_ID = "epaper-dashboard";
constexpr const char* MQTT_STATE_TOPIC = "epaper-dashboard/state";

// ============================================================================
// LAYOUT – v3-geometrin
// ============================================================================
struct Layout {
  int headerLineY = 42;
  int updatedX = 0;
  int updatedBaselineY = 30;
  int dateX = 255;
  int dateBaselineY = 30;
  int batteryX = 690;
  int batteryY = 13;      // 18 px kropp: nederkant på textens baseline y=30
  int batteryTextRightX = 798;

  int statsTop = 47;
  int statsBottom = 128;
  int sectionLineY = 134;

  int nowX = 0;
  int nowY = 47;
  int nowW = 135;
  int nowH = 81;

  int dayTitleBaselineY = 166;
  int dayDateBaselineY = 188;

  int chartX = 70;
  int chartY = 203;
  int chartW = 729;
  int chartH = 240;

  int xLabelBaselineY = 477;
};

constexpr Layout L;

// ============================================================================
// BARLOW-FONTER – genereras av tools/prepare_barlow_fonts.py
// ============================================================================
#define FONT_HEADER      BarlowSemiBold24
#define FONT_STAT_LABEL  BarlowBold20
#define FONT_STAT_VALUE  BarlowSemiBold40
#define FONT_STAT_SMALL  BarlowSemiBold18
#define FONT_DAY         BarlowSemiBold24
#define FONT_DAY_DATE    BarlowSemiBold20
#define FONT_Y_AXIS      BarlowSemiBold22
#define FONT_UNIT        BarlowSemiBold24
#define FONT_X_AXIS      BarlowCondensedSemiBold22
#define FONT_NOW_TAG     BarlowBold16

const char* WEEKDAY_SHORT[] = {
  "Sön", "Mån", "Tis", "Ons", "Tor", "Fre", "Lör"
};

const char* WEEKDAY_LONG_UPPER[] = {
  "SÖNDAG", "MÅNDAG", "TISDAG", "ONSDAG",
  "TORSDAG", "FREDAG", "LÖRDAG"
};

const char* MONTH_SHORT[] = {
  "jan", "feb", "mar", "apr", "maj", "jun",
  "jul", "aug", "sep", "okt", "nov", "dec"
};

// ============================================================================
// UTF-8 -> LATIN-1 för Adafruit_GFX custom fonts
// ============================================================================
void utf8ToLatin1(const char* src, char* dst, size_t dstSize) {
  if (!dstSize) return;
  size_t out = 0;

  while (*src && out + 1 < dstSize) {
    uint8_t c = (uint8_t)*src++;

    if (c < 0x80) {
      dst[out++] = (char)c;
      continue;
    }

    if ((c == 0xC2 || c == 0xC3) && *src) {
      uint8_t c2 = (uint8_t)*src++;
      uint16_t cp = (c == 0xC2) ? c2 : (uint16_t)(c2 + 0x40);
      if (cp <= 0xFF) {
        dst[out++] = (char)cp;
        continue;
      }
    }

    dst[out++] = '?';
  }

  dst[out] = '\0';
}

void setFont(const GFXfont* font) {
  display.setFont(font);
  display.setTextColor(GxEPD_BLACK);
}

int textWidth(const char* utf8) {
  char latin[128];
  utf8ToLatin1(utf8, latin, sizeof(latin));

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(latin, 0, 0, &x1, &y1, &w, &h);
  return x1 + (int)w;
}

void drawText(const char* utf8, int x, int baselineY) {
  char latin[128];
  utf8ToLatin1(utf8, latin, sizeof(latin));
  display.setCursor(x, baselineY);
  display.print(latin);
}

void drawCentered(const char* utf8, int centerX, int baselineY) {
  char latin[128];
  utf8ToLatin1(utf8, latin, sizeof(latin));

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(latin, 0, baselineY, &x1, &y1, &w, &h);
  display.setCursor(centerX - (x1 + (int)w / 2), baselineY);
  display.print(latin);
}

void drawRight(const char* utf8, int rightX, int baselineY) {
  char latin[64];
  utf8ToLatin1(utf8, latin, sizeof(latin));

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(latin, 0, baselineY, &x1, &y1, &w, &h);
  display.setCursor(rightX - (x1 + (int)w), baselineY);
  display.print(latin);
}

// ============================================================================
// DATUM / CACHE
// ============================================================================
void initializeStateForDate(time_t now) {
  time_t midnight = PriceSources::localMidnight(now);

  PriceSources::clearDay(
    actualToday,
    PriceSources::dateKey(midnight)
  );

  PriceSources::clearDay(
    actualTomorrow,
    PriceSources::dateKey(
      PriceSources::addLocalDays(midnight, 1)
    )
  );

  for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
    PriceSources::clearDay(
      forecastDays[d],
      PriceSources::dateKey(
        PriceSources::addLocalDays(midnight, d)
      )
    );
  }

  lastForecastMidnightYday = -1;
  lastForecastAfternoonYday = -1;
  rtcStateMagic = RTC_STATE_MAGIC;
}

bool alignCachesToToday(time_t now) {
  int todayKey = PriceSources::dateKey(now);
  if (rtcStateMagic != RTC_STATE_MAGIC) {
    initializeStateForDate(now);
    return true;
  }

  if (actualToday.dateKey == todayKey) return false;

  time_t midnight = PriceSources::localMidnight(now);
  int tomorrowKey = PriceSources::dateKey(
    PriceSources::addLocalDays(midnight, 1)
  );

  if (actualTomorrow.dateKey == todayKey) {
    actualToday = actualTomorrow;
    PriceSources::clearDay(actualTomorrow, tomorrowKey);

    // Behåll tidigare prognos genom att flytta cache ett dygn.
    if (forecastDays[1].dateKey == todayKey) {
      forecastDays[0] = forecastDays[1];
      forecastDays[1] = forecastDays[2];
      forecastDays[2] = forecastDays[3];
      PriceSources::clearDay(
        forecastDays[3],
        PriceSources::dateKey(
          PriceSources::addLocalDays(midnight, 3)
        )
      );
    } else {
      for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
        PriceSources::clearDay(
          forecastDays[d],
          PriceSources::dateKey(
            PriceSources::addLocalDays(midnight, d)
          )
        );
      }
    }

    Serial.println("Midnight/date rollover: tomorrow -> today");
    return true;
  }

  // Mer än ett dygn har passerat, eller RTC-state matchar inte längre kalendern.
  Serial.println("Price cache date mismatch: rebuilding cache frame");
  initializeStateForDate(now);
  return true;
}

void rebuildDerivedPriceData(time_t now) {
  time_t midnight = PriceSources::localMidnight(now);

  PriceSources::build72Hours(
    actualToday,
    actualTomorrow,
    forecastDays,
    midnight,
    graphHours
  );

  priceStats = PriceSources::calculateStats(
    actualToday,
    graphHours
  );

  currentPriceValid =
    PriceSources::currentQuarterPrice(
      actualToday,
      now,
      currentPrice
    );

  if (!currentPriceValid) currentPrice = NAN;
}

// ============================================================================
// I2C / RTC
// ============================================================================
uint8_t bcdToDec(uint8_t value) {
  return ((value >> 4) * 10) + (value & 0x0F);
}

uint8_t decToBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

bool readRtcUtc(struct tm& utcTime) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(RTC_TIME_REG);

  if (Wire.endTransmission(false) != 0) {
    Serial.println("RTC: could not address PCF85063");
    return false;
  }

  if (Wire.requestFrom(RTC_ADDR, (uint8_t)7) != 7) {
    Serial.println("RTC: could not read time registers");
    return false;
  }

  uint8_t secondReg  = Wire.read();
  uint8_t minuteReg  = Wire.read();
  uint8_t hourReg    = Wire.read();
  uint8_t dayReg     = Wire.read();
  uint8_t weekdayReg = Wire.read();
  uint8_t monthReg   = Wire.read();
  uint8_t yearReg    = Wire.read();

  if (secondReg & 0x80) {
    Serial.println("RTC: oscillator-stop flag is set");
    return false;
  }

  int second  = bcdToDec(secondReg & 0x7F);
  int minute  = bcdToDec(minuteReg & 0x7F);
  int hour    = bcdToDec(hourReg & 0x3F);
  int day     = bcdToDec(dayReg & 0x3F);
  int weekday = weekdayReg & 0x07;
  int month   = bcdToDec(monthReg & 0x1F);
  int year    = 2000 + bcdToDec(yearReg);

  if (second > 59 || minute > 59 || hour > 23 ||
      day < 1 || day > 31 || month < 1 || month > 12) {
    Serial.println("RTC: invalid date/time");
    return false;
  }

  memset(&utcTime, 0, sizeof(utcTime));
  utcTime.tm_sec = second;
  utcTime.tm_min = minute;
  utcTime.tm_hour = hour;
  utcTime.tm_mday = day;
  utcTime.tm_mon = month - 1;
  utcTime.tm_year = year - 1900;
  utcTime.tm_wday = weekday;
  utcTime.tm_isdst = 0;
  return true;
}

bool writeRtcUtc(time_t epoch) {
  struct tm utcTime;
  gmtime_r(&epoch, &utcTime);

  Wire.beginTransmission(RTC_ADDR);
  Wire.write(RTC_TIME_REG);
  Wire.write(decToBcd(utcTime.tm_sec));
  Wire.write(decToBcd(utcTime.tm_min));
  Wire.write(decToBcd(utcTime.tm_hour));
  Wire.write(decToBcd(utcTime.tm_mday));
  Wire.write(utcTime.tm_wday & 0x07);
  Wire.write(decToBcd(utcTime.tm_mon + 1));
  Wire.write(decToBcd((utcTime.tm_year + 1900) - 2000));

  if (Wire.endTransmission() != 0) {
    Serial.println("RTC: write failed");
    return false;
  }

  Serial.println("RTC updated from NTP");
  return true;
}

bool setSystemClockFromRTC() {
  struct tm rtcUtc;
  if (!readRtcUtc(rtcUtc)) return false;

  Serial.printf(
    "RTC UTC: %04d-%02d-%02d %02d:%02d:%02d\n",
    rtcUtc.tm_year + 1900,
    rtcUtc.tm_mon + 1,
    rtcUtc.tm_mday,
    rtcUtc.tm_hour,
    rtcUtc.tm_min,
    rtcUtc.tm_sec
  );

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&rtcUtc);

  setenv("TZ", TZ_SWEDEN, 1);
  tzset();

  if (epoch <= 0) {
    Serial.println("RTC: could not convert time");
    return false;
  }

  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;

  if (settimeofday(&tv, nullptr) != 0) {
    Serial.println("RTC: settimeofday failed");
    return false;
  }

  struct tm localNow;
  localtime_r(&epoch, &localNow);

  Serial.printf(
    "Local time from RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
    localNow.tm_year + 1900,
    localNow.tm_mon + 1,
    localNow.tm_mday,
    localNow.tm_hour,
    localNow.tm_min,
    localNow.tm_sec
  );

  return true;
}

// ============================================================================
// WI-FI / NTP
// ============================================================================
bool connectWiFi() {
  Serial.println("Connecting to WiFi");
  WiFi.mode(WIFI_STA);

  if (lastWifiChannel >= 1 && lastWifiChannel <= 14) {
    Serial.printf("Trying cached AP, channel %u\n", lastWifiChannel);
    WiFi.begin(
      ep_settings().ssid.c_str(),
      ep_settings().wifiPassword.c_str(),
      lastWifiChannel,
      lastWifiBssid
    );

    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 5000) {
      delay(100);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Trying normal WiFi scan");
    WiFi.disconnect(false);
    WiFi.begin(ep_settings().ssid.c_str(), ep_settings().wifiPassword.c_str());

    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < 15000) {
      delay(100);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }

  lastWifiChannel = WiFi.channel();
  const uint8_t* bssid = WiFi.BSSID();
  if (bssid != nullptr)
    memcpy(lastWifiBssid, bssid, sizeof(lastWifiBssid));

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("WiFi channel: %u\n", lastWifiChannel);
  return true;
}

// Placeringen direkt före syncClock är avsiktlig.
// Den undviker Arduino IDE:s tidigare auto-prototype-problem.
void onNtpTimeSync(struct timeval* tv) {
  ntpSyncCompleted = true;
}

bool syncClock() {
  Serial.println("Starting NTP sync...");
  ntpSyncCompleted = false;

  sntp_set_time_sync_notification_cb(onNtpTimeSync);

  configTzTime(
    TZ_SWEDEN,
    "pool.ntp.org",
    "time.cloudflare.com"
  );

  const unsigned long timeoutMs = 12000;
  const unsigned long started = millis();

  while (!ntpSyncCompleted &&
         millis() - started < timeoutMs) {
    delay(100);
  }

  if (!ntpSyncCompleted) {
    Serial.println("NTP time sync failed");
    return false;
  }

  time_t now = time(nullptr);
  if (now < 100000) {
    Serial.println("NTP returned invalid time");
    return false;
  }

  struct tm localNow;
  localtime_r(&now, &localNow);

  Serial.printf(
    "NTP synced time: %04d-%02d-%02d %02d:%02d:%02d\n",
    localNow.tm_year + 1900,
    localNow.tm_mon + 1,
    localNow.tm_mday,
    localNow.tm_hour,
    localNow.tm_min,
    localNow.tm_sec
  );

  return writeRtcUtc(now);
}

// ============================================================================
// BATTERI / SHTC3 / MQTT
// ============================================================================
uint8_t shtc3Crc(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;

  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];

    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }

  return crc;
}

bool sendI2cCommand(uint8_t address, uint16_t command) {
  Wire.beginTransmission(address);
  Wire.write((uint8_t)(command >> 8));
  Wire.write((uint8_t)(command & 0xFF));
  return Wire.endTransmission() == 0;
}

bool readShtc3(float& temperature, float& humidity) {
  if (!sendI2cCommand(SHTC3_ADDR, 0x3517)) return false;
  delay(1);

  if (!sendI2cCommand(SHTC3_ADDR, 0x609C)) return false;
  delay(2);

  if (Wire.requestFrom(SHTC3_ADDR, (uint8_t)6) != 6) return false;

  uint8_t data[6];
  for (int i = 0; i < 6; i++) data[i] = Wire.read();

  sendI2cCommand(SHTC3_ADDR, 0xB098);

  if (shtc3Crc(data, 2) != data[2] ||
      shtc3Crc(data + 3, 2) != data[5]) {
    return false;
  }

  uint16_t rawTemperature =
    ((uint16_t)data[0] << 8) | data[1];

  uint16_t rawHumidity =
    ((uint16_t)data[3] << 8) | data[4];

  temperature =
    -45.0f + 175.0f * rawTemperature / 65535.0f;

  humidity =
    100.0f * rawHumidity / 65535.0f;

  return true;
}

bool readI2cRegister(uint8_t address,
                     uint8_t reg,
                     uint8_t& value) {
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, (uint8_t)1) != 1) return false;

  value = Wire.read();
  return true;
}

bool readAxp2101Battery(float& voltage, int& level) {
  uint8_t highByte;
  uint8_t lowByte;
  uint8_t percentage;

  if (!readI2cRegister(AXP2101_ADDR, 0x34, highByte) ||
      !readI2cRegister(AXP2101_ADDR, 0x35, lowByte) ||
      !readI2cRegister(AXP2101_ADDR, 0xA4, percentage)) {
    return false;
  }

  uint16_t millivolts =
    ((uint16_t)(highByte & 0x3F) << 8) | lowByte;

  if (millivolts < 2500 ||
      millivolts > 5000 ||
      percentage > 100) {
    return false;
  }

  voltage = millivolts / 1000.0f;
  level = percentage;
  return true;
}

void updateBatteryForDisplay() {
  float voltage = NAN;
  int level = -1;

  if (readAxp2101Battery(voltage, level)) {
    cachedBatteryLevel = level;
    Serial.printf("Battery: %.3f V, %d %%\n", voltage, level);
  } else {
    Serial.println("Battery read failed");
  }
}

bool readTelemetry(TelemetryData& data) {
  data.environmentValid =
    readShtc3(data.temperature, data.humidity);

  data.batteryValid =
    readAxp2101Battery(
      data.batteryVoltage,
      data.batteryLevel
    );

  if (data.batteryValid)
    cachedBatteryLevel = data.batteryLevel;

  return data.environmentValid || data.batteryValid;
}

bool mqttConnect() {
  if (!ep_settings().mqttEnabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  mqttClient.setServer(ep_settings().mqttHost.c_str(), ep_settings().mqttPort);
  mqttClient.setBufferSize(1024);
  mqttClient.setSocketTimeout(3);

  if (mqttClient.connected()) return true;

  if (!mqttClient.connect(
        MQTT_CLIENT_ID,
        ep_settings().mqttUser.c_str(),
        ep_settings().mqttPassword.c_str())) {
    Serial.printf(
      "MQTT connection failed, state=%d\n",
      mqttClient.state()
    );
    return false;
  }

  return true;
}

bool publishDiscoverySensor(
    const char* objectId,
    const char* name,
    const char* valueKey,
    const char* unit,
    const char* deviceClass) {
  String topic =
    String("homeassistant/sensor/epaper_dashboard_") +
    objectId +
    "/config";

  JsonDocument doc;
  doc["name"] = name;
  doc["unique_id"] =
    String("epaper_dashboard_") + objectId;
  doc["state_topic"] = MQTT_STATE_TOPIC;
  doc["value_template"] =
    String("{{ value_json.") + valueKey + " }}";
  doc["unit_of_measurement"] = unit;
  doc["device_class"] = deviceClass;
  doc["state_class"] = "measurement";

  JsonObject device = doc["device"].to<JsonObject>();
  JsonArray identifiers =
    device["identifiers"].to<JsonArray>();

  identifiers.add("epaper_dashboard");
  device["name"] = "e-Paper Dashboard";
  device["manufacturer"] = "Waveshare";
  device["model"] = "ESP32-S3-ePaper-3.97";

  String payload;
  serializeJson(doc, payload);

  return mqttClient.publish(
    topic.c_str(),
    payload.c_str(),
    true
  );
}

bool publishTelemetry(const TelemetryData& data) {
  if (!ep_settings().mqttEnabled) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!mqttConnect()) return false;

  bool discoveryOK = true;

  discoveryOK &= publishDiscoverySensor(
    "battery_voltage",
    "Batterispänning",
    "battery_voltage",
    "V",
    "voltage"
  );

  discoveryOK &= publishDiscoverySensor(
    "battery_level",
    "Batterinivå",
    "battery_level",
    "%",
    "battery"
  );

  discoveryOK &= publishDiscoverySensor(
    "temperature",
    "Temperatur",
    "temperature",
    "°C",
    "temperature"
  );

  discoveryOK &= publishDiscoverySensor(
    "humidity",
    "Luftfuktighet",
    "humidity",
    "%",
    "humidity"
  );

  JsonDocument doc;

  if (data.batteryValid) {
    doc["battery_voltage"] = data.batteryVoltage;
    doc["battery_level"] = data.batteryLevel;
  }

  if (data.environmentValid) {
    doc["temperature"] = data.temperature;
    doc["humidity"] = data.humidity;
  }

  String payload;
  serializeJson(doc, payload);

  bool stateOK =
    mqttClient.publish(
      MQTT_STATE_TOPIC,
      payload.c_str(),
      true
    );

  mqttClient.disconnect();

  if (discoveryOK && stateOK)
    Serial.println("Telemetry published to Home Assistant");
  else
    Serial.println("Telemetry publish incomplete");

  return discoveryOK && stateOK;
}

// ============================================================================
// DISPLAY INIT / SLEEP
// ============================================================================
void initDisplayHardware(bool initial) {
  SPI.begin(EPD_SCLK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, initial);
  display.setRotation(0);
  display.setTextWrap(false);
}

void drawConfigScreen() {
  constexpr int quiet = 4;
  constexpr int scale = 8;
  constexpr int qrSide = (EP_QR_MODULES + quiet * 2) * scale;
  constexpr int qrX = (800 - qrSide) / 2;
  constexpr int qrY = 66;

  display.setFullWindow();
  display.firstPage();

  do {
    display.fillScreen(GxEPD_WHITE);

    setFont(&FONT_HEADER);
    drawCentered("Konfigurera Elprisdisplay", 400, 45);

    display.fillRect(qrX, qrY, qrSide, qrSide, GxEPD_WHITE);

    for (unsigned y = 0; y < EP_QR_MODULES; ++y) {
      for (unsigned x = 0; x < EP_QR_MODULES; ++x) {
        const unsigned k = y * EP_QR_MODULES + x;
        const uint8_t b = pgm_read_byte(&EP_QR_DATA[k / 8]);
        if ((b >> (7 - (k % 8))) & 1u) {
          display.fillRect(
            qrX + (int(x) + quiet) * scale,
            qrY + (int(y) + quiet) * scale,
            scale,
            scale,
            GxEPD_BLACK
          );
        }
      }
    }

    setFont(&FONT_STAT_LABEL);
    drawCentered("Skanna QR-koden för att konfigurera", 400, 397);

    setFont(&FONT_STAT_SMALL);
    drawCentered("eller öppna adressen:", 400, 425);
    drawCentered(EP_QR_URL, 400, 456);
  }
  while (display.nextPage());

  Serial.printf("CONFIG QR: %s\n", EP_QR_URL);
}

void sleepUntilNextQuarter() {
  time_t now = time(nullptr);

  if (now < 100000) {
    Serial.println("No valid time - refusing to sleep");
    emergencyStayAwake = true;
    return;
  }

  uint32_t secondsIntoQuarter =
    (uint32_t)(now % 900);

  uint32_t sleepSeconds =
    900 - secondsIntoQuarter + 10;

  time_t wakeTime = now + sleepSeconds;

  struct tm localNow;
  struct tm localWake;

  localtime_r(&now, &localNow);
  localtime_r(&wakeTime, &localWake);

  Serial.printf(
    "Current time: %02d:%02d:%02d\n",
    localNow.tm_hour,
    localNow.tm_min,
    localNow.tm_sec
  );

  Serial.printf(
    "Sleeping %lu seconds - next wake %02d:%02d:%02d\n",
    (unsigned long)sleepSeconds,
    localWake.tm_hour,
    localWake.tm_min,
    localWake.tm_sec
  );

  display.hibernate();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(
    (uint64_t)sleepSeconds * 1000000ULL
  );

  esp_sleep_enable_ext0_wakeup(
    GPIO_NUM_5,
    0
  );

  Serial.flush();
  delay(100);
  esp_deep_sleep_start();
}

// ============================================================================
// RENDERING
// ============================================================================
void formatPrice(float value, char* out, size_t n) {
  if (isnan(value))
    snprintf(out, n, "--");
  else
    snprintf(out, n, "%.2f", value);
}

void formatQuarterTime(int quarter, char* out, size_t n) {
  if (quarter < 0) {
    snprintf(out, n, "--");
    return;
  }

  int hour = quarter / 4;
  int minute = (quarter % 4) * 15;

  snprintf(
    out,
    n,
    "kl %02d:%02d",
    hour,
    minute
  );
}

void drawBatteryIcon(int x, int y, int percent) {
  constexpr int w = 34;
  constexpr int h = 18;

  display.drawRoundRect(
    x,
    y,
    w,
    h,
    2,
    GxEPD_BLACK
  );

  display.fillRect(
    x + w,
    y + 5,
    4,
    8,
    GxEPD_BLACK
  );

  if (percent < 0) return;

  // Fyra tydliga batteristeg. 1–25 % = ett segment,
  // 26–50 % = två, 51–75 % = tre, 76–100 % = fyra.
  int filledSegments =
    constrain((percent + 24) / 25, 0, 4);

  constexpr int segmentW = 6;
  constexpr int segmentH = 12;
  constexpr int gap = 1;
  constexpr int innerX = 3;
  constexpr int innerY = 3;

  for (int i = 0; i < filledSegments; ++i) {
    display.fillRect(
      x + innerX + i * (segmentW + gap),
      y + innerY,
      segmentW,
      segmentH,
      GxEPD_BLACK
    );
  }
}

void drawHeader(time_t now) {
  struct tm localNow;
  localtime_r(&now, &localNow);

  char updated[40];
  snprintf(
    updated,
    sizeof(updated),
    "Uppdaterad %02d:%02d",
    localNow.tm_hour,
    localNow.tm_min
  );

  char dateText[40];
  snprintf(
    dateText,
    sizeof(dateText),
    "%s %d %s %d",
    WEEKDAY_SHORT[localNow.tm_wday],
    localNow.tm_mday,
    MONTH_SHORT[localNow.tm_mon],
    localNow.tm_year + 1900
  );

  // Samma font för båda, enligt slutjusteringen i Etapp 1.
  setFont(&FONT_HEADER);
  drawText(updated, L.updatedX, L.updatedBaselineY);
  drawText(dateText, L.dateX, L.dateBaselineY);

  drawBatteryIcon(
    L.batteryX,
    L.batteryY,
    cachedBatteryLevel
  );

  char batteryText[16];

  if (cachedBatteryLevel >= 0)
    snprintf(
      batteryText,
      sizeof(batteryText),
      "%d %%",
      cachedBatteryLevel
    );
  else
    snprintf(
      batteryText,
      sizeof(batteryText),
      "-- %%"
    );

  drawRight(
    batteryText,
    L.batteryTextRightX,
    L.updatedBaselineY
  );

  display.drawLine(
    0,
    L.headerLineY,
    799,
    L.headerLineY,
    GxEPD_BLACK
  );
}

void drawNowBox() {
  display.fillRoundRect(
    L.nowX,
    L.nowY,
    L.nowW,
    L.nowH,
    7,
    GxEPD_BLACK
  );

  setFont(&FONT_STAT_LABEL);
  display.setTextColor(GxEPD_WHITE);
  drawCentered(
    "NU",
    L.nowX + L.nowW / 2,
    L.nowY + 23
  );

  char priceText[16];
  formatPrice(
    currentPriceValid ? currentPrice : NAN,
    priceText,
    sizeof(priceText)
  );

  setFont(&FONT_STAT_VALUE);
  display.setTextColor(GxEPD_WHITE);
  drawCentered(
    priceText,
    L.nowX + L.nowW / 2,
    L.nowY + 67
  );

  display.setTextColor(GxEPD_BLACK);
}

void drawStatCell(
    int x0,
    int x1,
    const char* label,
    float value,
    const char* sub) {
  int cx = (x0 + x1) / 2;

  setFont(&FONT_STAT_LABEL);
  drawCentered(
    label,
    cx,
    L.statsTop + 24
  );

  char valueText[16];
  formatPrice(
    value,
    valueText,
    sizeof(valueText)
  );

  setFont(&FONT_STAT_VALUE);
  drawCentered(
    valueText,
    cx,
    L.statsTop + 64
  );

  if (sub && *sub) {
    setFont(&FONT_STAT_SMALL);
    drawCentered(
      sub,
      cx,
      L.statsTop + 80
    );
  }
}

void drawStats() {
  drawNowBox();

  constexpr int x1 = 143;
  constexpr int x2 = 307;
  constexpr int x3 = 471;
  constexpr int x4 = 635;
  constexpr int x5 = 800;

  display.drawLine(
    x1, L.statsTop + 5,
    x1, L.statsBottom - 5,
    GxEPD_BLACK
  );

  display.drawLine(
    x2, L.statsTop + 5,
    x2, L.statsBottom - 5,
    GxEPD_BLACK
  );

  display.drawLine(
    x3, L.statsTop + 5,
    x3, L.statsBottom - 5,
    GxEPD_BLACK
  );

  display.drawLine(
    x4, L.statsTop + 5,
    x4, L.statsBottom - 5,
    GxEPD_BLACK
  );

  char minTime[20];
  char maxTime[20];

  formatQuarterTime(
    priceStats.minTodayQuarter,
    minTime,
    sizeof(minTime)
  );

  formatQuarterTime(
    priceStats.maxTodayQuarter,
    maxTime,
    sizeof(maxTime)
  );

  drawStatCell(
    x1, x2,
    "LÄGST IDAG",
    priceStats.minToday,
    minTime
  );

  drawStatCell(
    x2, x3,
    "HÖGST IDAG",
    priceStats.maxToday,
    maxTime
  );

  drawStatCell(
    x3, x4,
    "SNITT IDAG",
    priceStats.avgToday,
    ""
  );

  drawStatCell(
    x4, x5,
    "SNITT 72H",
    priceStats.avg72,
    ""
  );

  display.drawLine(
    0,
    L.sectionLineY,
    799,
    L.sectionLineY,
    GxEPD_BLACK
  );
}

void drawTopPanel(time_t now) {
  display.fillRect(
    0,
    0,
    800,
    L.sectionLineY + 1,
    GxEPD_WHITE
  );

  drawHeader(now);
  drawStats();
}

void drawDayHeadings(time_t now) {
  time_t midnight =
    PriceSources::localMidnight(now);

  int dayW = L.chartW / 3;

  for (int d = 0; d < 3; ++d) {
    time_t dayEpoch =
      PriceSources::addLocalDays(
        midnight,
        d
      );

    struct tm dt;
    localtime_r(&dayEpoch, &dt);

    char title[24];

    if (d == 0)
      snprintf(title, sizeof(title), "IDAG");
    else if (d == 1)
      snprintf(title, sizeof(title), "IMORGON");
    else
      snprintf(
        title,
        sizeof(title),
        "%s",
        WEEKDAY_LONG_UPPER[dt.tm_wday]
      );

    char dateText[24];

    snprintf(
      dateText,
      sizeof(dateText),
      "%s %d %s",
      WEEKDAY_SHORT[dt.tm_wday],
      dt.tm_mday,
      MONTH_SHORT[dt.tm_mon]
    );

    int centerX =
      L.chartX +
      d * dayW +
      dayW / 2;

    setFont(&FONT_DAY);
    drawCentered(
      title,
      centerX,
      L.dayTitleBaselineY
    );

    setFont(&FONT_DAY_DATE);
    drawCentered(
      dateText,
      centerX,
      L.dayDateBaselineY
    );
  }
}

float niceStep(float raw) {
  if (raw <= 0.25f) return 0.25f;
  if (raw <= 0.50f) return 0.50f;
  if (raw <= 1.00f) return 1.00f;
  if (raw <= 2.00f) return 2.00f;
  if (raw <= 5.00f) return 5.00f;
  return ceilf(raw / 5.0f) * 5.0f;
}

YScale calculateYScale() {
  float minP = 0.0f;
  float maxP = 0.0f;
  bool have = false;

  for (const HourPrice& h : graphHours) {
    if (h.kind == PriceKind::Missing ||
        isnan(h.price)) {
      continue;
    }

    if (!have) {
      minP = maxP = h.price;
      have = true;
    } else {
      minP = min(minP, h.price);
      maxP = max(maxP, h.price);
    }
  }

  if (currentPriceValid) {
    if (!have) {
      minP = maxP = currentPrice;
      have = true;
    } else {
      minP = min(minP, currentPrice);
      maxP = max(maxP, currentPrice);
    }
  }

  if (!have) return YScale{};

  minP = min(0.0f, minP);
  maxP = max(0.0f, maxP);

  float range = maxP - minP;
  if (range < 0.5f) range = 0.5f;

  float step = niceStep(range / 6.0f);

  float scaleMin =
    floorf(minP / step) * step;

  float scaleMax =
    ceilf(maxP / step) * step;

  if (scaleMax <= scaleMin)
    scaleMax = scaleMin + step;

  return {
    scaleMin,
    scaleMax,
    step
  };
}

YScale currentScale;

int chartYForPrice(float price) {
  float range =
    currentScale.maxValue -
    currentScale.minValue;

  if (range <= 0.0f)
    return L.chartY + L.chartH;

  float normalized =
    (price - currentScale.minValue) /
    range;

  normalized =
    constrain(
      normalized,
      0.0f,
      1.0f
    );

  return
    L.chartY +
    L.chartH -
    lroundf(
      normalized * L.chartH
    );
}

void drawChartGrid() {
  setFont(&FONT_Y_AXIS);

  int tickCount =
    lroundf(
      (currentScale.maxValue -
       currentScale.minValue) /
      currentScale.step
    );

  for (int i = 0; i <= tickCount; ++i) {
    float value =
      currentScale.minValue +
      i * currentScale.step;

    int y = chartYForPrice(value);

    if (fabsf(value) < 0.0001f) {
      display.drawLine(
        L.chartX,
        y,
        L.chartX + L.chartW,
        y,
        GxEPD_BLACK
      );
    } else {
      for (int x = L.chartX;
           x <= L.chartX + L.chartW;
           x += 5) {
        display.drawPixel(
          x,
          y,
          GxEPD_BLACK
        );
      }
    }

    char label[16];

    if (fabsf(value) < 10.0f)
      snprintf(
        label,
        sizeof(label),
        "%.2f",
        value
      );
    else
      snprintf(
        label,
        sizeof(label),
        "%.1f",
        value
      );

    drawRight(
      label,
      L.chartX - 10,
      y + 7
    );
  }

  display.drawLine(
    L.chartX,
    L.chartY,
    L.chartX,
    L.chartY + L.chartH,
    GxEPD_BLACK
  );

  setFont(&FONT_UNIT);
  drawText(
    "SEK/kWh",
    0,
    L.chartY - 24
  );
}

void drawForecastBar(
    int x,
    int valueY,
    int w,
    int zeroY) {
  int top = min(valueY, zeroY);
  int bottom = max(valueY, zeroY);

  display.drawLine(
    x,
    top,
    x,
    bottom,
    GxEPD_BLACK
  );

  display.drawLine(
    x + 1,
    top,
    x + 1,
    bottom,
    GxEPD_BLACK
  );

  display.drawLine(
    x + w,
    top,
    x + w,
    bottom,
    GxEPD_BLACK
  );

  display.drawLine(
    x + w - 1,
    top,
    x + w - 1,
    bottom,
    GxEPD_BLACK
  );

  for (int xx = x;
       xx <= x + w;
       xx += 4) {
    display.drawLine(
      xx,
      valueY,
      min(xx + 2, x + w),
      valueY,
      GxEPD_BLACK
    );
  }
}

void drawPastActualBar(
    int x,
    int valueY,
    int w,
    int zeroY) {
  int top = min(valueY, zeroY);
  int bottom = max(valueY, zeroY);

  // 75 % svart 4x4-dither. På svart/vitt e-paper upplevs det som
  // mörkgrått men lämnar framtida faktiska priser tydligt helsvarta.
  for (int yy = top; yy < bottom; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      if (((xx + yy) & 0x03) != 0) {
        display.drawPixel(
          x + xx,
          yy,
          GxEPD_BLACK
        );
      }
    }
  }
}

void drawPriceBars(time_t now) {
  float slot =
    (float)L.chartW /
    (float)GRAPH_HOURS;

  int barW =
    max(
      4,
      (int)floorf(slot) - 2
    );

  int zeroY =
    chartYForPrice(0.0f);

  struct tm localNow;
  localtime_r(&now, &localNow);
  int currentHour = localNow.tm_hour;

  for (int i = 0;
       i < GRAPH_HOURS;
       ++i) {
    const HourPrice& h =
      graphHours[i];

    if (h.kind == PriceKind::Missing ||
        isnan(h.price)) {
      continue;
    }

    int x =
      L.chartX +
      lroundf(i * slot) +
      1;

    int valueY =
      chartYForPrice(h.price);

    if (h.kind == PriceKind::Actual) {
      if (i < currentHour) {
        drawPastActualBar(
          x,
          valueY,
          barW,
          zeroY
        );
      } else {
        int top =
          min(valueY, zeroY);

        int height =
          max(
            1,
            abs(valueY - zeroY)
          );

        display.fillRect(
          x,
          top,
          barW,
          height,
          GxEPD_BLACK
        );
      }
    } else {
      drawForecastBar(
        x,
        valueY,
        barW,
        zeroY
      );
    }
  }
}

void drawDaySeparators() {
  int dayW = L.chartW / 3;

  for (int d = 1; d <= 2; ++d) {
    int x =
      L.chartX +
      d * dayW;

    for (int y = L.chartY - 8;
         y < L.chartY + L.chartH;
         y += 8) {
      display.drawLine(
        x,
        y,
        x,
        min(
          y + 4,
          L.chartY + L.chartH
        ),
        GxEPD_BLACK
      );
    }
  }
}

void drawCurrentMarker(time_t now) {
  if (!currentPriceValid) return;

  struct tm localNow;
  localtime_r(&now, &localNow);

  float slot =
    (float)L.chartW /
    (float)GRAPH_HOURS;

  int x =
    L.chartX +
    lroundf(
      (localNow.tm_hour + 0.5f) *
      slot
    );

  int y =
    chartYForPrice(currentPrice);

  for (int yy = L.chartY;
       yy < L.chartY + L.chartH;
       yy += 8) {
    display.drawLine(
      x,
      yy,
      x,
      min(
        yy + 4,
        L.chartY + L.chartH
      ),
      GxEPD_BLACK
    );
  }

  display.fillCircle(
    x,
    y,
    8,
    GxEPD_WHITE
  );

  display.drawCircle(
    x,
    y,
    8,
    GxEPD_BLACK
  );

  display.drawCircle(
    x,
    y,
    7,
    GxEPD_BLACK
  );

  constexpr int tagW = 42;
  constexpr int tagH = 23;

  int tagX =
    constrain(
      x - tagW / 2,
      L.chartX + 2,
      L.chartX + L.chartW -
        tagW - 2
    );

  int tagY =
    L.chartY + 6;

  display.fillRoundRect(
    tagX,
    tagY,
    tagW,
    tagH,
    4,
    GxEPD_BLACK
  );

  setFont(&FONT_NOW_TAG);
  display.setTextColor(GxEPD_WHITE);

  drawCentered(
    "NU",
    tagX + tagW / 2,
    tagY + 17
  );

  display.setTextColor(GxEPD_BLACK);
}

void drawXAxis() {
  setFont(&FONT_X_AXIS);

  float slot =
    (float)L.chartW /
    (float)GRAPH_HOURS;

  int bottom =
    L.chartY +
    L.chartH;

  for (int i = 0;
       i < GRAPH_HOURS;
       i += 3) {
    int x =
      L.chartX +
      lroundf(
        (i + 0.5f) * slot
      );

    display.drawLine(
      x,
      bottom,
      x,
      bottom + 6,
      GxEPD_BLACK
    );

    char label[4];
    snprintf(
      label,
      sizeof(label),
      "%02d",
      i % 24
    );

    drawCentered(
      label,
      x,
      L.xLabelBaselineY
    );
  }
}

void drawGraphPanel(time_t now) {
  display.fillRect(
    0,
    L.sectionLineY + 1,
    800,
    480 - (L.sectionLineY + 1),
    GxEPD_WHITE
  );

  drawDayHeadings(now);

  currentScale =
    calculateYScale();

  drawChartGrid();
  drawPriceBars(now);
  drawDaySeparators();
  drawCurrentMarker(now);
  drawXAxis();

  int availableHours = 0;
  for (const HourPrice& h : graphHours) {
    if (h.kind != PriceKind::Missing && !isnan(h.price))
      ++availableHours;
  }

  if (availableHours == 0) {
    setFont(&FONT_DAY);
    drawCentered(
      "INGA PRISDATA",
      L.chartX + L.chartW / 2,
      L.chartY + L.chartH / 2
    );
  }
}

void drawDashboard(time_t now) {
  display.fillScreen(GxEPD_WHITE);
  drawTopPanel(now);
  drawGraphPanel(now);
}

void refreshTopPanel(time_t now) {
  display.setPartialWindow(
    0,
    0,
    800,
    L.sectionLineY + 1
  );

  display.firstPage();

  do {
    drawTopPanel(now);
  }
  while (display.nextPage());
}

void refreshGraphPanel(time_t now) {
  display.setPartialWindow(
    0,
    L.sectionLineY,
    800,
    480 - L.sectionLineY
  );

  display.firstPage();

  do {
    drawGraphPanel(now);
  }
  while (display.nextPage());
}


// ============================================================================
// PRISDIAGNOSTIK
// ============================================================================
void printPriceDiagnostics(time_t now) {
  Serial.println();
  Serial.println("----- PRICE DIAGNOSTICS -----");
  Serial.printf(
    "actualToday:    date=%d points=%d/96 complete=%s\n",
    actualToday.dateKey,
    PriceSources::validCount(actualToday),
    PriceSources::isComplete(actualToday) ? "yes" : "no"
  );
  Serial.printf(
    "actualTomorrow: date=%d points=%d/96 complete=%s\n",
    actualTomorrow.dateKey,
    PriceSources::validCount(actualTomorrow),
    PriceSources::isComplete(actualTomorrow) ? "yes" : "no"
  );

  for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
    Serial.printf(
      "forecast[%d]:    date=%d points=%d/96\n",
      d,
      forecastDays[d].dateKey,
      PriceSources::validCount(forecastDays[d])
    );
  }

  int actualHours = 0;
  int forecastHours = 0;
  int missingHours = 0;

  for (int i = 0; i < GRAPH_HOURS; ++i) {
    switch (graphHours[i].kind) {
      case PriceKind::Actual:
        ++actualHours;
        break;
      case PriceKind::Forecast:
        ++forecastHours;
        break;
      default:
        ++missingHours;
        break;
    }
  }

  Serial.printf(
    "graphHours: actual=%d forecast=%d missing=%d total=%d\n",
    actualHours,
    forecastHours,
    missingHours,
    GRAPH_HOURS
  );

  Serial.printf(
    "currentQuarter: valid=%s value=%.4f\n",
    currentPriceValid ? "yes" : "no",
    currentPriceValid ? currentPrice : NAN
  );

  struct tm localNow{};
  localtime_r(&now, &localNow);
  Serial.printf(
    "local time: %04d-%02d-%02d %02d:%02d:%02d\n",
    localNow.tm_year + 1900,
    localNow.tm_mon + 1,
    localNow.tm_mday,
    localNow.tm_hour,
    localNow.tm_min,
    localNow.tm_sec
  );

  Serial.printf("free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println("-----------------------------");
  Serial.println();
}

// ============================================================================
// SCHEDULER
// ============================================================================
void runScheduler() {
  time_t now = time(nullptr);
  if (now < 100000) return;

  struct tm localNow;
  localtime_r(&now, &localNow);

  int quarter =
    localNow.tm_hour * 4 +
    localNow.tm_min / 15;

  if (localNow.tm_yday == lastHandledDay &&
      quarter == lastHandledQuarter) {
    Serial.println("This quarter was already handled");
    return;
  }

  lastHandledDay = localNow.tm_yday;
  lastHandledQuarter = quarter;

  Serial.printf(
    "\n=== Wake cycle: %04d-%02d-%02d %02d:%02d ===\n",
    localNow.tm_year + 1900,
    localNow.tm_mon + 1,
    localNow.tm_mday,
    localNow.tm_hour,
    localNow.tm_min
  );

  bool fullHour =
    localNow.tm_min == 0;

  bool midnight =
    fullHour &&
    localNow.tm_hour == 0;

  bool dateChanged =
    alignCachesToToday(now);

  bool actualTodayMissing =
    !PriceSources::isComplete(actualToday);

  bool actualTomorrowMissing =
    !PriceSources::isComplete(actualTomorrow);

  bool afterPricePublicationWindow =
    localNow.tm_hour > 13 ||
    (localNow.tm_hour == 13 &&
     localNow.tm_min >= 15);

  bool shouldTryTomorrowPrices =
    afterPricePublicationWindow &&
    actualTomorrowMissing;

  // Wolta hämtas i två separata dagliga luckor:
  // 1) vid midnatt/dygnsskifte
  // 2) från 13:15, normalt i samma Wi-Fi-session som morgondagspriserna.
  // Varje lucka används högst en gång per dygn. Ett 404-svar för
  // morgondagens verkliga priser får alltså inte trigga ny Wolta-hämtning.
  bool shouldRefreshForecastAtMidnight =
    (midnight || dateChanged) &&
    lastForecastMidnightYday != localNow.tm_yday;

  bool shouldRefreshForecastAt1315 =
    afterPricePublicationWindow &&
    lastForecastAfternoonYday != localNow.tm_yday;

  bool shouldRefreshForecast =
    shouldRefreshForecastAtMidnight ||
    shouldRefreshForecastAt1315;

  bool needsWiFi =
    actualTodayMissing ||
    shouldTryTomorrowPrices ||
    shouldRefreshForecast;

  updateBatteryForDisplay();

  TelemetryData telemetry;
  bool telemetryReady = false;

  if (needsWiFi)
    telemetryReady = readTelemetry(telemetry);

  if (needsWiFi &&
      WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi required for this wake cycle");

    if (!connectWiFi())
      Serial.println("WiFi connection FAILED");
  } else if (!needsWiFi) {
    Serial.println("WiFi not required");
  }

  bool dataChanged = dateChanged;
  bool newTomorrowPrices = false;

  if (WiFi.status() == WL_CONNECTED) {
    time_t midnightEpoch =
      PriceSources::localMidnight(now);

    if (actualTodayMissing) {
      Serial.println("ACTION: fetching today's actual prices");

      if (PriceSources::fetchActualDay(
            ep_settings().priceZone.c_str(),
            midnightEpoch,
            ep_settings().includeVat,
            actualToday)) {
        dataChanged = true;
      }
    }

    if (shouldTryTomorrowPrices) {
      Serial.println("ACTION: trying tomorrow's actual prices");

      bool wasComplete =
        PriceSources::isComplete(
          actualTomorrow
        );

      time_t tomorrow =
        PriceSources::addLocalDays(
          midnightEpoch,
          1
        );

      if (PriceSources::fetchActualDay(
            ep_settings().priceZone.c_str(),
            tomorrow,
            ep_settings().includeVat,
            actualTomorrow)) {
        bool nowComplete =
          PriceSources::isComplete(
            actualTomorrow
          );

        if (!wasComplete && nowComplete) {
          newTomorrowPrices = true;
          dataChanged = true;

          Serial.println(
            "Tomorrow prices complete"
          );

          // Samma princip som v0.9:
          // NTP endast när de nya morgondagspriserna verkligen kommit.
          syncClock();
          now = time(nullptr);
          localtime_r(&now, &localNow);
        }
      } else {
        Serial.println(
          "Tomorrow prices not available yet"
        );
      }
    }

    if (shouldRefreshForecast) {
      // Markera den schemalagda luckan när Wi-Fi faktiskt är uppe och
      // vi står i begrepp att göra Wolta-anropet. Därmed hämtas inte
      // Wolta igen vid varje kvart bara för att morgondagspriset ger 404.
      if (shouldRefreshForecastAtMidnight)
        lastForecastMidnightYday = localNow.tm_yday;

      if (shouldRefreshForecastAt1315)
        lastForecastAfternoonYday = localNow.tm_yday;

      Serial.println("ACTION: refreshing Wolta forecast");

      time_t midnightEpoch2 =
        PriceSources::localMidnight(now);

      if (PriceSources::fetchForecast(
            ep_settings().priceZone.c_str(),
            midnightEpoch2,
            ep_settings().includeVat,
            forecastDays)) {
        dataChanged = true;
      }
    }

    if (telemetryReady)
      publishTelemetry(telemetry);
  }

  rebuildDerivedPriceData(now);

  // Toppen innehåller uppdateringstid, batteri och aktuellt pris:
  // den uppdateras varje kvart.
  refreshTopPanel(now);

  // Grafen behöver bara ritas om varje hel timme när NU flyttar,
  // vid dygnsskifte eller när nya pris/prognosdata har kommit.
  if (fullHour ||
      dataChanged ||
      newTomorrowPrices) {
    refreshGraphPanel(now);
  }
}

// ============================================================================
// KALLSTART
// ============================================================================
bool initialDataFetch(bool clockWasValid) {
  if (!connectWiFi()) return false;

  if (!clockWasValid) {
    if (!syncClock()) return false;
  }

  time_t now = time(nullptr);
  if (now < 100000) return false;

  alignCachesToToday(now);

  time_t midnight =
    PriceSources::localMidnight(now);

  bool todayOK =
    PriceSources::fetchActualDay(
      ep_settings().priceZone.c_str(),
      midnight,
      ep_settings().includeVat,
      actualToday
    );

  time_t tomorrow =
    PriceSources::addLocalDays(
      midnight,
      1
    );

  bool tomorrowOK =
    PriceSources::fetchActualDay(
      ep_settings().priceZone.c_str(),
      tomorrow,
      ep_settings().includeVat,
      actualTomorrow
    );

  bool forecastOK =
    PriceSources::fetchForecast(
      ep_settings().priceZone.c_str(),
      midnight,
      ep_settings().includeVat,
      forecastDays
    );

  struct tm localNow;
  localtime_r(&now, &localNow);

  if (forecastOK) {
    bool after1315 =
      localNow.tm_hour > 13 ||
      (localNow.tm_hour == 13 && localNow.tm_min >= 15);

    if (after1315)
      lastForecastAfternoonYday = localNow.tm_yday;
    else
      lastForecastMidnightYday = localNow.tm_yday;
  }

  // V0.9-principen: om RTC redan var giltig och Wi-Fi ändå är uppe,
  // korrigera den mot NTP efter lyckad prisdatahämtning.
  if (clockWasValid &&
      (todayOK || tomorrowOK || forecastOK)) {
    syncClock();
  }

  return todayOK || forecastOK;
}

// ============================================================================
// SETUP / LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wakeCause =
    esp_sleep_get_wakeup_cause();

  // Vid kallstart/reset ger vi Serial Monitor gott om tid att ansluta.
  // Normal timer-wake får fortfarande vara snabb.
  if (wakeCause != ESP_SLEEP_WAKEUP_TIMER) {
    delay(3000);
  }

  Serial.println();
  Serial.println("Elprisdisplay 2.0 v0.2.2 CONFIG DEV 2 BLE NAME FIX");
  Serial.println("======================");

  const bool configValid = ep_load_settings();
  if (configValid)
    ep_print_settings_summary();
  else
    Serial.println("CONFIG: ingen giltig NVS-konfiguration");

  pinMode(
    FUNCTION_BUTTON,
    INPUT_PULLUP
  );

  bool configModeRequested = !configValid;

  switch (wakeCause) {
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wake cause: TIMER");
      break;

    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wake cause: FUNCTION BUTTON");
      configModeRequested = true;
      break;

    default:
      Serial.println("Wake cause: POWER ON / RESET");
      break;
  }

  if (digitalRead(FUNCTION_BUTTON) == LOW)
    configModeRequested = true;

  if (configModeRequested) {
    Serial.println("CONFIG MODE requested");
    initDisplayHardware(true);
    drawConfigScreen();
    display.hibernate();
    ep_run_config_mode();
    for (;;) delay(1000);
  }

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  bool clockOK =
    setSystemClockFromRTC();

  // Normal timer-wake:
  // återanvänd RTC-cache, initiera e-paper utan cold/full reset,
  // kör schedulern och gå tillbaka till deep sleep.
if (wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
    clockOK &&
    !emergencyStayAwake) {
  initDisplayHardware(false);
  runScheduler();


  sleepUntilNextQuarter();
  return;
}

  // Kallstart / reset / function-button.
  updateBatteryForDisplay();

  TelemetryData telemetry;
  bool telemetryReady =
    readTelemetry(telemetry);

  bool dataOK =
    initialDataFetch(clockOK);

  if (telemetryReady &&
      WiFi.status() == WL_CONNECTED) {
    publishTelemetry(telemetry);
  }

  time_t now = time(nullptr);

  if (now >= 100000) {
    alignCachesToToday(now);
    rebuildDerivedPriceData(now);
    printPriceDiagnostics(now);
  }

  initDisplayHardware(true);

  display.setFullWindow();
  display.firstPage();

  do {
    drawDashboard(now);
  }
  while (display.nextPage());

  display.hibernate();

  Serial.printf(
    "Dashboard drawn, data=%s\n",
    dataOK ? "OK" : "incomplete"
  );

  if (now >= 100000) {
    struct tm localNow;
    localtime_r(&now, &localNow);

    lastHandledDay =
      localNow.tm_yday;

    lastHandledQuarter =
      localNow.tm_hour * 4 +
      localNow.tm_min / 15;
  }

  // Under integrationsfasen håller vi kallstart/reset vaken så att
  // Serial Monitor kan ansluta efter renderingen. Dessutom får en misslyckad
  // datahämtning ALDRIG gömmas genom omedelbar deep sleep.
  if (INTEGRATION_DEBUG_HOLD) {
    Serial.println();
    Serial.println("INTEGRATION DEBUG HOLD: deep sleep disabled after cold start/reset");
    Serial.println("Open/keep Serial Monitor at 115200 baud.");
    Serial.flush();
    return;
  }

  if (!dataOK) {
    Serial.println();
    Serial.println("DATA FETCH FAILED: deep sleep disabled for diagnostics");
    Serial.flush();
    emergencyStayAwake = true;
    return;
  }

  if (!emergencyStayAwake)
    sleepUntilNextQuarter();
}

void loop() {
  if (INTEGRATION_DEBUG_HOLD) {
    delay(1000);
    return;
  }

  if (emergencyStayAwake) {
    runScheduler();
    delay(1000);
  } else {
    delay(1000);
  }
}
