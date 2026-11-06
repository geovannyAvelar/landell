#include "lcd_view.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static dp32g030_cpu_t *make_cpu() {
  const uint8_t image[8] = {0x00, 0x40, 0x00, 0x20, 0x09, 0x00, 0x00, 0x00};
  return dp32g030_cpu_new(image, sizeof image);
}

struct Frame {
  int scale;
  sk_sp<SkSurface> surface;
  SkBitmap bitmap;

  Frame(const dp32g030_cpu_t *cpu, int s, float backlight = 1.0f) : scale(s) {
    SkImageInfo info = SkImageInfo::MakeN32Premul(128 * s, 64 * s);
    bitmap.allocPixels(info);
    surface = SkSurfaces::WrapPixels(info, bitmap.getPixels(), bitmap.rowBytes());
    lcd_view_draw(cpu, surface->getCanvas(), s, backlight);
  }
  // Color at the centre of LCD pixel (x, y).
  SkColor center(int x, int y) const {
    return bitmap.getColor(x * scale + scale / 2, y * scale + scale / 2);
  }
  SkColor at(int px, int py) const { return bitmap.getColor(px, py); }
};

int main() {
  const LcdPalette &pal = lcd_palette();
  dp32g030_cpu_t *cpu = make_cpu();
  if (!cpu)
    return 1;
  cpu->periph.lcd.display_on = true;
  // Firmware setup (SEG reverse, as the UV-K5 driver sends 0xA1): the glass
  // shows RAM columns 4..131 left to right (segments 127..0).
  cpu->periph.lcd.seg_reverse = true;
  cpu->periph.lcd.ram[0][4] = 0x01; // pixel (0,0) on

  { // display_on_pixel_on_color / display_on_pixel_off_color
    Frame f(cpu, 6);
    CHECK(f.center(0, 0) == pal.pixel_on);
    CHECK(f.center(1, 0) == pal.pixel_off);
    // gap_between_pixels_is_background: last column of pixel (0,0)'s cell
    CHECK(f.at(5, 0) == pal.background);
    CHECK(f.at(0, 5) == pal.background);
  }
  { // scale_one_has_no_gap
    Frame f(cpu, 1);
    CHECK(f.center(0, 0) == pal.pixel_on);
    CHECK(f.center(1, 0) == pal.pixel_off);
  }
  { // glass_mounting_without_seg_reverse: RAM column 127 is on the left edge
    cpu->periph.lcd.seg_reverse = false;
    cpu->periph.lcd.ram[0][4] = 0x00;
    cpu->periph.lcd.ram[0][127] = 0x01;
    Frame f(cpu, 6);
    CHECK(f.center(0, 0) == pal.pixel_on);
    CHECK(f.center(127, 0) == pal.pixel_off);
    cpu->periph.lcd.ram[0][127] = 0x00;
    cpu->periph.lcd.ram[0][4] = 0x01;
    cpu->periph.lcd.seg_reverse = true;
  }
  { // hidden_ram_columns_not_drawn: RAM columns 0..3 are off the glass
    cpu->periph.lcd.ram[0][0] = 0xFF;
    Frame f(cpu, 6);
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 128; x++)
        if (x != 0 || y != 0)
          CHECK(f.center(x, y) == pal.pixel_off);
    cpu->periph.lcd.ram[0][0] = 0x00;
  }
  { // backlight_dims_the_glass: full backlight is the normal palette
    LcdPalette lit = lcd_palette(1.0f), dark = lcd_palette(0.0f), mid = lcd_palette(0.5f);
    CHECK(lit.background == pal.background);
    auto lum = [](SkColor c) { return SkColorGetR(c) + SkColorGetG(c) + SkColorGetB(c); };
    CHECK(lum(dark.background) < lum(lit.background));
    CHECK(lum(dark.pixel_off) < lum(lit.pixel_off));
    CHECK(lum(mid.background) > lum(dark.background));
    CHECK(lum(mid.background) < lum(lit.background));
    // the glass stays readable without light: pixels still contrast with the field
    CHECK(lum(dark.background) - lum(dark.pixel_on) > 150);
    CHECK(dark.off == lit.off); // a switched-off display is not affected

    Frame f(cpu, 6, 0.0f);
    CHECK(f.center(0, 0) == dark.pixel_on);
    CHECK(f.center(1, 0) == dark.pixel_off);
    CHECK(f.at(5, 0) == dark.background);
  }
  { // brightness_out_of_range_is_clamped
    LcdPalette lo = lcd_palette(-3.0f), hi = lcd_palette(7.0f);
    CHECK(lo.background == lcd_palette(0.0f).background);
    CHECK(hi.background == lcd_palette(1.0f).background);
  }
  { // display_off_is_flat
    cpu->periph.lcd.display_on = false;
    Frame f(cpu, 6);
    for (int y = 0; y < 64; y += 7)
      for (int x = 0; x < 128; x += 5)
        CHECK(f.center(x, y) == pal.off);
    CHECK(f.at(5, 0) == pal.off);
  }

  dp32g030_cpu_free(cpu);
  return failures ? 1 : 0;
}
