#include "periph.h"
#include "socdesc.h"

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

#define REG_02_VOX_LOST (1u << 4)
#define REG_02_VOX_FOUND (1u << 5)
#define REG_31_VOX_ENABLE (1u << 2)

#define REG_50_TX_MUTE (1u << 15)
#define REG_70_ENABLE_TONE1 (1u << 15)
#define REG_47_AF_SHIFT 8
#define REG_47_AF_MASK 0xFu
#define BK4819_AF_BEEP 3u
/* Inverse of firmware scale_freq: ((hz * 1353245) + (1<<16)) >> 17 */
#define TONE_SCALE_NUM 1353245u

/* REG_33: GPIOn is bit (6-n). GPIO1 = PA enable → bit 5. */
#define REG_33_PA_ENABLE (1u << 5)
#define REG_30_PA_GAIN (1u << 3)
#define REG_30_MIC_ADC (1u << 2)
#define REG_30_TX_DSP (1u << 1)
#define REG_30_TX_VOICE (REG_30_PA_GAIN | REG_30_MIC_ADC | REG_30_TX_DSP)
/* Firmware stores frequency in 10 Hz units in REG_38/REG_39. */
#define FREQ_HZ_PER_LSB 10u

static void bk4819_fsk_fifo_status(dp32g030_bk4819_t *b);
static void bk4819_fsk_fifo_pop(dp32g030_bk4819_t *b);
static void bk4819_update_tone(dp32g030_cpu_t *c);
static void bk4819_update_tx(dp32g030_cpu_t *c);

static bool bk4819_compute_tx_active(const dp32g030_bk4819_t *b) {
  if (!(b->regs[0x33] & REG_33_PA_ENABLE))
    return false;
  return (b->regs[0x30] & REG_30_TX_VOICE) == REG_30_TX_VOICE;
}

static uint32_t bk4819_tuned_hz(const dp32g030_bk4819_t *b) {
  uint32_t raw =
      (uint32_t)b->regs[0x38] | ((uint32_t)b->regs[0x39] << 16);
  return raw * FREQ_HZ_PER_LSB;
}

static void bk4819_update_tx(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  const dp32g030_hooks_t *h = &c->periph.hooks;
  bool active = bk4819_compute_tx_active(b);
  uint32_t hz = active ? bk4819_tuned_hz(b) : 0;

  if (active == b->tx_active && hz == b->tx_hz)
    return;
  b->tx_active = active;
  b->tx_hz = hz;
  if (h->bk4819_tx_change)
    h->bk4819_tx_change(h->user, active, hz);
}

static unsigned bk4819_unscale_tone(uint16_t reg71) {
  return (unsigned)(((uint64_t)reg71 << 17) / TONE_SCALE_NUM);
}

static unsigned bk4819_compute_tone_hz(const dp32g030_bk4819_t *b) {
  if (!(b->regs[0x70] & REG_70_ENABLE_TONE1))
    return 0;
  if (b->regs[0x50] & REG_50_TX_MUTE)
    return 0;
  if (((b->regs[0x47] >> REG_47_AF_SHIFT) & REG_47_AF_MASK) != BK4819_AF_BEEP)
    return 0;
  return bk4819_unscale_tone(b->regs[0x71]);
}

static void bk4819_update_tone(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  unsigned hz = bk4819_compute_tone_hz(b);
  const dp32g030_hooks_t *h = &c->periph.hooks;

  if (hz == b->tone_hz)
    return;
  b->tone_hz = hz;
  if (h->bk4819_tone_change)
    h->bk4819_tone_change(h->user, hz);
}

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

#define REG_32_SCAN_ENABLE (1u << 0)

#define REG_59_RX_FIFO_CLEAR (1u << 14)
#define REG_59_FSK_RX_ENABLE (1u << 12)
#define REG_02_FSK_RX_FINISHED (1u << 13)
#define REG_02_FSK_FIFO_ALMOST_FULL (1u << 12)
#define REG_02_FSK_FIFO_ALMOST_EMPTY (1u << 14)
#define FSK_FIFO_DEPTH 8u
#define FSK_FIFO_ALMOST_FULL_LEVEL 4u
#define REG_0D_NOT_FINISHED (1u << 15)

/* BK4819_GetFrequencyScanResult: while a scan is enabled (REG_32),
   reports the currently tuned frequency as "found" whenever the
   RSSI-threshold squelch is open (REG_0D/REG_0E), mirroring how the
   real firmware only ever starts a scan at one frequency at a time
   and relies on the chip's own RSSI/squelch to say whether that
   frequency is active. */
