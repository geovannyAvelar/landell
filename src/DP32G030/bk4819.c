#include "periph.h"

/* BK4819: bit-banged over GPIOC, not a DP32G030 MMIO peripheral. See
   dp32g030_bk4819_gpio_edge (hooked from dp32g030_gpio_write) and the
   struct comment in DP32G030.h. */

#define BK4819_PIN_SCN (1u << 0)
#define BK4819_PIN_SCL (1u << 1)
#define BK4819_PIN_SDA (1u << 2)

#define REG_02_SQUELCH_LOST (1u << 2)
#define REG_02_SQUELCH_FOUND (1u << 3)
#define REG_02_CTCSS_LOST (1u << 6)
#define REG_02_CTCSS_FOUND (1u << 7)
#define REG_02_CDCSS_LOST (1u << 8)
#define REG_02_CDCSS_FOUND (1u << 9)

#define REG_07_MODE_SHIFT 13
#define REG_07_MODE_CTC1 0u
#define REG_07_MODE_CDCSS 2u
#define REG_07_FREQ_MASK 0x1FFFu

#define REG_51_CDCSS_24BIT (1u << 11)

#define REG_02_DTMF_5TONE_FOUND (1u << 11)
#define REG_24_ENABLE (1u << 5)
#define REG_24_SELECT_DTMF (1u << 4)

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

/* Inverse of the firmware's control_word = round(freq_Hz * 20.64888);
   returns freq in 0.1 Hz units, rounded. */
static uint32_t ctc1_word_to_tenth_hz(uint16_t word) {
  return ((uint32_t)word * 100000u + 103244u) / 206488u;
}

static void bk4819_update_ctcss(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  unsigned mode = (b->regs[0x07] >> REG_07_MODE_SHIFT) & 7u;
  bool found = false;

  if (mode == REG_07_MODE_CTC1 && b->ctcss_tone_in != 0) {
    uint32_t target = ctc1_word_to_tenth_hz(b->regs[0x07] & REG_07_FREQ_MASK);
    uint32_t diff = target > b->ctcss_tone_in ? target - b->ctcss_tone_in
                                              : b->ctcss_tone_in - target;

    found = diff <= 10; /* +/- 1 Hz tolerance for the rounding above */
  }
  if (found == b->ctcss_found)
    return;
  b->ctcss_found = found;
  b->regs[0x02] |= found ? REG_02_CTCSS_FOUND : REG_02_CTCSS_LOST;
}

static void bk4819_update_cdcss(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  unsigned mode = (b->regs[0x07] >> REG_07_MODE_SHIFT) & 7u;
  uint32_t mask = (b->regs[0x51] & REG_51_CDCSS_24BIT) ? 0xFFFFFFu : 0x7FFFFFu;
  uint32_t configured = b->cdcss_low | ((uint32_t)b->cdcss_high << 12);
  bool found = false;

  if (mode == REG_07_MODE_CDCSS && b->cdcss_present_in)
    found = (configured & mask) == (b->cdcss_code_in & mask);
  if (found == b->cdcss_found)
    return;
  b->cdcss_found = found;
  b->regs[0x02] |= found ? REG_02_CDCSS_FOUND : REG_02_CDCSS_LOST;
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
  case 0x07:
    bk4819_update_ctcss(c);
    bk4819_update_cdcss(c);
    break;
  case 0x08:
    if (b->regs[0x08] & 0x8000u)
      b->cdcss_high = b->regs[0x08] & 0x0FFFu;
    else
      b->cdcss_low = b->regs[0x08] & 0x0FFFu;
    bk4819_update_cdcss(c);
    break;
  case 0x51:
    bk4819_update_cdcss(c);
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
          b->regs[0x02] &= (uint16_t)~(
              REG_02_SQUELCH_FOUND | REG_02_SQUELCH_LOST | REG_02_CTCSS_FOUND |
              REG_02_CTCSS_LOST | REG_02_CDCSS_FOUND | REG_02_CDCSS_LOST |
              REG_02_DTMF_5TONE_FOUND);
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

void dp32g030_bk4819_set_ctcss_tone(dp32g030_cpu_t *c, uint32_t tenth_hz) {
  c->periph.bk4819.ctcss_tone_in = tenth_hz;
  bk4819_update_ctcss(c);
}

void dp32g030_bk4819_set_cdcss_code(dp32g030_cpu_t *c, uint32_t code,
                                    bool present) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  b->cdcss_code_in = code;
  b->cdcss_present_in = present;
  bk4819_update_cdcss(c);
}

bool dp32g030_bk4819_signal_dtmf(dp32g030_cpu_t *c, char digit) {
  static const char keys[16] = "0123456789ABCD*#";
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  unsigned code;

  if ((b->regs[0x24] & (REG_24_ENABLE | REG_24_SELECT_DTMF)) !=
      (REG_24_ENABLE | REG_24_SELECT_DTMF))
    return false; /* the 5-tone decoder isn't enabled for DTMF */
  for (code = 0; code < 16; code++)
    if (keys[code] == digit)
      break;
  if (code == 16)
    return false;
  b->regs[0x0B] = (uint16_t)((b->regs[0x0B] & ~0x0F00u) | (code << 8));
  b->regs[0x02] |= REG_02_DTMF_5TONE_FOUND;
  return true;
}
