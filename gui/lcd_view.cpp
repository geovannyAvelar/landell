#include "lcd_view.h"

#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

namespace {
constexpr int kLcdWidth = 128;
constexpr int kLcdHeight = 64;

const LcdPalette kPalette = {
    /*off*/ SkColorSetRGB(0x2B, 0x2F, 0x2A),
    /*background*/ SkColorSetRGB(0xB4, 0xC9, 0x9A),
    /*pixel_on*/ SkColorSetRGB(0x1B, 0x26, 0x14),
    /*pixel_off*/ SkColorSetRGB(0xA8, 0xBF, 0x8C),
};
} // namespace

const LcdPalette &lcd_palette() { return kPalette; }

void lcd_view_draw(const dp32g030_cpu_t *cpu, SkCanvas *canvas, int scale) {
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
      paint.setColor(dp32g030_lcd_get_pixel(cpu, unsigned(x), unsigned(y))
                         ? kPalette.pixel_on
                         : kPalette.pixel_off);
      canvas->drawRect(SkRect::MakeXYWH(float(x * scale), float(y * scale),
                                        size, size),
                       paint);
    }
  }
}
