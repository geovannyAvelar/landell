#pragma once

#include "keypad.h"

#include "include/core/SkCanvas.h"

#include <optional>
#include <vector>

// Panel height in LCD-pixel units; the panel is drawn below the 64-unit LCD
// and is 128 units wide, like the LCD.
constexpr int kPanelUnits = 72;
constexpr int kLcdUnitsHigh = 64;

struct KeyRect {
  Key key;
  float x, y, w, h; // logical units, origin at the panel's top-left
};

const std::vector<KeyRect> &panel_keys();

// Window-pixel point (origin at the window's top-left, so the LCD occupies the
// first 64*scale rows) to the key under it, if any.
std::optional<Key> panel_hit(float px, float py, int scale);

// Draws the panel into the canvas area below the LCD.
void panel_draw(SkCanvas *canvas, int scale, const Keypad &keypad);
