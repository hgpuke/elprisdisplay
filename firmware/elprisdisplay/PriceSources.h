#pragma once
#include <Arduino.h>

constexpr int QUARTERS_PER_DAY = 96;
constexpr int FORECAST_CACHE_DAYS = 4;
constexpr int GRAPH_HOURS = 72;

enum class PriceKind : uint8_t {
  Missing = 0,
  Forecast = 1,
  Actual = 2
};

struct QuarterDay {
  int dateKey;  // YYYYMMDD in Europe/Stockholm
  float price[QUARTERS_PER_DAY];
  bool valid[QUARTERS_PER_DAY];
};

struct HourPrice {
  float price = NAN;
  PriceKind kind = PriceKind::Missing;
};

struct PriceStats {
  float minToday = NAN;
  float maxToday = NAN;
  float avgToday = NAN;
  float avg72 = NAN;
  int minTodayQuarter = -1;
  int maxTodayQuarter = -1;
};

class PriceSources {
public:
  static void clearDay(QuarterDay& day, int dateKey);
  static int validCount(const QuarterDay& day);
  static bool isComplete(const QuarterDay& day);

  static bool fetchActualDay(const char* zone,
                             time_t localMidnight,
                             bool includeVat,
                             QuarterDay& destination);

  static bool fetchForecast(const char* zone,
                            time_t baseLocalMidnight,
                            bool includeVat,
                            QuarterDay destination[FORECAST_CACHE_DAYS]);

  static void build72Hours(const QuarterDay& actualToday,
                           const QuarterDay& actualTomorrow,
                           const QuarterDay forecast[FORECAST_CACHE_DAYS],
                           time_t baseLocalMidnight,
                           HourPrice out[GRAPH_HOURS]);

  static bool currentQuarterPrice(const QuarterDay& actualToday,
                                  time_t now,
                                  float& outPrice);

  static PriceStats calculateStats(const QuarterDay& actualToday,
                                   const HourPrice hours[GRAPH_HOURS]);

  static int dateKey(time_t localEpoch);
  static time_t localMidnight(time_t epoch);
  static time_t addLocalDays(time_t localMidnightEpoch, int days);
  static bool parseIso8601(const char* s, time_t& epochUtc);
};
