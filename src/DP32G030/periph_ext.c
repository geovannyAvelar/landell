#include "periph.h"

#include "exc.h"

#include <string.h>

/* ---- SPI (master only; transfers complete instantly) --------------- */

#define SPI_CR_RF_CLR (1u << 15)
#define SPI_IF_OVF (1u << 0)
#define SPI_IF_RFF (1u << 1)
#define SPI_IF_RFHF (1u << 2)
#define SPI_IF_TFE (1u << 3)
#define SPI_IE_MASK 0x1Fu

static unsigned spi_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x400B8000u) / DP32G030_MMIO_REGION_SIZE;
}

static uint32_t spi_status(const dp32g030_spi_t *s) {
  uint32_t f = s->sticky & SPI_IF_OVF;

  if (s->rx_count == DP32G030_SPI_FIFO_DEPTH)
    f |= SPI_IF_RFF;
  if (s->rx_count >= DP32G030_SPI_FIFO_DEPTH / 2)
    f |= SPI_IF_RFHF;
  return f | SPI_IF_TFE;
}

/* FIFOST: RFE RFF RFHF TFE TFF TFHF, RF level [8:6], TF level [11:9]. */
static uint32_t spi_fifost(const dp32g030_spi_t *s) {
  uint32_t v = (s->rx_count == 0) ? 1u : 0u;

  if (s->rx_count == DP32G030_SPI_FIFO_DEPTH)
    v |= 1u << 1;
  if (s->rx_count >= DP32G030_SPI_FIFO_DEPTH / 2)
    v |= 1u << 2;
  v |= 1u << 3; /* TX FIFO empty */
  v |= (uint32_t)(s->rx_count & 7u) << 6;
  return v;
}

static void spi_update_irq(dp32g030_cpu_t *c, const dp32g030_region_t *r) {
  const dp32g030_spi_t *s = &c->periph.spi[spi_index(r)];

  if (r->irq >= 0)
    dp32g030_irq_set(c, (unsigned)r->irq, (spi_status(s) & s->ie & SPI_IE_MASK) != 0);
}

bool dp32g030_spi_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  dp32g030_spi_t *s = &c->periph.spi[spi_index(r)];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    v = s->cr & ~SPI_CR_RF_CLR;
    break;
  case 0x04:
    v = 0;
    break;
  case 0x08:
    v = 0;
    if (s->rx_count) {
      v = s->rx[s->rx_head];
      s->rx_head = (s->rx_head + 1) % DP32G030_SPI_FIFO_DEPTH;
      s->rx_count--;
      spi_update_irq(c, r);
    }
    break;
  case 0x10:
    v = s->ie;
    break;
  case 0x14:
    v = spi_status(s);
    break;
  case 0x18:
    v = spi_fifost(s);
    break;
  default:
    return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_spi_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  unsigned n = spi_index(r);
  dp32g030_spi_t *s = &c->periph.spi[n];

  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    s->cr = val;
    if (val & SPI_CR_RF_CLR) {
      s->rx_head = 0;
      s->rx_count = 0;
    }
    break;
  case 0x04:
    /* Needs SPE (bit 3) and MSTR (bit 6); otherwise nothing is clocked. */
    if ((s->cr & (1u << 3)) && (s->cr & (1u << 6))) {
      const dp32g030_hooks_t *h = &c->periph.hooks;
      uint8_t rx = h->spi_xfer ? h->spi_xfer(h->user, n, (uint8_t)val) : 0;

      if (s->rx_count == DP32G030_SPI_FIFO_DEPTH) {
        s->sticky |= SPI_IF_OVF;
      } else {
        s->rx[(s->rx_head + s->rx_count) % DP32G030_SPI_FIFO_DEPTH] = rx;
        s->rx_count++;
      }
    }
    break;
  case 0x10:
    s->ie = val & SPI_IE_MASK;
    break;
  case 0x14:
    s->sticky &= ~val;
    break;
  default:
    return dp32g030_regfile_write(c, r, off, size, val);
  }
  spi_update_irq(c, r);
  return true;
}

/* ---- SARADC -------------------------------------------------------- */

#define ADC_CFG_CONT (1u << 18)
#define ADC_CFG_EN (1u << 27)
#define ADC_START_START (1u << 0)
#define ADC_START_RESET (1u << 2)
#define ADC_IF_FULL (1u << 16)
#define ADC_IF_HFULL (1u << 17)

void dp32g030_adc_set_input(dp32g030_cpu_t *c, unsigned ch, uint32_t value) {
  if (ch < DP32G030_ADC_CHANNELS)
    c->periph.adc.input[ch] = value & 0xFFFu;
}

static void adc_update_irq(dp32g030_cpu_t *c, const dp32g030_region_t *r) {
  const dp32g030_adc_t *a = &c->periph.adc;

  if (r->irq >= 0)
    dp32g030_irq_set(c, (unsigned)r->irq,
                     (a->ifl & a->ie & (ADC_IF_FULL | ADC_IF_HFULL)) != 0);
}

