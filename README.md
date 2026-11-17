# Clock and Weather display

This displays Bogotá's current datetime and weather information

## Table of contents

- [Clock and Weather display](#clock-and-weather-display)
  - [Table of contents](#table-of-contents)
  - [Hardware](#hardware)
  - [Dependencies](#dependencies)
  - [WiFi credentials](#wifi-credentials)
  - [Building and flashing](#building-and-flashing)
  - [Configuration](#configuration)
  - [How it works](#how-it-works)
    - [Source files](#source-files)
    - [Tasks](#tasks)
    - [Display layout](#display-layout)
    - [Serial fallback](#serial-fallback)

## Hardware

- ESP32-S3 mini
- SSD1306 128x64 OLED display (I2C, address `0x3C`), optional

| OLED | ESP32-S3 |
| ---- | -------- |
| SDA  | GPIO 8   |
| SCL  | GPIO 9   |
| VCC  | 3V3      |
| GND  | GND      |

The ESP32 only supports 2.4 GHz WiFi networks.

## Dependencies

> [!IMPORTANT]
> Install the libraries from the Arduino IDE Library Manager

- [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) (tested with 3.3.12), board `ESP32S3 Dev Module` (`esp32:esp32:esp32s3`)
- Libraries:
  - `Adafruit SSD1306`
  - `Adafruit GFX Library`
  - `ArduinoJson` by Benoit Blanchon (v7)

## WiFi credentials

> [!IMPORTANT]
> Credentials are not stored in the source code. They are read from
> `wifi.txt` at compile time (by `wifi_utils.cpp`), which is
> git-ignored.

```sh
cp wifi.example.txt wifi.txt
```

Then edit `wifi.txt`. It must contain two C string literals separated by a comma (escape any `"` or `\` with a backslash):

```c
"MY_SSID", "MY_PASSWORD"
```

If `wifi.txt` is missing, the build fails with an error explaining what to do.

## Building and flashing

Open `clock_weather.ino` in the Arduino IDE, select the board and port, and upload. Or with `arduino-cli`:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p /dev/ttyACM0 .
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

## Configuration

Constants at the top of `clock_weather.ino`:

| Constant                  | Default              | Description                                  |
| ------------------------- | -------------------- | -------------------------------------------- |
| `latitude` / `longitude`  | `4.60` / `-74.08`    | Location sent to the weather API (Bogotá)    |
| `gmtOffset_sec`           | `-18000` (GMT-5)     | Time zone offset for NTP                     |
| `ntpServer`               | `pool.ntp.org`       | NTP server                                   |
| `WEATHER_REFRESH_MS`      | 15 min               | Weather refresh interval after a success     |
| `WEATHER_RETRY_MS`        | 30 s                 | Retry interval after a failed fetch          |
| `WIFI_CONNECT_TIMEOUT_MS` | 15 s                 | WiFi connection timeout per attempt          |

Weather data comes from [Open-Meteo](https://open-meteo.com/) (no API key needed): the current temperature and today's hourly temperatures.

## How it works

### Source files

| File                                    | Contents                                                                  |
| --------------------------------------- | ------------------------------------------------------------------------- |
| `clock_weather.ino`                     | Configuration, setup, mutexes and the FreeRTOS tasks                      |
| `wifi_utils.h` / `wifi_utils.cpp`       | `wifiConnect()` / `wifiOff()` and the credentials from `wifi.txt`         |
| `weather.h` / `weather.cpp`             | `WeatherData`, `fetchWeather()` (HTTPS + JSON parsing), `hourlyMinMax()`  |
| `display_utils.h` / `display_utils.cpp` | `printTemp()` and `drawHourlyChart()` for the OLED                        |
| `splash.h`                              | Startup logo and loading bar                                              |

### Tasks

The sketch runs on FreeRTOS tasks; `loop()` deletes itself.

| Task              | Core | Description                                                                                                                                    |
| ----------------- | :--: | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `SyncTimeTask`    | 1    | Connects to WiFi (retrying until it succeeds), syncs the clock via NTP, turns WiFi off and suspends itself. The RTC keeps the time afterwards. |
| `WeatherTask`     | 1    | Periodically connects to WiFi, downloads the forecast over HTTPS, parses it, turns WiFi off and stores the result.                             |
| `DisplayTimeTask` | 0    | Every second, reads the clock and a copy of the latest weather data and draws it on the OLED (or prints it to Serial).                         |

Synchronization:

- `wifiMutex` ensures only one task uses WiFi at a time.
- `weatherMutex` protects the shared weather data between the weather and display tasks.

The HTTPS request skips certificate validation (`setInsecure()`), since the data is public and read-only.

### Display layout

```txt
Mar 17 Nov 2026
   13:45:07          <- large
22.8°C      ↑23.1    <- large temperature, day's max/min
            ↓13.4
/\/‾‾\__.__/         <- hourly temperature chart, current hour marked
```

Day and month names are abbreviated without accents because the default GFX font has no accented characters. Until data is available, the screen shows `Sincronizando hora...` / `Clima: cargando...`.

### Serial fallback

If no OLED is detected at startup, the same information is printed to Serial (115200 baud) every second:

```log
[Display] Fecha: Martes, 17 de Noviembre de 2026 | Hora: 13:45:07 | Temp: 22.8 C (pronostico 13:00: 23.0 C)
```
