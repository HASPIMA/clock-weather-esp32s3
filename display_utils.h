// display_utils.h - Drawing helpers for the weather screen.
#pragma once

#include <Adafruit_GFX.h>
#include "weather.h"

// Prints "22.8°C" at the current cursor; the degree sign is drawn as a small circle.
// size must match the current text size.
void printTemp(Adafruit_GFX &gfx, float t, uint8_t size);

// Line chart of the hourly temperatures, with a marker on currentHour (-1 for none).
void drawHourlyChart(
  Adafruit_GFX &gfx,
  const WeatherData &w,
  int currentHour,
  int16_t x0,
  int16_t y0,
  int16_t width,
  int16_t height);
