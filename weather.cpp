#include "weather.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>  // Library Manager: "ArduinoJson" by Benoit Blanchon (v7)

bool fetchWeather(const char* tag, const String& url, WeatherData& out) {
  WiFiClientSecure client;
  // No CA bundle configured: skip certificate validation (fine for public, read-only weather data)
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) {
    Serial.printf("%s http.begin() failed\n", tag);
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("%s HTTP GET failed: %d (%s)\n", tag, code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  // Only keep the fields we use; everything else is discarded while parsing
  JsonDocument filter;
  filter["current"]["time"] = true;
  filter["current"]["temperature_2m"] = true;
  filter["hourly"]["temperature_2m"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("%s JSON parse error: %s\n", tag, err.c_str());
    return false;
  }

  JsonVariantConst current = doc["current"]["temperature_2m"];
  JsonArrayConst hourly = doc["hourly"]["temperature_2m"];
  if (!current.is<float>() || hourly.isNull()) {
    Serial.printf("%s Unexpected JSON shape\n", tag);
    return false;
  }

  out.currentTemp = current.as<float>();
  strlcpy(out.currentTime, doc["current"]["time"] | "", sizeof(out.currentTime));
  out.hourlyCount = 0;
  for (JsonVariantConst t : hourly) {
    if (out.hourlyCount >= HOURLY_MAX) break;
    out.hourly[out.hourlyCount++] = t.as<float>();
  }
  out.valid = true;
  return true;
}

bool hourlyMinMax(const WeatherData& w, float& tMin, float& tMax) {
  if (w.hourlyCount == 0) return false;
  tMin = tMax = w.hourly[0];
  for (uint8_t i = 1; i < w.hourlyCount; i++) {
    tMin = min(tMin, w.hourly[i]);
    tMax = max(tMax, w.hourly[i]);
  }
  return true;
}
