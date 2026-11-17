#include "display_utils.h"

// Monochrome "on" color (SSD1306_WHITE), kept generic to avoid depending on the driver
static const uint16_t WHITE = 1;

void printTemp(Adafruit_GFX &gfx, float t, uint8_t size) {
  gfx.print(t, 1);
  int16_t x = gfx.getCursorX() + size;
  int16_t y = gfx.getCursorY() + size;
  gfx.drawCircle(x + size, y + size, size, WHITE);
  gfx.setCursor(x + 3 * size + 1, gfx.getCursorY());
  gfx.print("C");
}

void drawHourlyChart(
  Adafruit_GFX &gfx,
  const WeatherData &w,
  int currentHour,
  int16_t x0,
  int16_t y0,
  int16_t width,
  int16_t height) {
  float tMin, tMax;
  if (w.hourlyCount < 2 || !hourlyMinMax(w, tMin, tMax))
    return;
  float range = (tMax - tMin) < 0.1f ? 0.1f : (tMax - tMin);

  auto px = [&](uint8_t i) {
    return (int16_t)(x0 + (int32_t)i * (width - 1) / (w.hourlyCount - 1));
  };
  auto py = [&](uint8_t i) {
    return (int16_t)(y0 + height - 1 - (w.hourly[i] - tMin) * (height - 1) / range);
  };

  for (uint8_t i = 1; i < w.hourlyCount; i++) {
    gfx.drawLine(px(i - 1), py(i - 1), px(i), py(i), WHITE);
  }

  if (currentHour >= 0 && currentHour < w.hourlyCount) {
    int16_t cx = px(currentHour);
    for (int16_t y = y0; y < y0 + height; y += 2)
      gfx.drawPixel(cx, y, WHITE);
    gfx.fillCircle(cx, py(currentHour), 2, WHITE);
  }
}