static void bk4819_update_scan(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  if (!(b->regs[0x32] & REG_32_SCAN_ENABLE) || !b->squelch_open) {
    b->regs[0x0D] |= REG_0D_NOT_FINISHED;
    return;
  }
  {
    uint32_t freq = (uint32_t)b->regs[0x38] | ((uint32_t)b->regs[0x39] << 16);

    b->regs[0x0D] = (uint16_t)((freq >> 16) & 0x7FFu);
    b->regs[0x0E] = (uint16_t)(freq & 0xFFFFu);
  }
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

static void bk4819_update_vox(dp32g030_cpu_t *c) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;
  uint16_t amp = b->regs[0x64] & 0x7FFFu;
  uint16_t enable_thresh = b->regs[0x46] & 0x07FFu;
  uint16_t disable_thresh = b->regs[0x79] & 0x07FFu;
  bool was_active = b->vox_active;
  bool now_active;

  if (!(b->regs[0x31] & REG_31_VOX_ENABLE))
    now_active = false;
  else
    now_active = was_active ? amp >= disable_thresh : amp >= enable_thresh;
  if (now_active == was_active)
    return;
  b->vox_active = now_active;
  b->regs[0x02] |= now_active ? REG_02_VOX_FOUND : REG_02_VOX_LOST;
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
    bk4819_update_scan(c);
    bk4819_update_tx(c);
    break;
  }
  case 0x30:
    bk4819_update_tx(c);
    break;
  case 0x78:
    bk4819_update_squelch(c);
    bk4819_update_scan(c);
    break;
  case 0x32:
    bk4819_update_scan(c);
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
  case 0x31: case 0x46: case 0x79: case 0x64:
    bk4819_update_vox(c);
    break;
  case 0x33: {
    const dp32g030_hooks_t *h = &c->periph.hooks;

    if (h->bk4819_gpio_change)
      h->bk4819_gpio_change(h->user, (uint8_t)b->regs[0x33]);
    bk4819_update_tx(c);
    break;
  }
  case 0x47:
  case 0x50:
  case 0x70:
  case 0x71:
    bk4819_update_tone(c);
    break;
  case 0x59:
    if (b->regs[0x59] & REG_59_RX_FIFO_CLEAR) {
      b->fsk_rx_head = b->fsk_rx_count = 0;
      bk4819_fsk_fifo_status(b);
    }
    break;
  default:
    break;
  }
}

static void bk4819_fsk_fifo_status(dp32g030_bk4819_t *b) {
  if (b->fsk_rx_count >= FSK_FIFO_ALMOST_FULL_LEVEL)
    b->regs[0x02] |= REG_02_FSK_FIFO_ALMOST_FULL;
  else
    b->regs[0x02] &= (uint16_t)~REG_02_FSK_FIFO_ALMOST_FULL;
  if (b->fsk_rx_count == 0)
    b->regs[0x02] |= REG_02_FSK_FIFO_ALMOST_EMPTY;
  else
    b->regs[0x02] &= (uint16_t)~REG_02_FSK_FIFO_ALMOST_EMPTY;
}

/* Pop the front of the RX FIFO into regs[0x5F] (0 if empty), ready for
   the bit-bang read phase to stream out. */
static void bk4819_fsk_fifo_pop(dp32g030_bk4819_t *b) {
  if (b->fsk_rx_count == 0) {
    b->regs[0x5F] = 0;
    return;
  }
  b->regs[0x5F] = b->fsk_rx[b->fsk_rx_head];
  b->fsk_rx_head = (b->fsk_rx_head + 1) % FSK_FIFO_DEPTH;
  b->fsk_rx_count--;
  bk4819_fsk_fifo_status(b);
}

/* Protocol decoder for the bit-banged chip-select/clock/data link. Called
   after every write that can change the three lines, with their effective
   levels: `scn_scl_out` says both CS and clock are driven as outputs (the
   firmware has configured the link), `sda_in` says the data line is currently
   an input (the host is reading). The chip answers by driving SDA through the
   SoC (soc->bk4819_drive_sda). */
