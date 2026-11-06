#include "panel.h"

#include "glyphs.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"

#include <cstring>

namespace {

// Layout in logical units (panel is 128 wide). Left column: side keys and PTT.
// Right block: MENU/UP/DOWN/EXIT row, then the 3x4 digit grid.
std::vector<KeyRect> build_layout() {
  std::vector<KeyRect> v;

  v.push_back({Key::Side1, 2, 4, 24, 14});
  v.push_back({Key::Side2, 2, 20, 24, 14});
  v.push_back({Key::Ptt, 2, 38, 24, 26});

  const Key top[4] = {Key::Menu, Key::Up, Key::Down, Key::Exit};
  for (int i = 0; i < 4; i++)
    v.push_back({top[i], 32.0f + 24.0f * float(i), 2, 22, 12});

  const Key grid[4][3] = {
      {Key::K1, Key::K2, Key::K3},
      {Key::K4, Key::K5, Key::K6},
      {Key::K7, Key::K8, Key::K9},
      {Key::Star, Key::K0, Key::F},
  };
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 3; c++)
      v.push_back({grid[r][c], 32.0f + 32.0f * float(c),
                   16.0f + 14.0f * float(r), 30, 12});
  return v;
}

const char *label_for(Key k) {
  switch (k) {
  case Key::Menu: return "MENU";
  case Key::Exit: return "EXIT";
  case Key::Ptt: return "PTT";
  case Key::Side1: return "S1";
  case Key::Side2: return "S2";
  case Key::Star: return "*";
  case Key::F: return "#";
  case Key::Up:
  case Key::Down: return "";
  default: {
    static char digits[10][2];
    int d = int(k) - int(Key::K0);
    digits[d][0] = char('0' + d);
    digits[d][1] = '\0';
    return digits[d];
  }
  }
}

constexpr SkColor kBody = SkColorSetRGB(0x22, 0x26, 0x2B);
constexpr SkColor kButton = SkColorSetRGB(0x8A, 0x90, 0x99);
constexpr SkColor kButtonDown = SkColorSetRGB(0x4A, 0x4F, 0x56);
constexpr SkColor kLabel = SkColorSetRGB(0x10, 0x12, 0x14);
constexpr SkColor kLabelDown = SkColorSetRGB(0xE6, 0xE8, 0xEA);

void draw_label(SkCanvas *canvas, const KeyRect &r, int scale, SkColor color) {
  const char *text = label_for(r.key);
  size_t n = std::strlen(text);
  SkPaint paint;
  paint.setColor(color);
  paint.setAntiAlias(false);

  if (n == 0) { // UP / DOWN arrows
    float cx = (r.x + r.w / 2) * float(scale);
    float cy = (r.y + r.h / 2) * float(scale);
    float s = 3.0f * float(scale);
    SkPathBuilder path;
    if (r.key == Key::Up) {
      path.moveTo(cx, cy - s);
      path.lineTo(cx + s, cy + s);
      path.lineTo(cx - s, cy + s);
    } else {
      path.moveTo(cx, cy + s);
      path.lineTo(cx + s, cy - s);
      path.lineTo(cx - s, cy - s);
    }
    path.close();
    paint.setAntiAlias(true);
    canvas->drawPath(path.detach(), paint);
    return;
  }

  const float unit = n > 2 ? 0.75f : 1.0f; // glyph pixel size, logical units
  const float text_w = (6.0f * float(n) - 1.0f) * unit;
  float x0 = r.x + (r.w - text_w) / 2;
  float y0 = r.y + (r.h - 7.0f * unit) / 2;
  for (size_t i = 0; i < n; i++) {
    uint8_t rows[7];
    if (!glyph_rows(text[i], rows))
      continue;
    for (int gy = 0; gy < 7; gy++)
      for (int gx = 0; gx < 5; gx++)
        if (rows[gy] & (0x10 >> gx))
          canvas->drawRect(
              SkRect::MakeXYWH((x0 + (6.0f * float(i) + float(gx)) * unit) * float(scale),
                               (y0 + float(gy) * unit) * float(scale),
                               unit * float(scale), unit * float(scale)),
              paint);
  }
}

} // namespace

const std::vector<KeyRect> &panel_keys() {
  static const std::vector<KeyRect> keys = build_layout();
  return keys;
}

std::optional<Key> panel_hit(float px, float py, int scale) {
  float x = px / float(scale);
  float y = py / float(scale) - float(kLcdUnitsHigh);
  for (const KeyRect &r : panel_keys())
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
      return r.key;
  return std::nullopt;
}

void panel_draw(SkCanvas *canvas, int scale, const Keypad &keypad) {
  SkPaint paint;
  paint.setColor(kBody);
  canvas->drawRect(SkRect::MakeXYWH(0, float(kLcdUnitsHigh * scale),
                                    float(128 * scale),
                                    float(kPanelUnits * scale)),
                   paint);

  canvas->save();
  canvas->translate(0, float(kLcdUnitsHigh * scale));
  paint.setAntiAlias(true);
  for (const KeyRect &r : panel_keys()) {
    bool down = keypad.pressed(r.key);
    paint.setColor(down ? kButtonDown : kButton);
    SkRect rect = SkRect::MakeXYWH(r.x * float(scale), r.y * float(scale),
                                   r.w * float(scale), r.h * float(scale));
    float radius = 2.0f * float(scale);
    canvas->drawRRect(SkRRect::MakeRectXY(rect, radius, radius), paint);
    draw_label(canvas, r, scale, down ? kLabelDown : kLabel);
  }
  canvas->restore();
}
