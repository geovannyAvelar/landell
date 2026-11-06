#include "lcd_view.h"

#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

namespace {
constexpr int kLcdWidth = 128;
constexpr int kLcdHeight = 64;
// The controller has 132 segments but the UV-K5 glass uses 128 of them, wired
// right to left: screen column x shows segment (kLcdWidth - 1 - x). With SEG
// reverse (what the firmware sends) that is RAM column 4 + x.

// Lit glass, and the same glass with the light off (still readable, as the
// real transflective LCD is in daylight).
constexpr LcdPalette kLit = {
    /*off*/ SkColorSetRGB(0x2B, 0x2F, 0x2A),
    /*background*/ SkColorSetRGB(0xB4, 0xC9, 0x9A),
    /*pixel_on*/ SkColorSetRGB(0x1B, 0x26, 0x14),
    /*pixel_off*/ SkColorSetRGB(0xA8, 0xBF, 0x8C),
};
constexpr LcdPalette kUnlit = {
    /*off*/ SkColorSetRGB(0x2B, 0x2F, 0x2A),
    /*background*/ SkColorSetRGB(0x66, 0x70, 0x5C),
    /*pixel_on*/ SkColorSetRGB(0x12, 0x18, 0x0E),
    /*pixel_off*/ SkColorSetRGB(0x5C, 0x67, 0x53),
};

SkColor mix(SkColor a, SkColor b, float t) {
  auto ch = [&](int shift) {
    float x = float((a >> shift) & 0xFF) * (1.0f - t) +
              float((b >> shift) & 0xFF) * t;
    return uint32_t(x + 0.5f) & 0xFF;
  };
  return SkColorSetRGB(ch(16), ch(8), ch(0));
}
} // namespace

LcdPalette lcd_palette(float backlight) {
  float t = backlight < 0.0f ? 0.0f : backlight > 1.0f ? 1.0f : backlight;

  return {kLit.off, mix(kUnlit.background, kLit.background, t),
          mix(kUnlit.pixel_on, kLit.pixel_on, t),
          mix(kUnlit.pixel_off, kLit.pixel_off, t)};
}

void lcd_view_draw(const dp32g030_cpu_t *cpu, SkCanvas *canvas, int scale,
                   float backlight) {
  const LcdPalette kPalette = lcd_palette(backlight);

  if (!dp32g030_lcd_is_on(cpu)) {
    canvas->clear(kPalette.off);
    return;
  }

  canvas->clear(kPalette.background);
  const int gap = scale >= 3 ? 1 : 0;
  const float size = float(scale - gap);
  SkPaint paint;
  paint.setAntiAlias(false);

  for (int y = 0; y < kLcdHeight; y++) {
    for (int x = 0; x < kLcdWidth; x++) {
      paint.setColor(dp32g030_lcd_get_pixel(cpu, unsigned(kLcdWidth - 1 - x), unsigned(y))
                         ? kPalette.pixel_on
                         : kPalette.pixel_off);
      canvas->drawRect(SkRect::MakeXYWH(float(x * scale), float(y * scale),
                                        size, size),
                       paint);
    }
  }
}
