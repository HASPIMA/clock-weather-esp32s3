#include <WiFi.h>
#include "time.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>  // Library Manager: "ArduinoJson" by Benoit Blanchon (v7)

// --- Screen ---
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "driver/pulse_cnt.h"
#include "splash.h"  // Logo + barra de carga al iniciar

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define I2C_SDA 8
#define I2C_SCL 9

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOk = false;

// Make sure display is available
bool initOled() {
  Wire.beginTransmission(SCREEN_ADDRESS);
  if (Wire.endTransmission() != 0) return false;
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) return false;
  display.clearDisplay();
  display.display();
  return true;
}

// --- Networking Configuration ---
// The ESP32 only works on 2.4 GHz
// Credentials are read from wifi.txt at compile time (git-ignored; see wifi.example.txt)
#if !__has_include("wifi.txt")
#error "wifi.txt not found: copy wifi.example.txt to wifi.txt and fill in your network credentials"
#endif
const char* const wifiCredentials[] = {
#include "wifi.txt"
};
static_assert(sizeof(wifiCredentials) / sizeof(wifiCredentials[0]) == 2,
              "wifi.txt must contain exactly two strings: \"SSID\", \"password\"");
const char* ssid = wifiCredentials[0];
const char* password = wifiCredentials[1];

// Since wifi is global we may need to protect its access.
// Maybe this won't be necessary in the future if we do it in parallel
SemaphoreHandle_t wifiMutex;


// Time settings: GMT-5, No DST
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600;  // -18000 seconds
const int daylightOffset_sec = 0;      // No daylight saving time

// Weather endpoint settings
const float latitude = 4.60;
const float longitude = -74.08;
const String host_name = "https://api.open-meteo.com";
const String endpoint = "/v1/forecast";
const String query_string = String("latitude=") + String(latitude) + String("&longitude=") + String(longitude) + String("&hourly=temperature_2m&current=temperature_2m&timezone=auto&forecast_days=1");
const String weather_url = host_name + endpoint + "?" + query_string;

const uint32_t WEATHER_REFRESH_MS = 15 * 60 * 1000;  // Open-Meteo "current" updates every 900 s
const uint32_t WEATHER_RETRY_MS = 30 * 1000;         // Retry sooner after a failure
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15 * 1000;

// --- Shared weather data (written by the weather task, read by the display task) ---
#define HOURLY_MAX 24

struct WeatherData {
  bool valid = false;
  float currentTemp = NAN;
  char currentTime[17] = "";  // "YYYY-MM-DDTHH:MM"
  float hourly[HOURLY_MAX];
  uint8_t hourlyCount = 0;
};

WeatherData weather;
SemaphoreHandle_t weatherMutex;

// --- FreeRTOS Task Handles ---
TaskHandle_t syncTimeTaskHandle = NULL;
TaskHandle_t weatherTaskHandle = NULL;

// --- Task Prototypes ---
void taskSyncTime(void* pvParameters);
void taskFetchWeather(void* pvParameters);

// Explicit prototypes: the Arduino preprocessor would otherwise place them before WeatherData is declared
bool fetchWeather(const char* tag, WeatherData& out);
void drawHourlyChart(const WeatherData& w, int currentHour, int16_t x0, int16_t y0, int16_t width, int16_t height);
void taskDisplayTimeAndWeather(void* pvParameters);

void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  oledOk = initOled();
  Serial.println(oledOk ? F("OLED detected at 0x3C")
                        : F("OLED unavailable (check SDA=GPIO8, SCL=GPIO9, VCC, GND); Serial will be used instead"));

  // Create Mutex for Wifi
  wifiMutex = xSemaphoreCreateMutex();
  weatherMutex = xSemaphoreCreateMutex();

  if (wifiMutex == NULL || weatherMutex == NULL) {
    Serial.println("Error creating mutexes!");
    Serial.println("STOPPING!");
    return;
  }

  // Splash screen
  if (oledOk) showSplash(display);


  // 1. Create Synchronization Task (Higher priority, Core 1)
  xTaskCreatePinnedToCore(
    taskSyncTime,         // Function
    "SyncTimeTask",       // Name
    4096,                 // Stack size
    NULL,                 // Parameters
    2,                    // Priority
    &syncTimeTaskHandle,  // Handle (assigned here)
    1                     // Core
  );

  // 2. Create Weather Task (Core 1, next to the other WiFi user).
  //    TLS handshake + JSON parsing need a bigger stack.
  xTaskCreatePinnedToCore(
    taskFetchWeather,
    "WeatherTask",
    10240,
    NULL,
    1,
    &weatherTaskHandle,
    1);

  // 3. Create Display Task (Lower priority, Core 0)
  xTaskCreatePinnedToCore(
    taskDisplayTimeAndWeather,
    "DisplayTimeTask",
    4096,
    NULL,
    1,
    NULL,
    0);
}

