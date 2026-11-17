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

// --- WiFi/NTP Configuration ---
// The ESP32 only works on 2.4 GHz
const char* ssid = "WIFI4614";
const char* password = "hrey4868+";

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

  // Pantalla de inicio: logo con barra de carga (solo si hay OLED)
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
  Serial.println("[Sync] Starting Wi-Fi connection...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    // Serial.printf("%d_%d", WiFi.status(), WiFi.isConnected());
    Serial.print(".");
  }
  Serial.println("\n[Sync] WiFi Connected.");

  // Configure the ESP32 internal system time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("[Sync] Synchronizing with NTP server...");

  struct tm timeinfo;
  // Wait until time is actually retrieved from the network
  while (!getLocalTime(&timeinfo)) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    Serial.println("[Sync] Waiting for NTP response...");
  }

  Serial.println("[Sync] System Time Synchronized!");

  // Disconnect WiFi to save power after sync
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[Sync] WiFi turned off.");

  // SUSPEND TASK: The task enters a suspended state.
  Serial.println("[Sync] Task Suspending now...");
  vTaskSuspend(NULL);

  // This part of the code is never reached while suspended
  for (;;) { vTaskDelay(1000); }
}

// --- TASK: DISPLAY TIME IN SPANISH ---
void taskDisplayTimeAndWeather(void* pvParameters) {
  struct tm timeinfo;

  // Spanish Arrays
  const char* dias[] = { "Domingo", "Lunes", "Martes", "Miércoles", "Jueves", "Viernes", "Sábado" };
  const char* meses[] = { "Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                          "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre" };

  for (;;) {
    // getLocalTime accesses the internal RTC which keeps running even if Sync Task is suspended
    if (getLocalTime(&timeinfo)) {

      if (!oledOk) {
        Serial.printf("Fecha: %s, %02d de %s de %04d | Hora: %02d:%02d:%02d\n",
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
      Serial.println("[Display] Waiting for system clock to be set...");
    }

    // Refresh display every 1 second
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