void dp32g030_bk4819_pins(dp32g030_cpu_t *c, bool scn, bool scl, bool sda,
                          bool scn_scl_out, bool sda_in) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  if (!scn_scl_out) {
    /* SCN/SCL not driven as outputs: not wired up (yet), or firmware
       hasn't configured the pins. Nothing to decode. */
    bk4819_reset_transaction(b);
    b->prev_scn = b->prev_scl = b->prev_sda_in = false;
    return;
  }

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
      b->shift = (uint16_t)((b->shift << 1) | (sda ? 1u : 0u));
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
        if (b->addr == 0x5F)
          bk4819_fsk_fifo_pop(b);
        b->shift = b->regs[b->addr];
        if (b->addr == 0x02)
          b->regs[0x02] &= (uint16_t)~(
              REG_02_SQUELCH_FOUND | REG_02_SQUELCH_LOST | REG_02_CTCSS_FOUND |
              REG_02_CTCSS_LOST | REG_02_CDCSS_FOUND | REG_02_CDCSS_LOST |
              REG_02_DTMF_5TONE_FOUND | REG_02_VOX_FOUND | REG_02_VOX_LOST |
              REG_02_FSK_RX_FINISHED);
        b->bitpos = 0;
        c->soc->bk4819_drive_sda(c, (b->shift & 0x8000u) != 0);
      } else if (sda_in && b->prev_scl && !scl && b->bitpos < 15) {
        /* Falling clock edge: the host just sampled; queue the next bit. */
        b->bitpos++;
        c->soc->bk4819_drive_sda(c, ((b->shift >> (15 - b->bitpos)) & 1u) != 0);
      }
    }
  }
  b->prev_scn = scn;
  b->prev_scl = scl;
  b->prev_sda_in = sda_in;
}

/* DP32G030: CS, clock and data are GPIOC pins 0-2. Called after every GPIOC
   DATA/DIR write with the port's effective levels and direction. */
void dp32g030_bk4819_gpio_edge(dp32g030_cpu_t *c, uint32_t before,
                               uint32_t after, uint32_t dir) {
  (void)before;
  dp32g030_bk4819_pins(
      c, (after & BK4819_PIN_SCN) != 0, (after & BK4819_PIN_SCL) != 0,
      (after & BK4819_PIN_SDA) != 0,
      (dir & (BK4819_PIN_SCN | BK4819_PIN_SCL)) ==
          (BK4819_PIN_SCN | BK4819_PIN_SCL),
      !(dir & BK4819_PIN_SDA));
}

void dp32g030_bk4819_drive_sda_dp32(dp32g030_cpu_t *c, bool level) {
  dp32g030_gpio_set_input_dp32(c, 2, BK4819_PIN_SDA,
                               level ? BK4819_PIN_SDA : 0);
}

void dp32g030_bk4819_set_signal(dp32g030_cpu_t *c, uint16_t rssi,
                                uint8_t glitch, uint8_t noise) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  b->regs[0x67] = rssi & 0x01FFu;
  b->regs[0x63] = glitch;
  b->regs[0x65] = noise & 0x7Fu;
  bk4819_update_squelch(c);
  bk4819_update_scan(c);
}

uint32_t dp32g030_bk4819_frequency(const dp32g030_cpu_t *c) {
  const dp32g030_bk4819_t *b = &c->periph.bk4819;

  return (uint32_t)b->regs[0x38] | ((uint32_t)b->regs[0x39] << 16);
}

unsigned dp32g030_bk4819_tone_hz(const dp32g030_cpu_t *c) {
  return c->periph.bk4819.tone_hz;
}

bool dp32g030_bk4819_tx_active(const dp32g030_cpu_t *c) {
  return c->periph.bk4819.tx_active;
}

uint32_t dp32g030_bk4819_tx_hz(const dp32g030_cpu_t *c) {
  return c->periph.bk4819.tx_hz;
}

bool dp32g030_bk4819_tx_muted(const dp32g030_cpu_t *c) {
  return (c->periph.bk4819.regs[0x50] & REG_50_TX_MUTE) != 0;
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
  static const char keys[] = "0123456789ABCD*#";
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

void dp32g030_bk4819_set_mic_level(dp32g030_cpu_t *c, uint16_t amplitude) {
  c->periph.bk4819.regs[0x64] = amplitude & 0x7FFFu;
  bk4819_update_vox(c);
}

bool dp32g030_bk4819_fsk_rx_push(dp32g030_cpu_t *c, uint16_t word) {
  dp32g030_bk4819_t *b = &c->periph.bk4819;

  if (!(b->regs[0x59] & REG_59_FSK_RX_ENABLE) || b->fsk_rx_count >= FSK_FIFO_DEPTH)
    return false;
  b->fsk_rx[(b->fsk_rx_head + b->fsk_rx_count) % FSK_FIFO_DEPTH] = word;
  b->fsk_rx_count++;
  bk4819_fsk_fifo_status(b);
  return true;
}

void dp32g030_bk4819_fsk_rx_finish(dp32g030_cpu_t *c) {
  c->periph.bk4819.regs[0x02] |= REG_02_FSK_RX_FINISHED;
}
