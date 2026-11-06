#include "periph.h"

#include "socdesc.h"

/* PY32F071 GPIO: STM32F0-style ports. Modes per pin: 0 input, 1 output,
   2 alternate function, 3 analog. IDR of an input or alternate pin follows
   the outside world (dp32g030_gpio_set_input) and falls back to the pull
   resistors; an analog pin reads 0; a released open-drain output reads high
   (the board has an external pull-up). */

#define PORT_A_MODER_RESET 0xEBFFFFFFu
#define MODER_ANALOG 0xFFFFFFFFu

static uint32_t mode_mask(uint32_t moder, unsigned mode) {
  uint32_t m = 0;

  for (unsigned p = 0; p < 16; p++)
    if (((moder >> (2 * p)) & 3u) == mode)
      m |= 1u << p;
  return m;
}

static uint32_t pull_mask(uint32_t pupdr, unsigned pull) {
  uint32_t m = 0;

  for (unsigned p = 0; p < 16; p++)
    if (((pupdr >> (2 * p)) & 3u) == pull)
      m |= 1u << p;
  return m;
}

static uint32_t py32_gpio_level(const dp32g030_py32_gpio_t *g) {
  uint32_t out = mode_mask(g->moder, 1);
  uint32_t analog = mode_mask(g->moder, 3);
  uint32_t open_drain = out & g->otyper;
  uint32_t released = open_drain & g->odr;
  uint32_t input = ~out & ~analog;
  uint32_t pulled_up = pull_mask(g->pupdr, 1);
  uint32_t level = 0;

  level |= out & ~open_drain & g->odr;
  level |= released; /* an open-drain driven low adds nothing */
  level |= input & ((g->in & g->driven) | (pulled_up & ~g->driven));
  return level & 0xFFFFu;
}

static void py32_gpio_notify(dp32g030_cpu_t *c, unsigned port,
                             uint32_t before) {
  const dp32g030_hooks_t *h = &c->periph.hooks;
  const dp32g030_py32_gpio_t *g = &c->py32.gpio[port];
  uint32_t now = py32_gpio_level(g);

  if (now != before && h->gpio_change)
    h->gpio_change(h->user, port, now, mode_mask(g->moder, 1));
}

void py32_gpio_reset(dp32g030_cpu_t *c) {
  for (unsigned i = 0; i < PY32_NGPIO; i++) {
    dp32g030_py32_gpio_t *g = &c->py32.gpio[i];

    *g = (dp32g030_py32_gpio_t){0};
    g->moder = i == 0 ? PORT_A_MODER_RESET : MODER_ANALOG;
  }
}

uint32_t py32_gpio_pins(const dp32g030_cpu_t *c, unsigned port) {
  return port < PY32_NGPIO ? py32_gpio_level(&c->py32.gpio[port]) : 0;
}

void py32_gpio_set_input(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                         uint32_t value) {
  if (port >= PY32_NGPIO)
    return;
  dp32g030_py32_gpio_t *g = &c->py32.gpio[port];
  uint32_t before = py32_gpio_level(g);

  g->in = (g->in & ~mask) | (value & mask);
  g->driven |= mask;
  py32_gpio_notify(c, port, before);
}

bool py32_gpio_read(dp32g030_cpu_t *c, unsigned port, uint32_t off,
                    unsigned size, uint32_t *out) {
  const dp32g030_py32_gpio_t *g = &c->py32.gpio[port];
  uint32_t v;

  switch (off & ~3u) {
  case 0x00: v = g->moder; break;
  case 0x04: v = g->otyper; break;
  case 0x08: v = g->ospeedr; break;
  case 0x0C: v = g->pupdr; break;
  case 0x10: v = py32_gpio_level(g); break;
  case 0x14: v = g->odr; break;
  case 0x18: v = 0; break; /* BSRR is write-only */
  case 0x1C: v = g->lckr; break;
  case 0x20: v = g->afr[0]; break;
  case 0x24: v = g->afr[1]; break;
  case 0x28: v = 0; break; /* BRR is write-only */
  default: return false;
  }
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (v >> shift) & mask;
  return true;
}

/* The UV-K5 V3 / K1 wires the BK4819 serial link to CSN = PF9, SCL = PB8,
   SDA = PB9. */
#define BK_PORT_B 1u
#define BK_PORT_F 3u
#define BK_SCL (1u << 8)
#define BK_SDA (1u << 9)
#define BK_CSN (1u << 9)

void py32_bk4819_drive_sda(dp32g030_cpu_t *c, bool level) {
  py32_gpio_set_input(c, BK_PORT_B, BK_SDA, level ? BK_SDA : 0);
}

static void py32_bk4819_edge(dp32g030_cpu_t *c) {
  const dp32g030_py32_gpio_t *b = &c->py32.gpio[BK_PORT_B];
  const dp32g030_py32_gpio_t *f = &c->py32.gpio[BK_PORT_F];
  uint32_t b_level = py32_gpio_level(b);
  uint32_t out_b = mode_mask(b->moder, 1);
  uint32_t out_f = mode_mask(f->moder, 1);

  dp32g030_bk4819_pins(c, (py32_gpio_level(f) & BK_CSN) != 0,
                       (b_level & BK_SCL) != 0, (b_level & BK_SDA) != 0,
                       (out_f & BK_CSN) && (out_b & BK_SCL),
                       !(out_b & BK_SDA));
}

bool py32_gpio_write(dp32g030_cpu_t *c, unsigned port, uint32_t off,
                     unsigned size, uint32_t val) {
  dp32g030_py32_gpio_t *g = &c->py32.gpio[port];
  uint32_t before = py32_gpio_level(g);
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  uint32_t *reg = NULL;

  switch (off & ~3u) {
  case 0x00: reg = &g->moder; break;
  case 0x04: reg = &g->otyper; break;
  case 0x08: reg = &g->ospeedr; break;
  case 0x0C: reg = &g->pupdr; break;
  case 0x10: return true; /* IDR is read-only */
  case 0x14: reg = &g->odr; break;
  case 0x18: { /* BSRR: low half sets, high half resets and wins */
    uint32_t v = (val & mask) << shift;
    g->odr |= v & 0xFFFFu;
    g->odr &= ~(v >> 16);
    break;
  }
  case 0x1C: reg = &g->lckr; break;
  case 0x20: reg = &g->afr[0]; break;
  case 0x24: reg = &g->afr[1]; break;
  case 0x28: g->odr &= ~(((val & mask) << shift) & 0xFFFFu); break;
  default: return false;
  }
  if (reg)
    *reg = (*reg & ~(mask << shift)) | ((val & mask) << shift);
  py32_gpio_notify(c, port, before);
  if (port == BK_PORT_B || port == BK_PORT_F)
    py32_bk4819_edge(c);
  if (c->soc->extflash.present && port == c->soc->extflash.cs_port)
    dp32g030_extflash_cs(c);
  return true;
}
