#include "periph.h"

/* BK4819: bit-banged over GPIOC, not a DP32G030 MMIO peripheral. See
   dp32g030_bk4819_gpio_edge (hooked from dp32g030_gpio_write) and the
   struct comment in DP32G030.h. */

#define BK4819_PIN_SCN (1u << 0)
#define BK4819_PIN_SCL (1u << 1)
#define BK4819_PIN_SDA (1u << 2)

#define REG_02_SQUELCH_LOST (1u << 2)
#define REG_02_SQUELCH_FOUND (1u << 3)

static void bk4819_reset_transaction(dp32g030_bk4819_t *b) {
  b->selected = false;
  b->addr_phase = false;
  b->reading = false;
  b->bitpos = 0;
  b->addr = 0;
  b->shift = 0;
}

static void bk4819_update_squelch(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  uint16_t open_thresh = (b->regs[0x78] >> 8) & 0xFFu;
  uint16_t close_thresh = b->regs[0x78] & 0xFFu;
  uint16_t rssi = b->regs[0x67] & 0x01FFu;
  bool was_open = b->squelch_open;
  bool now_open = was_open ? rssi >= close_thresh : rssi >= open_thresh;

  if (now_open == was_open)
    return;
  b->squelch_open = now_open;
  b->regs[0x02] |= now_open ? REG_02_SQUELCH_FOUND : REG_02_SQUELCH_LOST;
}

/* Apply side effects for a register that was just written by the host
   protocol (address `addr`, 16-bit value already stored in regs[addr]). */
static void bk4819_commit_write(dp32g030_cpu_t *c, uint8_t addr) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  switch (addr) {
  case 0x38: case 0x39: {
    uint32_t freq = (uint32_t)b->regs[0x38] | ((uint32_t)b->regs[0x39] << 16);
    const dp32g030_hooks_t *h = &c->periph.hooks;

    if (h->bk4819_freq_change)
      h->bk4819_freq_change(h->user, freq);
    break;
  }
  case 0x78:
    bk4819_update_squelch(c);
    break;
  default:
    break;
  }
}

/* Called after every GPIOC DATA/DIR write, with the port's effective
   pin levels (dp32g030_gpio_pins) and direction before and after. */
void dp32g030_bk4819_gpio_edge(dp32g030_cpu_t *c, uint32_t before,
                               uint32_t after, uint32_t dir) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  bool scn, scl, sda_in;

  (void)before;
  if ((dir & (BK4819_PIN_SCN | BK4819_PIN_SCL)) !=
      (BK4819_PIN_SCN | BK4819_PIN_SCL)) {
    /* SCN/SCL not driven as outputs: not wired up (yet), or firmware
       hasn't configured the pins. Nothing to decode. */
    bk4819_reset_transaction(b);
    b->prev_scn = b->prev_scl = b->prev_sda_in = false;
    return;
  }

  scn = (after & BK4819_PIN_SCN) != 0;
  scl = (after & BK4819_PIN_SCL) != 0;
  sda_in = !(dir & BK4819_PIN_SDA); /* SDA currently an input (host reading) */

  if (b->prev_scn && !scn) {
    /* Falling edge on chip select: transaction starts. */
    bk4819_reset_transaction(b);
    b->selected = true;
    b->addr_phase = true;
  } else if (!b->prev_scn && scn) {
    /* Rising edge: transaction ends, whether or not it finished cleanly. */
    bk4819_reset_transaction(b);
  } else if (b->selected) {
    if (!b->prev_scl && scl && !sda_in) {
      /* Rising clock edge while the host drives SDA: sample the bit
         (address/command byte, or a 16-bit write). */
      bool bit = (after & BK4819_PIN_SDA) != 0;

      b->shift = (uint16_t)((b->shift << 1) | (bit ? 1u : 0u));
      b->bitpos++;
      if (b->addr_phase && b->bitpos == 8) {
        b->addr = (uint8_t)(b->shift & 0x7Fu);
        b->reading = (b->shift & 0x80u) != 0;
        b->addr_phase = false;
        b->bitpos = 0;
        b->shift = 0;
      } else if (!b->addr_phase && !b->reading && b->bitpos == 16) {
        b->regs[b->addr] = b->shift;
        bk4819_commit_write(c, b->addr);
      }
    } else if (!b->addr_phase && b->reading) {
      if (!b->prev_sda_in && sda_in) {
        /* The host just switched SDA to input: snapshot the register
           (so it reads consistently even if it changes mid-stream) and
           present the first (MSB) response bit before it samples. */
        b->shift = b->regs[b->addr];
        if (b->addr == 0x02)
          b->regs[0x02] &= (uint16_t)~(REG_02_SQUELCH_FOUND | REG_02_SQUELCH_LOST);
        b->bitpos = 0;
        dp32g030_gpio_set_input(c, 2, BK4819_PIN_SDA,
                                (b->shift & 0x8000u) ? BK4819_PIN_SDA : 0);
      } else if (sda_in && b->prev_scl && !scl && b->bitpos < 15) {
        /* Falling clock edge: the host just sampled; queue the next bit. */
        uint32_t bit;

        b->bitpos++;
        bit = (b->shift >> (15 - b->bitpos)) & 1u;
        dp32g030_gpio_set_input(c, 2, BK4819_PIN_SDA, bit ? BK4819_PIN_SDA : 0);
      }
    }
  }
  b->prev_scn = scn;
  b->prev_scl = scl;
  b->prev_sda_in = sda_in;
}

void dp32g030_bk4819_set_signal(dp32g030_cpu_t *c, uint16_t rssi,
                                uint8_t glitch, uint8_t noise) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  b->regs[0x67] = rssi & 0x01FFu;
  b->regs[0x63] = glitch;
  b->regs[0x65] = noise & 0x7Fu;
  bk4819_update_squelch(c);
}

uint32_t dp32g030_bk4819_frequency(const dp32g030_cpu_t *c) {
  const dp32g030_bk4819_t *b = &c->periph.bk4819;

  return (uint32_t)b->regs[0x38] | ((uint32_t)b->regs[0x39] << 16);
}

bool dp32g030_bk4819_squelch_open(const dp32g030_cpu_t *c) {
  return c->periph.bk4819.squelch_open;
}
