#include "PriceSources.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>

namespace {
constexpr const char* ACTUAL_BASE =
  "https://www.elprisetjustnu.se/api/v1/prices";
constexpr const char* FORECAST_BASE =
  "https://wolta.se/api/v1/prices/forecast?zone=";

float applyVat(float price, bool includeVat) {
  return includeVat ? price * 1.25f : price;
}

// Convert a Gregorian UTC date directly to Unix epoch seconds without timegm().
// This is independent of the process TZ and therefore safe while the ESP32 uses
// Europe/Stockholm for local display/scheduler time.
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
    (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe =
    yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 +
         static_cast<int64_t>(doe) - 719468;
}

time_t utcEpoch(int year, int month, int day,
                int hour, int minute, int second) {
  const int64_t days = daysFromCivil(
    year,
    static_cast<unsigned>(month),
    static_cast<unsigned>(day)
  );
  const int64_t seconds =
    days * 86400LL +
    static_cast<int64_t>(hour) * 3600LL +
    static_cast<int64_t>(minute) * 60LL +
    second;
  return static_cast<time_t>(seconds);
}

bool getJson(const String& url, JsonDocument& doc, int& httpCode) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  Serial.printf("HTTP begin: %s\n", url.c_str());
  Serial.printf("Heap before HTTP: %u bytes\n", (unsigned)ESP.getFreeHeap());

  if (!http.begin(client, url)) {
    httpCode = -1;
    Serial.println("HTTP begin FAILED");
    return false;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  httpCode = http.GET();

  Serial.printf(
    "HTTP result: code=%d content-length=%d\n",
    httpCode,
    http.getSize()
  );

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP %d: %s\n", httpCode, url.c_str());
    http.end();
    return false;
  }

  // Läs hela svaret innan JSON-tolkning. Detta är avsiktligt:
  // Elpriset just nu kan svara chunkat (content-length=-1), vilket gav
  // ArduinoJson InvalidInput när vi parsade direkt från HTTP-strömmen.
  String payload = http.getString();
  http.end();

  Serial.printf(
    "HTTP payload: %u bytes, heap=%u\n",
    (unsigned)payload.length(),
    (unsigned)ESP.getFreeHeap()
  );

  if (payload.length() == 0) {
    Serial.println("HTTP payload is empty");
    return false;
  }

  // Tåla UTF-8 BOM om servern skulle skicka en sådan.
  if (payload.length() >= 3 &&
      (uint8_t)payload[0] == 0xEF &&
      (uint8_t)payload[1] == 0xBB &&
      (uint8_t)payload[2] == 0xBF) {
    Serial.println("Removing UTF-8 BOM");
    payload.remove(0, 3);
  }

  // Hoppa över eventuell inledande whitespace innan diagnostik.
  int first = 0;
  while (first < (int)payload.length() &&
         isspace((unsigned char)payload[first])) {
    ++first;
  }

  if (first >= (int)payload.length()) {
    Serial.println("HTTP payload contains only whitespace");
    return false;
  }

  char firstChar = payload[first];
  Serial.printf("JSON first non-space char: '%c'\n", firstChar);

  if (firstChar != '[' && firstChar != '{') {
    Serial.print("Unexpected payload prefix: ");
    Serial.println(payload.substring(first, min(first + 120, (int)payload.length())));
    return false;
  }

  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf(
      "JSON error: %s at payload size %u\n",
      err.c_str(),
      (unsigned)payload.length()
    );
    Serial.print("Payload prefix: ");
    Serial.println(payload.substring(first, min(first + 160, (int)payload.length())));
    return false;
  }

  Serial.printf(
    "JSON OK, heap after parse: %u bytes\n",
    (unsigned)ESP.getFreeHeap()
  );

  return true;
}


int dateKeyFromIsoDate(const char* s) {
  if (!s || strlen(s) < 10) return 0;

  int year = 0;
  int month = 0;
  int day = 0;

  if (sscanf(s, "%4d-%2d-%2d", &year, &month, &day) != 3)
    return 0;

  return year * 10000 + month * 100 + day;
}

int quarterIndexFromLocal(time_t epoch) {
  tm local{};
  localtime_r(&epoch, &local);
  int minuteQuarter = local.tm_min / 15;
  if (minuteQuarter < 0 || minuteQuarter > 3) return -1;
  return local.tm_hour * 4 + minuteQuarter;
}

