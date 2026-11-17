#include <WiFi.h>
#include "time.h"

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
const char* ssid = "WIFI4614";
const char* password = "nxfw9794(";

// Since wifi is global we may need to protect its access.
// Maybe this won't be necessary in the future if we do it in parallel
SemaphoreHandle_t wifiMutex;


// Time settings: GMT-5, No DST
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600;  // -18000 seconds
const int daylightOffset_sec = 0;      // No daylight saving time

// --- FreeRTOS Task Handles ---
TaskHandle_t syncTimeTaskHandle = NULL;

// --- Task Prototypes ---
void taskSyncTime(void* pvParameters);
void taskDisplayTimeAndWeather(void* pvParameters);

void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  oledOk = initOled();
  Serial.println(oledOk ? F("OLED detectada en 0x3C")
                        : F("OLED no responde (revisa SDA=GPIO8, SCL=GPIO9, VCC, GND); sigo por serial"));

  // Create Mutex for Wifi
  wifiMutex = xSemaphoreCreateMutex();

  if (wifiMutex == NULL) {
    Serial.println("Error creating WiFi mutex!");
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

  // 2. Create Display Task (Lower priority, Core 0)
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

// --- TASK: DISPLAY TIME IN SPANISH ---
void taskDisplayTimeAndWeather(void* pvParameters) {
  struct tm timeinfo;
  const char* tag = "[Display]";

  // Spanish Arrays
  const char* dias[] = { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" };
  const char* meses[] = { "Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                          "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre" };

  for (;;) {
    // getLocalTime accesses the internal RTC which keeps running even if Sync Task is suspended
    if (getLocalTime(&timeinfo)) {

      if (!oledOk) {
        Serial.printf("%s Fecha: %s, %02d de %s de %04d | Hora: %02d:%02d:%02d\n",
                      tag,
                      dias[timeinfo.tm_wday],
                      timeinfo.tm_mday,
                      meses[timeinfo.tm_mon],
                      timeinfo.tm_year + 1900,
                      timeinfo.tm_hour,
                      timeinfo.tm_min,
                      timeinfo.tm_sec);
      } else {
        // TODO: Display in screen
      }
    } else {
      Serial.printf("%s Waiting for system clock to be set...\n", tag);
    }

    // Refresh display every 1 second
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
