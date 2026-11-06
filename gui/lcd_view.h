#pragma once

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

extern "C" {
#include "DP32G030.h"
}

struct LcdPalette {
  SkColor off;        // display switched off
  SkColor background; // gaps between pixels
  SkColor pixel_on;
  SkColor pixel_off;
};

const LcdPalette &lcd_palette();

// Draws the LCD into a (128 * scale) x (64 * scale) area of `canvas`.
void lcd_view_draw(const dp32g030_cpu_t *cpu, SkCanvas *canvas, int scale);
