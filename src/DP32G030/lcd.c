#include "periph.h"

/* ST7565-compatible LCD controller, driven over SPI0 (see
   dp32g030_lcd_spi_byte, hooked from dp32g030_spi_write) plus GPIOB
   pin 9 (A0) to tell a command byte from a data byte. Command set is
   the subset the UV-K5's driver actually sends. */

#define LCD_PAGES 8
#define LCD_COLS 132

static void lcd_notify(dp32g030_cpu_t *c) {
  const dp32g030_hooks_t *h = &c->periph.hooks;

  if (h->lcd_update)
    h->lcd_update(h->user);
}

static void lcd_command(dp32g030_cpu_t *c, uint8_t b) {
  dp32g030_lcd_t *l = &c->periph.lcd;

  if (l->pending_contrast) {
    l->contrast = b & 0x3Fu;
    l->pending_contrast = false;
    return;
  }
  if (b == 0xE2) { /* software reset */
    dp32g030_lcd_t cleared = {0};

    *l = cleared;
    return;
  }
  if (b == 0x81) { /* SET_EV: next byte is the contrast level */
    l->pending_contrast = true;
    return;
  }
  if ((b & 0xF0u) == 0xB0u) { /* set page address, 0-7 */
    l->page = b & 0x07u;
    return;
  }
  if ((b & 0xF0u) == 0x10u) { /* set column, upper nibble */
    l->col = (uint8_t)((l->col & 0x0Fu) | ((b & 0x0Fu) << 4));
    return;
  }
  if ((b & 0xF0u) == 0x00u) { /* set column, lower nibble */
    l->col = (uint8_t)((l->col & 0xF0u) | (b & 0x0Fu));
    return;
  }
  if ((b & 0xC0u) == 0x40u) { /* set display start line, 0-63 */
    l->start_line = b & 0x3Fu;
    return;
  }
  if ((b & 0xFEu) == 0xAEu) { /* display on/off */
    l->display_on = (b & 1u) != 0;
    return;
  }
  if ((b & 0xFEu) == 0xA0u) { /* SEG (column) direction */
    l->seg_reverse = (b & 1u) != 0;
    return;
  }
  if ((b & 0xF7u) == 0xC0u) { /* COM (row) direction */
    l->com_reverse = (b & 0x08u) != 0;
    return;
  }
  if ((b & 0xFEu) == 0xA6u) { /* inverse display */
    l->inverse = (b & 1u) != 0;
    return;
  }
  if ((b & 0xFEu) == 0xA4u) { /* all pixels on */
    l->all_on = (b & 1u) != 0;
    return;
  }
  /* Bias select (0xA2/0xA3), regulation ratio (0x20-0x27) and the
     power circuit (0x28-0x2F) have no visible effect in this model. */
}

void dp32g030_lcd_spi_byte(dp32g030_cpu_t *c, uint8_t byte, bool data) {
  dp32g030_lcd_t *l = &c->periph.lcd;

  if (!data) {
    lcd_command(c, byte);
  } else {
    l->ram[l->page][l->col] = byte;
    l->col = (uint8_t)((l->col + 1) % LCD_COLS);
  }
  lcd_notify(c);
}

uint8_t dp32g030_lcd_ram(const dp32g030_cpu_t *c, unsigned page, unsigned col) {
  if (page >= LCD_PAGES || col >= LCD_COLS)
    return 0;
  return c->periph.lcd.ram[page][col];
}

bool dp32g030_lcd_is_on(const dp32g030_cpu_t *c) {
  return c->periph.lcd.display_on;
}

bool dp32g030_lcd_get_pixel(const dp32g030_cpu_t *c, unsigned x, unsigned y) {
  const dp32g030_lcd_t *l = &c->periph.lcd;
  unsigned ram_col, ram_row, logical_row, page, bit;
  bool pixel;

  if (!l->display_on || x >= LCD_COLS || y >= 64)
    return false;
  if (l->all_on)
    return true;
  ram_col = l->seg_reverse ? (LCD_COLS - 1 - x) : x;
  ram_row = l->com_reverse ? (63u - y) : y;
  logical_row = (ram_row + l->start_line) % 64u;
  page = logical_row / 8u;
  bit = logical_row % 8u;
  pixel = ((l->ram[page][ram_col] >> bit) & 1u) != 0;
  return l->inverse ? !pixel : pixel;
}