float hourlyAverage(const QuarterDay& day, int hour, bool& complete) {
  complete = false;
  if (hour < 0 || hour > 23) return NAN;

  float sum = 0.0f;
  int count = 0;
  for (int q = hour * 4; q < hour * 4 + 4; ++q) {
    if (day.valid[q]) {
      sum += day.price[q];
      ++count;
    }
  }

  if (count == 0) return NAN;
  complete = count == 4;
  return sum / count;
}
}

void PriceSources::clearDay(QuarterDay& day, int key) {
  day.dateKey = key;
  for (int i = 0; i < QUARTERS_PER_DAY; ++i) {
    day.price[i] = NAN;
    day.valid[i] = false;
  }
}

int PriceSources::validCount(const QuarterDay& day) {
  int count = 0;
  for (bool v : day.valid) if (v) ++count;
  return count;
}

bool PriceSources::isComplete(const QuarterDay& day) {
  // The display still uses 96 wall-clock quarter slots.
  // DST switch days remain a known limitation inherited from v0.9.
  return validCount(day) == QUARTERS_PER_DAY;
}

int PriceSources::dateKey(time_t epoch) {
  tm local{};
  localtime_r(&epoch, &local);
  return (local.tm_year + 1900) * 10000 +
         (local.tm_mon + 1) * 100 +
         local.tm_mday;
}

time_t PriceSources::localMidnight(time_t epoch) {
  tm local{};
  localtime_r(&epoch, &local);
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return mktime(&local);
}

time_t PriceSources::addLocalDays(time_t midnightEpoch, int days) {
  tm local{};
  localtime_r(&midnightEpoch, &local);
  local.tm_mday += days;
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return mktime(&local);
}

bool PriceSources::parseIso8601(const char* s, time_t& epochUtc) {
  if (!s || strlen(s) < 19) return false;

  int year, mon, day, hour, minute, second;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d",
             &year, &mon, &day, &hour, &minute, &second) != 6) {
    return false;
  }

  time_t base = utcEpoch(
    year,
    mon,
    day,
    hour,
    minute,
    second
  );

  const char* tz = s + 19;
  if (*tz == '.') {
    while (*tz && *tz != 'Z' && *tz != 'z' && *tz != '+' && *tz != '-') ++tz;
  }

  if (*tz == '\0' || *tz == 'Z' || *tz == 'z') {
    epochUtc = base;
    return true;
  }

  if (*tz != '+' && *tz != '-') return false;

  int offHour = 0;
  int offMinute = 0;
  if (sscanf(tz + 1, "%2d:%2d", &offHour, &offMinute) < 1) return false;

  int offset = offHour * 3600 + offMinute * 60;
  epochUtc = (*tz == '+') ? base - offset : base + offset;
  return true;
}

bool PriceSources::fetchActualDay(const char* zone,
                                  time_t localDay,
                                  bool includeVat,
                                  QuarterDay& destination) {
  tm d{};
  localtime_r(&localDay, &d);

  char url[180];
  snprintf(url, sizeof(url),
           "%s/%04d/%02d-%02d_%s.json",
           ACTUAL_BASE,
           d.tm_year + 1900,
           d.tm_mon + 1,
           d.tm_mday,
           zone);

  JsonDocument doc;
  int httpCode = 0;

  Serial.printf("GET actual: %s\n", url);
  if (!getJson(url, doc, httpCode)) return false;

  JsonArray rows = doc.as<JsonArray>();
  if (rows.isNull()) {
    Serial.println("Actual price response is not an array");
    return false;
  }

  QuarterDay temp;
  clearDay(temp, dateKey(localDay));

  int accepted = 0;
  bool loggedFirstActual = false;

  for (JsonObject row : rows) {
    JsonVariantConst startValue = row["time_start"];
    JsonVariantConst priceValue = row["SEK_per_kWh"];

    const char* start = startValue.as<const char*>();
    if (!start || priceValue.isNull()) continue;

    float price = priceValue.as<float>();

    time_t epoch = 0;
    if (!parseIso8601(start, epoch)) continue;
    if (dateKey(epoch) != temp.dateKey) continue;

    int q = quarterIndexFromLocal(epoch);
    if (q < 0 || q >= QUARTERS_PER_DAY) continue;

    if (!loggedFirstActual) {
      tm local{};
      localtime_r(&epoch, &local);
      Serial.printf(
        "First actual point: %s -> local %04d-%02d-%02d %02d:%02d, q=%d, %.5f SEK/kWh\n",
        start,
        local.tm_year + 1900,
        local.tm_mon + 1,
        local.tm_mday,
        local.tm_hour,
        local.tm_min,
        q,
        price
      );
      loggedFirstActual = true;
    }

    temp.price[q] = applyVat(price, includeVat);
    temp.valid[q] = true;
    ++accepted;
  }

  if (accepted == 0) {
    Serial.printf(
      "No usable actual-price points (JSON rows=%u, target date=%d)\n",
      (unsigned)rows.size(),
      temp.dateKey
    );

    if (rows.size() > 0) {
      Serial.print("First actual row: ");
      serializeJson(rows[0], Serial);
      Serial.println();
    }

    return false;
  }

  destination = temp;
  Serial.printf("Actual cache %d: accepted=%d valid=%d/96 points\n",
                destination.dateKey,
                accepted,
                validCount(destination));
  return true;
}