bool dp32g030_adc_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  dp32g030_adc_t *a = &c->periph.adc;

  if (size != 4)
    return false;
  if (off >= 0x10 && off < 0x10 + 4 * DP32G030_ADC_CHANNELS) {
    *out = a->result[(off - 0x10) / 4];
    return true;
  }
  switch (off) {
  case 0x00:
    *out = a->cfg;
    break;
  case 0x04:
    *out = 0; /* START self-clears */
    break;
  case 0x08:
    *out = a->ie;
    break;
  case 0x0C:
    *out = a->ifl;
    break;
  case 0xB0:
    *out = a->exttrig;
    break;
  case 0xF0:
    *out = a->calib_offset;
    break;
  case 0xF4:
    *out = a->calib_kd;
    break;
  default:
    return dp32g030_regfile_read(c, r, off, size, out);
  }
  return true;
}

bool dp32g030_adc_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  dp32g030_adc_t *a = &c->periph.adc;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    a->cfg = val;
    break;
  case 0x04:
    if (val & ADC_START_RESET) {
      memset(a->result, 0, sizeof a->result);
      a->ifl = 0;
    }
    /* Conversions finish instantly; results come from the host inputs. */
    if ((val & ADC_START_START) && (a->cfg & ADC_CFG_EN)) {
      for (unsigned i = 0; i < DP32G030_ADC_CHANNELS; i++)
        if (a->cfg & (1u << i))
          a->result[i] = a->input[i];
      a->ifl |= ADC_IF_FULL | ADC_IF_HFULL;
    }
    break;
  case 0x08:
    a->ie = val;
    break;
  case 0x0C:
    a->ifl &= ~val;
    break;
  case 0xB0:
    a->exttrig = val;
    break;
  case 0xF0:
    a->calib_offset = val;
    break;
  case 0xF4:
    a->calib_kd = val;
    break;
  default:
    return dp32g030_regfile_write(c, r, off, size, val);
  }
  adc_update_irq(c, r);
  return true;
}

/* ---- CRC ----------------------------------------------------------- */

static uint32_t rev_bits(uint32_t v, unsigned width) {
  uint32_t o = 0;

  for (unsigned i = 0; i < width; i++)
    if (v & (1u << i))
      o |= 1u << (width - 1 - i);
  return o;
}

static uint32_t rev_bytes(uint32_t v, unsigned width) {
  uint32_t o = 0;

  for (unsigned i = 0; i < width / 8; i++)
    o |= ((v >> (8 * i)) & 0xFFu) << (width - 8 - 8 * i);
  return o;
}

/* Mode 1: reverse bits in each byte; 2: reverse byte order; 3: both. */
static uint32_t crc_reorder(uint32_t v, unsigned mode, unsigned width) {
  if (mode & 1u) {
    uint32_t o = 0;
    for (unsigned i = 0; i < width / 8; i++)
      o |= rev_bits((v >> (8 * i)) & 0xFFu, 8) << (8 * i);
    v = o;
  }
  if (mode & 2u)
    v = rev_bytes(v, width);
  return v;
}

static void crc_feed(dp32g030_crc_t *k, uint32_t data) {
  static const unsigned widths[4] = {32, 16, 8, 8};
  unsigned dw = widths[(k->cr >> 7) & 3u];
  unsigned sel = (k->cr >> 9) & 3u;
  unsigned bits = sel == 0 ? 16 : sel == 1 ? 8 : sel == 2 ? 16 : 32;
  uint32_t poly = sel == 0 ? 0x1021u : sel == 1 ? 0x07u : sel == 2 ? 0x8005u
                                                                     : 0x04C11DB7u;
  uint32_t top = 1u << (bits - 1);
  uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
  uint32_t st = k->state & mask;

  if (dw < 32)
    data &= (1u << dw) - 1u;
  data = crc_reorder(data, (k->cr >> 2) & 3u, dw);
  if (k->cr & 2u)
    data = rev_bits(data, dw);
  for (int byte = (int)dw / 8 - 1; byte >= 0; byte--) {
    st ^= ((data >> (8 * byte)) & 0xFFu) << (bits - 8 < 24 ? bits - 8 : 24);
    if (bits == 8)
      st = (st & 0xFFu);
    for (int i = 0; i < 8; i++)
      st = (st & top) ? ((st << 1) ^ poly) & mask : (st << 1) & mask;
  }
  k->state = st;
}

bool dp32g030_crc_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  dp32g030_crc_t *k = &c->periph.crc;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    *out = k->cr;
    break;
  case 0x04:
    *out = k->iv;
    break;
  case 0x08:
    *out = 0;
    break;
  case 0x0C: {
    unsigned sel = (k->cr >> 9) & 3u;
    unsigned bits = sel == 0 ? 16 : sel == 1 ? 8 : sel == 2 ? 16 : 32;
    uint32_t v = k->state;

    if (k->cr & (1u << 4))
      v = rev_bits(v, bits);
    v = crc_reorder(v, (k->cr >> 5) & 3u, bits);
    *out = v;
    break;
  }
  default:
    return dp32g030_regfile_read(c, r, off, size, out);
  }
  return true;
}

bool dp32g030_crc_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  dp32g030_crc_t *k = &c->periph.crc;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    k->cr = val;
    break;
  case 0x04:
    k->iv = val;
    k->state = val;
    break;
  case 0x08:
    if (k->cr & 1u)
      crc_feed(k, val);
    break;
  default:
    return dp32g030_regfile_write(c, r, off, size, val);
  }
  return true;
}
