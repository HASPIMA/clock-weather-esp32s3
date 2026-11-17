#include "wifi_utils.h"

#include <WiFi.h>

// The ESP32 only works on 2.4 GHz.
// Credentials are read from wifi.txt at compile time (git-ignored; see wifi.example.txt)
#if !__has_include("wifi.txt")
#error "wifi.txt not found: copy wifi.example.txt to wifi.txt and fill in your network credentials"
#endif
static const char* const wifiCredentials[] = {
#include "wifi.txt"
};
static_assert(
  sizeof(wifiCredentials) / sizeof(wifiCredentials[0]) == 2,
  "wifi.txt must contain exactly two strings: \"SSID\", \"password\"");
static const char* const ssid = wifiCredentials[0];
static const char* const password = wifiCredentials[1];

bool wifiConnect(const char* tag, uint32_t timeoutMs) {
  Serial.printf("%s Starting Wi-Fi connection...\n", tag);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      Serial.printf("\n%s WiFi connection timed out.\n", tag);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.printf("\n%s WiFi Connected.\n", tag);
  return true;
}

void wifiOff(const char* tag) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("%s WiFi turned off.\n", tag);
}
