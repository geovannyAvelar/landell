#include "panel.h"
#include "glyphs.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <cstdio>
#include <cstring>

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static bool overlap(const KeyRect &a, const KeyRect &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
         b.y < a.y + a.h;
}

int main() {
  const auto &keys = panel_keys();

  { // every_key_has_rect
    int seen[int(Key::Count)] = {};
    for (const KeyRect &r : keys)
      seen[int(r.key)]++;
    for (int i = 0; i < int(Key::Count); i++)
      CHECK(seen[i] == 1);
  }
  for (size_t i = 0; i < keys.size(); i++) {
    const KeyRect &r = keys[i];
    CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 128 && r.y + r.h <= kPanelUnits); // inside_panel
    for (size_t j = i + 1; j < keys.size(); j++)
      CHECK(!overlap(r, keys[j])); // no_overlap
  }

  for (int scale : {1, 6}) { // hit_center
    for (const KeyRect &r : keys) {
      float px = (r.x + r.w / 2) * scale;
      float py = (kLcdUnitsHigh + r.y + r.h / 2) * scale;
      auto hit = panel_hit(px, py, scale);
      CHECK(hit.has_value() && *hit == r.key);
    }
  }

  { // hit_outside
    CHECK(!panel_hit(10, 10, 6));                         // on the LCD
    CHECK(!panel_hit(-1, kLcdUnitsHigh * 6 + 1, 6));      // left of the window
    CHECK(!panel_hit(5, (kLcdUnitsHigh + kPanelUnits) * 6 + 1, 6)); // below
    // gap between two buttons of the first row
    const KeyRect &a = keys[0];
    float gap_x = (a.x + a.w + 0.5f) * 6;
    float y = (kLcdUnitsHigh + a.y + a.h / 2) * 6;
    bool found_gap = false;
    for (const KeyRect &b : keys)
      if (b.x > a.x + a.w && b.y == a.y)
        found_gap = true;
    if (found_gap)
      CHECK(!panel_hit(gap_x, y, 6));
  }

  { // glyphs_for_labels
    const char *all = "0123456789*#MENUXITPS";
    for (const char *p = all; *p; p++) {
      uint8_t rows[7];
      CHECK(glyph_rows(*p, rows));
    }
    uint8_t rows[7];
    CHECK(!glyph_rows('~', rows));
  }

  { // draw_pressed_differs
    const int scale = 6;
    dp32g030_cpu_t *cpu = [] {
      const uint8_t image[8] = {0, 0x40, 0, 0x20, 9, 0, 0, 0};
      return dp32g030_cpu_new(image, sizeof image);
    }();
    Keypad pad(cpu);
    auto render = [&](SkBitmap &bm) {
      SkImageInfo info = SkImageInfo::MakeN32Premul(
          128 * scale, (kLcdUnitsHigh + kPanelUnits) * scale);
      bm.allocPixels(info);
      auto surface =
          SkSurfaces::WrapPixels(info, bm.getPixels(), bm.rowBytes());
      surface->getCanvas()->clear(SK_ColorBLACK);
      panel_draw(surface->getCanvas(), scale, pad);
    };
    auto corner = [&](const SkBitmap &bm, Key k) {
      for (const KeyRect &r : keys)
        if (r.key == k)
          return bm.getColor(int((r.x + 1.5f) * scale),
                             int((kLcdUnitsHigh + r.y + 1.5f) * scale));
      return SkColor(0);
    };
    SkBitmap before, after;
    render(before);
    pad.set(Key::Menu, true);
    render(after);
    CHECK(corner(before, Key::Menu) != corner(after, Key::Menu));
    CHECK(corner(before, Key::K5) == corner(after, Key::K5));
    CHECK(corner(before, Key::K5) != SK_ColorBLACK); // panel was drawn at all
    dp32g030_cpu_free(cpu);
  }

  return failures ? 1 : 0;
}