void loop() {
  // FreeRTOS handles everything. The loop task can be deleted.
  vTaskDelete(NULL);
}

// --- TASK: CONNECT WIFI & SYNC NTP ---
void taskSyncTime(void* pvParameters) {
  const char* tag = "[SyncTime]";

  while (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(100)) == pdFALSE) {
    Serial.printf("%s Could not acquire mutex to write\n", tag);
  }

  Serial.printf("%s Starting Wi-Fi connection...\n", tag);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial.print(".");
  }
  Serial.printf("\n%s WiFi Connected.\n", tag);

  // Configure the ESP32 internal system time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.printf("%s Synchronizing with NTP server...\n", tag);

  struct tm timeinfo;
  // Wait until time is actually retrieved from the network
  while (!getLocalTime(&timeinfo)) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    Serial.printf("%s Waiting for NTP response...\n", tag);
  }

  Serial.printf("%s System Time Synchronized!\n", tag);

  // Disconnect WiFi to save power after sync
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("%s WiFi turned off.\n", tag);

  // Always release after disconnecting (We may not need to disconnect at all)
  Serial.printf("%s Releasing WiFi mutex\n", tag);
  xSemaphoreGive(wifiMutex);

  // SUSPEND TASK: The task enters a suspended state.
  Serial.printf("%s Task Suspending now...\n", tag);
  vTaskSuspend(NULL);

  // This part of the code is never reached while suspended
  for (;;) { vTaskDelay(1000); }
}

// --- WEATHER HELPERS ---
// Caller must hold wifiMutex.
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

// Caller must hold wifiMutex.
void wifiOff(const char* tag) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("%s WiFi turned off.\n", tag);
}

// Downloads and parses the forecast. Caller must hold wifiMutex and be connected.
bool fetchWeather(const char* tag, WeatherData& out) {
  WiFiClientSecure client;
  // No CA bundle configured: skip certificate validation (fine for public, read-only weather data)
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, weather_url)) {
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

// --- TASK: FETCH WEATHER PERIODICALLY ---
void taskFetchWeather(void* pvParameters) {
  const char* tag = "[Weather]";

  for (;;) {
    Serial.printf("%s Waiting for WiFi mutex...\n", tag);
    xSemaphoreTake(wifiMutex, portMAX_DELAY);

    WeatherData fresh;
    bool ok = wifiConnect(tag, WIFI_CONNECT_TIMEOUT_MS) && fetchWeather(tag, fresh);
    wifiOff(tag);

    Serial.printf("%s Releasing WiFi mutex\n", tag);
    xSemaphoreGive(wifiMutex);

    if (ok) {
      xSemaphoreTake(weatherMutex, portMAX_DELAY);
      weather = fresh;
      xSemaphoreGive(weatherMutex);
      Serial.printf("%s Updated: %.1f C at %s (%u hourly values)\n",
                    tag, fresh.currentTemp, fresh.currentTime, fresh.hourlyCount);
    }

    vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS));
  }
}

// --- DISPLAY HELPERS ---
// Prints "22.8°C" at the current cursor; the degree sign is drawn as a small circle.
void printTemp(float t, uint8_t size) {
  display.print(t, 1);
  int16_t x = display.getCursorX() + size;
  int16_t y = display.getCursorY() + size;
  display.drawCircle(x + size, y + size, size, SSD1306_WHITE);
  display.setCursor(x + 3 * size + 1, display.getCursorY());
  display.print("C");
}