bool PriceSources::fetchForecast(
    const char* zone,
    time_t baseLocalMidnight,
    bool includeVat,
    QuarterDay destination[FORECAST_CACHE_DAYS]) {
  String url = String(FORECAST_BASE) + zone;

  JsonDocument doc;
  int httpCode = 0;

  Serial.printf("GET forecast: %s\n", url.c_str());
  if (!getJson(url, doc, httpCode)) return false;

  JsonArray days = doc["days"].as<JsonArray>();
  if (days.isNull()) {
    Serial.println("Forecast response has no days[]");
    return false;
  }

  QuarterDay temp[FORECAST_CACHE_DAYS];
  for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
    clearDay(temp[d], dateKey(addLocalDays(baseLocalMidnight, d)));
  }

  int accepted = 0;
  int rejectedDay = 0;
  int rejectedTimestamp = 0;
  int rejectedQuarter = 0;
  bool loggedFirstPoint = false;

  for (JsonObject day : days) {
    JsonVariantConst dayDateValue = day["date"];
    const char* dayDate = dayDateValue.as<const char*>();
    int woltaDayKey = dateKeyFromIsoDate(dayDate);

    int slot = -1;
    for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
      if (temp[d].dateKey == woltaDayKey) {
        slot = d;
        break;
      }
    }

    JsonArray points = day["points"].as<JsonArray>();
    if (points.isNull()) continue;

    if (slot < 0) {
      rejectedDay += points.size();
      Serial.printf(
        "Forecast day %s (key=%d) is outside cache frame\n",
        dayDate ? dayDate : "(null)",
        woltaDayKey
      );
      continue;
    }

    Serial.printf(
      "Forecast day %s -> cache[%d] (%u points)\n",
      dayDate ? dayDate : "(null)",
      slot,
      (unsigned)points.size()
    );

    for (JsonObject point : points) {
      JsonVariantConst tsValue = point["ts_utc"];
      JsonVariantConst oreValue = point["ore_kwh"];

      const char* ts = tsValue.as<const char*>();
      if (!ts || oreValue.isNull()) continue;

      float ore = oreValue.as<float>();

      time_t epoch = 0;
      if (!parseIso8601(ts, epoch)) {
        ++rejectedTimestamp;
        continue;
      }

      int q = quarterIndexFromLocal(epoch);
      if (q < 0 || q >= QUARTERS_PER_DAY) {
        ++rejectedQuarter;
        continue;
      }

      if (!loggedFirstPoint) {
        tm local{};
        localtime_r(&epoch, &local);

        Serial.printf(
          "First Wolta point: %s -> local %04d-%02d-%02d %02d:%02d, q=%d, %.2f ore/kWh\n",
          ts,
          local.tm_year + 1900,
          local.tm_mon + 1,
          local.tm_mday,
          local.tm_hour,
          local.tm_min,
          q,
          ore
        );

        loggedFirstPoint = true;
      }

      temp[slot].price[q] = applyVat(ore / 100.0f, includeVat);
      temp[slot].valid[q] = true;
      ++accepted;
    }
  }

  Serial.printf(
    "Forecast parse summary: accepted=%d rejectedDay=%d rejectedTimestamp=%d rejectedQuarter=%d\n",
    accepted,
    rejectedDay,
    rejectedTimestamp,
    rejectedQuarter
  );

  if (accepted == 0) {
    Serial.printf(
      "No usable forecast points (days=%u, base=%d)\n",
      (unsigned)days.size(),
      dateKey(baseLocalMidnight)
    );

    if (days.size() > 0) {
      Serial.print("First forecast day: ");
      serializeJson(days[0], Serial);
      Serial.println();
    }

    return false;
  }

  Serial.printf("Forecast accepted total: %d points\n", accepted);

  for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
    destination[d] = temp[d];
    Serial.printf("Forecast cache +%d day (%d): %d points\n",
                  d, destination[d].dateKey, validCount(destination[d]));
  }

  return true;
}

