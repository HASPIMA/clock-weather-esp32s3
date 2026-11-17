#include "time.h"
#include "wifi_utils.h"
#include "weather.h"
#include "display_utils.h"

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
// WiFi credentials live in wifi.txt (see wifi_utils.cpp)

// Since wifi is global we may need to protect its access.
// Maybe this won't be necessary in the future if we do it in parallel
SemaphoreHandle_t wifiMutex;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15 * 1000;


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

// --- Shared weather data (written by the weather task, read by the display task) ---
WeatherData weather;
SemaphoreHandle_t weatherMutex;

// --- FreeRTOS Task Handles ---
TaskHandle_t syncTimeTaskHandle = NULL;
TaskHandle_t weatherTaskHandle = NULL;

// --- Task Prototypes ---
void taskSyncTime(void* pvParameters);
void taskFetchWeather(void* pvParameters);
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

  // Keep retrying: the clock is useless without a first sync
  while (!wifiConnect(tag, WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.printf("%s Retrying...\n", tag);
  }

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
  wifiOff(tag);

  // Always release after disconnecting (We may not need to disconnect at all)
  Serial.printf("%s Releasing WiFi mutex\n", tag);
  xSemaphoreGive(wifiMutex);

  // SUSPEND TASK: The task enters a suspended state.
  Serial.printf("%s Task Suspending now...\n", tag);
  vTaskSuspend(NULL);

  // This part of the code is never reached while suspended
  for (;;) { vTaskDelay(1000); }
}

// --- TASK: FETCH WEATHER PERIODICALLY ---
void taskFetchWeather(void* pvParameters) {
  const char* tag = "[Weather]";

  for (;;) {
    Serial.printf("%s Waiting for WiFi mutex...\n", tag);
    xSemaphoreTake(wifiMutex, portMAX_DELAY);

    WeatherData fresh;
    bool ok = wifiConnect(tag, WIFI_CONNECT_TIMEOUT_MS) && fetchWeather(tag, weather_url, fresh);
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
        printTemp(display, w.currentTemp, 2);

        float tMin, tMax;
        if (hourlyMinMax(w, tMin, tMax)) {
          display.setTextSize(1);
          display.setCursor(86, 30);
          display.write(0x18);  // up arrow (classic GFX font)
          display.printf("%.1f", tMax);
          display.setCursor(86, 39);
          display.write(0x19);  // down arrow
          display.printf("%.1f", tMin);
        }

        // Row 4: hourly chart
        drawHourlyChart(display, w, haveTime ? timeinfo.tm_hour : -1, 0, 49, SCREEN_WIDTH, 15);
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