// Line chart of the hourly temperatures, with a marker on the current hour.
void drawHourlyChart(const WeatherData& w, int currentHour, int16_t x0, int16_t y0, int16_t width, int16_t height) {
  if (w.hourlyCount < 2) return;

  float tMin = w.hourly[0], tMax = w.hourly[0];
  for (uint8_t i = 1; i < w.hourlyCount; i++) {
    tMin = min(tMin, w.hourly[i]);
    tMax = max(tMax, w.hourly[i]);
  }
  float range = (tMax - tMin) < 0.1f ? 0.1f : (tMax - tMin);

  auto px = [&](uint8_t i) {
    return (int16_t)(x0 + (int32_t)i * (width - 1) / (w.hourlyCount - 1));
  };
  auto py = [&](uint8_t i) {
    return (int16_t)(y0 + height - 1 - (w.hourly[i] - tMin) * (height - 1) / range);
  };

  for (uint8_t i = 1; i < w.hourlyCount; i++) {
    display.drawLine(px(i - 1), py(i - 1), px(i), py(i), SSD1306_WHITE);
  }

  if (currentHour >= 0 && currentHour < w.hourlyCount) {
    int16_t cx = px(currentHour);
    for (int16_t y = y0; y < y0 + height; y += 2) display.drawPixel(cx, y, SSD1306_WHITE);
    display.fillCircle(cx, py(currentHour), 2, SSD1306_WHITE);
  }
}

// --- TASK: DISPLAY TIME IN SPANISH ---
void taskDisplayTimeAndWeather(void* pvParameters) {
  struct tm timeinfo;
  const char* tag = "[Display]";

  // Spanish Arrays
  const char* dias[] = { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" };
  const char* meses[] = { "Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                          "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre" };

  // Short ASCII names for the OLED (the default GFX font has no accented letters)
  const char* diasCortos[] = { "Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab" };
  const char* mesesCortos[] = { "Ene", "Feb", "Mar", "Abr", "May", "Jun",
                                "Jul", "Ago", "Sep", "Oct", "Nov", "Dic" };

  for (;;) {
    // Take a snapshot of the weather so we don't hold the mutex while drawing
    WeatherData w;
    xSemaphoreTake(weatherMutex, portMAX_DELAY);
    w = weather;
    xSemaphoreGive(weatherMutex);

    // getLocalTime accesses the internal RTC which keeps running even if Sync Task is suspended
    bool haveTime = getLocalTime(&timeinfo, 0);

    if (!oledOk) {
      if (haveTime) {
        Serial.printf("%s Fecha: %s, %02d de %s de %04d | Hora: %02d:%02d:%02d",
                      tag,
                      dias[timeinfo.tm_wday],
                      timeinfo.tm_mday,
                      meses[timeinfo.tm_mon],
                      timeinfo.tm_year + 1900,
                      timeinfo.tm_hour,
                      timeinfo.tm_min,
                      timeinfo.tm_sec);
      } else {
        Serial.printf("%s Waiting for system clock to be set...", tag);
      }

      if (w.valid) {
        Serial.printf(" | Temp: %.1f C", w.currentTemp);
        if (haveTime && timeinfo.tm_hour < w.hourlyCount) {
          Serial.printf(" (pronostico %02d:00: %.1f C)", timeinfo.tm_hour, w.hourly[timeinfo.tm_hour]);
        }
      } else {
        Serial.print(" | Temp: --");
      }
      Serial.println();
    } else {
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);

      if (haveTime) {
        // Row 1: date
        display.setCursor(0, 0);
        display.printf("%s %02d %s %04d", diasCortos[timeinfo.tm_wday], timeinfo.tm_mday,
                       mesesCortos[timeinfo.tm_mon], timeinfo.tm_year + 1900);

        // Row 2: time, big and centered (8 chars * 12 px = 96 px)
        display.setTextSize(2);
        display.setCursor((SCREEN_WIDTH - 96) / 2, 11);
        display.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      } else {
        display.setCursor(0, 0);
        display.print("Sincronizando hora...");
      }

      // Row 3: current temperature + day's max/min
      display.setTextSize(1);
      if (w.valid) {
        display.setTextSize(2);
        display.setCursor(0, 30);
        printTemp(w.currentTemp, 2);

        if (w.hourlyCount > 0) {
          float tMin = w.hourly[0], tMax = w.hourly[0];
          for (uint8_t i = 1; i < w.hourlyCount; i++) {
            tMin = min(tMin, w.hourly[i]);
            tMax = max(tMax, w.hourly[i]);
          }
          display.setTextSize(1);
          display.setCursor(86, 30);
          display.write(0x18);  // up arrow (classic GFX font)
          display.printf("%.1f", tMax);
          display.setCursor(86, 39);
          display.write(0x19);  // down arrow
          display.printf("%.1f", tMin);
        }

        // Row 4: hourly chart
        drawHourlyChart(w, haveTime ? timeinfo.tm_hour : -1, 0, 49, SCREEN_WIDTH, 15);
      } else {
        display.setCursor(0, 34);
        display.print("Clima: cargando...");
      }

      display.display();
    }

    // Refresh display every 1 second
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