void PriceSources::build72Hours(
    const QuarterDay& actualToday,
    const QuarterDay& actualTomorrow,
    const QuarterDay forecast[FORECAST_CACHE_DAYS],
    time_t baseLocalMidnight,
    HourPrice out[GRAPH_HOURS]) {
  int dayKeys[3] = {
    dateKey(baseLocalMidnight),
    dateKey(addLocalDays(baseLocalMidnight, 1)),
    dateKey(addLocalDays(baseLocalMidnight, 2))
  };

  for (int i = 0; i < GRAPH_HOURS; ++i) {
    out[i] = HourPrice{};

    int dayOffset = i / 24;
    int hour = i % 24;
    const QuarterDay* actual = nullptr;

    if (dayOffset == 0 && actualToday.dateKey == dayKeys[0])
      actual = &actualToday;
    else if (dayOffset == 1 && actualTomorrow.dateKey == dayKeys[1])
      actual = &actualTomorrow;

    if (actual) {
      bool complete = false;
      float p = hourlyAverage(*actual, hour, complete);
      if (!isnan(p) && complete) {
        out[i].price = p;
        out[i].kind = PriceKind::Actual;
        continue;
      }
    }

    const QuarterDay* fc = nullptr;
    for (int d = 0; d < FORECAST_CACHE_DAYS; ++d) {
      if (forecast[d].dateKey == dayKeys[dayOffset]) {
        fc = &forecast[d];
        break;
      }
    }

    if (fc) {
      bool complete = false;
      float p = hourlyAverage(*fc, hour, complete);
      if (!isnan(p) && complete) {
        out[i].price = p;
        out[i].kind = PriceKind::Forecast;
      }
    }
  }
}

bool PriceSources::currentQuarterPrice(const QuarterDay& actualToday,
                                       time_t now,
                                       float& outPrice) {
  int key = dateKey(now);
  if (actualToday.dateKey != key) return false;

  int q = quarterIndexFromLocal(now);
  if (q < 0 || q >= QUARTERS_PER_DAY || !actualToday.valid[q]) return false;

  outPrice = actualToday.price[q];
  return true;
}

PriceStats PriceSources::calculateStats(
    const QuarterDay& actualToday,
    const HourPrice hours[GRAPH_HOURS]) {
  PriceStats s{};

  float todaySum = 0.0f;
  int todayCount = 0;

  for (int q = 0; q < QUARTERS_PER_DAY; ++q) {
    if (!actualToday.valid[q]) continue;

    float p = actualToday.price[q];
    todaySum += p;
    ++todayCount;

    if (isnan(s.minToday) || p < s.minToday) {
      s.minToday = p;
      s.minTodayQuarter = q;
    }

    if (isnan(s.maxToday) || p > s.maxToday) {
      s.maxToday = p;
      s.maxTodayQuarter = q;
    }
  }

  if (todayCount > 0) s.avgToday = todaySum / todayCount;

  float sum72 = 0.0f;
  int count72 = 0;
  for (int i = 0; i < GRAPH_HOURS; ++i) {
    if (hours[i].kind == PriceKind::Missing || isnan(hours[i].price)) continue;
    sum72 += hours[i].price;
    ++count72;
  }
  if (count72 > 0) s.avg72 = sum72 / count72;

  return s;
}
