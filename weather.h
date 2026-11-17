// weather.h - Open-Meteo forecast download and parsing.
#pragma once

#include <Arduino.h>

#define HOURLY_MAX 24

struct WeatherData {
  bool valid = false;
  float currentTemp = NAN;
  char currentTime[17] = "";  // "YYYY-MM-DDTHH:MM"
  float hourly[HOURLY_MAX];
  uint8_t hourlyCount = 0;
};

// Downloads and parses the forecast at url into out.
// Caller must hold wifiMutex and be connected.
bool fetchWeather(const char* tag, const String& url, WeatherData& out);

// Lowest and highest hourly temperature. Returns false if there are no hourly values.
bool hourlyMinMax(const WeatherData& w, float& tMin, float& tMax);
