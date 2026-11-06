#include "periph.h"

#include "socdesc.h"

/* The backlight is a PWM of a GPIO pin; what the person sees is its average.
   The pin level is accounted over windows of WINDOW core clocks (~17 ms at
   48 MHz, many PWM periods) and the last closed window gives the level. */

#define WINDOW 800000u

static void advance(dp32g030_backlight_t *b, uint64_t now) {
  if (now < b->last_cycle)
    now = b->last_cycle;
  while (now - b->win_start >= WINDOW) {
    uint64_t end = b->win_start + WINDOW;

    if (b->level && end > b->last_cycle)
      b->high_acc += end - b->last_cycle;
    b->duty = (uint8_t)(b->high_acc * 256u / WINDOW > 255u
                            ? 255u
                            : b->high_acc * 256u / WINDOW);
    b->have = true;
    b->win_start = end;
    b->high_acc = 0;
    if (b->last_cycle < end)
      b->last_cycle = end;
  }
  if (b->level)
    b->high_acc += now - b->last_cycle;
  b->last_cycle = now;
}

void dp32g030_backlight_pin(dp32g030_cpu_t *c, bool level) {
  advance(&c->backlight, c->cycles);
  c->backlight.level = level;
}

void dp32g030_backlight_reset(dp32g030_cpu_t *c) {
  dp32g030_backlight_pin(c, false);
}

bool dp32g030_has_backlight(const dp32g030_cpu_t *c) {
  return c->soc->backlight.present;
}

uint8_t dp32g030_backlight_level(dp32g030_cpu_t *c) {
  dp32g030_backlight_t *b = &c->backlight;

  advance(b, c->cycles);
  if (b->have)
    return b->duty;
  uint64_t elapsed = c->cycles - b->win_start;

  if (elapsed == 0)
    return b->level ? 255 : 0;
  return (uint8_t)(b->high_acc * 255u / elapsed);
}
