#include "periph.h"

#include "bus.h"
#include "exc.h"

/* PY32F071 DMA1, STM32-style. A transfer runs to completion the moment it is
   started, except SPI and timer-paced channels. Memory-to-memory copies
   CPAR -> CMAR. A memory-to-peripheral channel (DIR = 1) clocks the
   peripheral: each unit is written to CPAR and, for every enabled
   peripheral-to-memory channel (DIR = 0) with the same CPAR, one unit is read
   back into its memory buffer. A peripheral-to-memory channel on its own waits
   until a matching transmit channel runs. A channel whose CPAR is an SPI data
   register waits until that SPI is enabled (SPE): the Fusion flash driver
   arms DMA while SPE is clear, then turns the SPI on. A channel mapped
   (SYSCFG CFGR3/CFGR4) to a timer update request is paced: one unit per
   request, and circular mode (CIRC) reloads it after a lap. Half-transfer
   flags are not modeled. */

#define CCR_EN (1u << 0)
#define CCR_TCIE (1u << 1)
#define CCR_HTIE (1u << 2)
#define CCR_TEIE (1u << 3)
#define CCR_DIR (1u << 4)
#define CCR_CIRC (1u << 5)
#define CCR_PINC (1u << 6)
#define CCR_MINC (1u << 7)
#define CCR_MEM2MEM (1u << 14)

#define SYSCFG_BASE 0x40010000u
#define DMAREQ_TIM6_UP 0x26u
#define DMAREQ_TIM7_UP 0x27u
#define SPI1_DR 0x4001300Cu
#define SPI2_DR 0x4000380Cu
#define SPI_CR1_SPE (1u << 6)
#define USART1_DR 0x40013804u
#define USART2_DR 0x40004404u
#define USART3_DR 0x40004804u
#define USART4_DR 0x40004C04u

/* The DMA request mapped to channel `n` (0-based) by SYSCFG CFGR3/CFGR4. */
static unsigned dma_map(const dp32g030_cpu_t *c, unsigned n) {
  uint32_t w = py32_regfile_word(c, SYSCFG_BASE, n < 4 ? 0x08u : 0x0Cu);

  return (w >> (8 * (n & 3u))) & 0x3Fu;
}

/* Request-paced channels wait for their peripheral's request. */
static bool dma_paced(const dp32g030_cpu_t *c, unsigned n) {
  unsigned m = dma_map(c, n);

  return m == DMAREQ_TIM6_UP || m == DMAREQ_TIM7_UP;
}

static bool dma_targets_spi(uint32_t par) {
  return par == SPI1_DR || par == SPI2_DR;
}

static bool dma_targets_usart_dr(uint32_t par) {
  return par == USART1_DR || par == USART2_DR || par == USART3_DR ||
         par == USART4_DR;
}

/* SPI DMA only moves while the SPI is enabled; arming EN with SPE clear must
   not silently "complete" a transfer into a dead data register. */
static bool dma_spi_ready(const dp32g030_cpu_t *c, uint32_t par) {
  unsigned n = par == SPI1_DR ? 1u : 2u;

  return (c->py32.spi[n - 1].cr1 & SPI_CR1_SPE) != 0;
}

static unsigned psize(uint32_t ccr) { return 1u << ((ccr >> 8) & 3u); }
static unsigned msize(uint32_t ccr) { return 1u << ((ccr >> 10) & 3u); }

static void dma_update_irq(dp32g030_cpu_t *c) {
  const dp32g030_py32_dma_t *d = &c->py32.dma;
  bool line[3] = {false, false, false}; /* IRQ 9, 10, 11 */

  for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
    uint32_t f = (d->isr >> (4 * n)) & 0xFu;
    uint32_t en = (d->ccr[n] & CCR_TCIE ? 1u << 1 : 0) |
                  (d->ccr[n] & CCR_HTIE ? 1u << 2 : 0) |
                  (d->ccr[n] & CCR_TEIE ? 1u << 3 : 0);

    if (f & en)
      line[n == 0 ? 0 : n < 3 ? 1 : 2] = true;
  }
  dp32g030_irq_set(c, 9, line[0]);
  dp32g030_irq_set(c, 10, line[1]);
  dp32g030_irq_set(c, 11, line[2]);
}

/* A bus error ends the transfer: GIF and TEIF set, the channel disabled. */
static void dma_error(dp32g030_py32_dma_t *d, unsigned n) {
  d->isr |= (1u << (4 * n)) | (1u << (4 * n + 3));
  d->ccr[n] &= ~CCR_EN;
}

static void dma_finish(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_py32_dma_t *d = &c->py32.dma;

  d->isr |= (1u << (4 * n)) | (1u << (4 * n + 1)); /* GIF, TCIF */
  d->ccr[n] &= ~CCR_EN;
}

/* Move one unit on channel `n` (memory side and peripheral side as per DIR). */
static bool dma_unit_rx(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_py32_dma_t *d = &c->py32.dma;
  uint32_t v;

  if (!dp32g030_bus_read(c, d->cur_p[n], psize(d->ccr[n]), &v) ||
      !dp32g030_bus_write(c, d->cur_m[n], msize(d->ccr[n]), v)) {
    dma_error(d, n);
    return false;
  }
  if (d->ccr[n] & CCR_PINC)
    d->cur_p[n] += psize(d->ccr[n]);
  if (d->ccr[n] & CCR_MINC)
    d->cur_m[n] += msize(d->ccr[n]);
  d->cndtr[n]--;
  return true;
}

static void dma_run(dp32g030_cpu_t *c) {
  dp32g030_py32_dma_t *d = &c->py32.dma;

  if (d->busy)
    return;
  d->busy = true;
  for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
    uint32_t ccr = d->ccr[n];

    if (!(ccr & CCR_EN) || !d->cndtr[n] || dma_paced(c, n))
      continue;
    if (!(ccr & CCR_MEM2MEM) && dma_targets_spi(d->cpar[n]) &&
        !dma_spi_ready(c, d->cpar[n]))
      continue; /* wait for SPE; py32_dma_spi_kick runs us then */
    /* USART RX is paced by each received byte (py32_dma_usart_rx_kick). */
    if (!(ccr & (CCR_MEM2MEM | CCR_DIR)) && dma_targets_usart_dr(d->cpar[n]))
      continue;
    if (ccr & CCR_MEM2MEM) {
      while (d->cndtr[n] && dma_unit_rx(c, n))
        ;
    } else if (ccr & CCR_DIR) { /* memory -> peripheral */
      while (d->cndtr[n]) {
        uint32_t v;

        if (!dp32g030_bus_read(c, d->cur_m[n], msize(ccr), &v) ||
            !dp32g030_bus_write(c, d->cur_p[n], psize(ccr), v)) {
          dma_error(d, n);
          break;
        }
        if (ccr & CCR_MINC)
          d->cur_m[n] += msize(ccr);
        if (ccr & CCR_PINC)
          d->cur_p[n] += psize(ccr);
        d->cndtr[n]--;
        /* the peripheral answered: feed the matching receive channels */
        for (unsigned r = 0; r < PY32_DMA_CHANNELS; r++) {
          if (r != n && (d->ccr[r] & CCR_EN) && !(d->ccr[r] & (CCR_DIR | CCR_MEM2MEM)) &&
              d->cndtr[r] && d->cpar[r] == d->cpar[n]) {
            dma_unit_rx(c, r);
          }
        }
      }
    } else {
      continue; /* receive channel: driven by a transmit channel */
    }
    if (!d->cndtr[n])
      dma_finish(c, n);
  }
  /* receive channels fed to completion by a transmit channel */
  for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
    uint32_t ccr = d->ccr[n];

    if ((ccr & CCR_EN) && !(ccr & (CCR_DIR | CCR_MEM2MEM)) && !d->cndtr[n] &&
        !dma_paced(c, n) && !dma_targets_usart_dr(d->cpar[n]))
      dma_finish(c, n);
  }
  d->busy = false;
  dma_update_irq(c);
}

void py32_dma_reset(dp32g030_cpu_t *c) {
  c->py32.dma = (dp32g030_py32_dma_t){0};
  dma_update_irq(c);
}

static uint32_t *dma_chan_reg(dp32g030_py32_dma_t *d, uint32_t off,
                              unsigned *chan) {
  if (off < 0x08 || off >= 0x08 + 0x14 * PY32_DMA_CHANNELS)
    return NULL;
  unsigned n = (off - 0x08) / 0x14;

  *chan = n;
  switch ((off - 0x08) % 0x14) {
  case 0x00: return &d->ccr[n];
  case 0x04: return &d->cndtr[n];
  case 0x08: return &d->cpar[n];
  case 0x0C: return &d->cmar[n];
  default: return NULL;
  }
}

bool py32_dma_read(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                   uint32_t *out) {
  dp32g030_py32_dma_t *d = &c->py32.dma;
  unsigned chan;
  uint32_t v;

  if ((off & ~3u) == 0x00) {
    v = d->isr;
  } else if ((off & ~3u) == 0x04) {
    v = 0; /* IFCR is write-only */
  } else {
    uint32_t *r = dma_chan_reg(d, off & ~3u, &chan);

    if (!r)
      return false;
    v = *r;
  }
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (v >> shift) & mask;
  return true;
}

bool py32_dma_write(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                    uint32_t val) {
  dp32g030_py32_dma_t *d = &c->py32.dma;
  unsigned chan = 0;
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  uint32_t v = (val & mask) << shift;

  if ((off & ~3u) == 0x00)
    return true; /* ISR is read-only */
  if ((off & ~3u) == 0x04) { /* IFCR: writing 1 clears the flags of a channel */
    for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
      uint32_t bits = (v >> (4 * n)) & 0xFu;

      if (bits & 1u) /* CGIF clears all four */
        bits = 0xFu;
      d->isr &= ~(bits << (4 * n));
    }
    dma_update_irq(c);
    return true;
  }
  uint32_t *r = dma_chan_reg(d, off & ~3u, &chan);

  if (!r)
    return false;
  uint32_t old = *r;

  *r = (old & ~(mask << shift)) | v;
  if (r == &d->cndtr[chan])
    d->reload[chan] = d->cndtr[chan];
  if (r == &d->ccr[chan] && !(old & CCR_EN) && (*r & CCR_EN)) {
    d->cur_p[chan] = d->cpar[chan];
    d->cur_m[chan] = d->cmar[chan];
  }
  dma_run(c);
  dma_update_irq(c);
  return true;
}

/* A peripheral raised request `req`: every enabled channel mapped to it moves
   one unit. */
void py32_dma_spi_kick(dp32g030_cpu_t *c) { dma_run(c); }

/* One USART RX byte is waiting in DR: move one unit on matching RX channels. */
void py32_dma_usart_rx_kick(dp32g030_cpu_t *c, uint32_t dr_addr) {
  dp32g030_py32_dma_t *d = &c->py32.dma;

  if (d->busy)
    return;
  d->busy = true;
  for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
    uint32_t ccr = d->ccr[n];

    if (!(ccr & CCR_EN) || !d->cndtr[n] || (ccr & (CCR_DIR | CCR_MEM2MEM)) ||
        d->cpar[n] != dr_addr)
      continue;
    if (!dma_unit_rx(c, n))
      continue;
    if (!d->cndtr[n]) {
      d->isr |= (1u << (4 * n)) | (1u << (4 * n + 1)); /* GIF, TCIF */
      if (ccr & CCR_CIRC) {
        d->cndtr[n] = d->reload[n];
        d->cur_p[n] = d->cpar[n];
        d->cur_m[n] = d->cmar[n];
      } else {
        d->ccr[n] &= ~CCR_EN;
      }
    }
  }
  d->busy = false;
  dma_update_irq(c);
}

void py32_dma_request(dp32g030_cpu_t *c, unsigned req) {
  dp32g030_py32_dma_t *d = &c->py32.dma;

  if (d->busy)
    return;
  d->busy = true;
  for (unsigned n = 0; n < PY32_DMA_CHANNELS; n++) {
    uint32_t ccr = d->ccr[n];

    if (!(ccr & CCR_EN) || !d->cndtr[n] || dma_map(c, n) != req ||
        !dma_paced(c, n))
      continue;
    if (ccr & CCR_DIR) { /* memory -> peripheral */
      uint32_t v;

      if (!dp32g030_bus_read(c, d->cur_m[n], msize(ccr), &v) ||
          !dp32g030_bus_write(c, d->cur_p[n], psize(ccr), v)) {
        dma_error(d, n);
        continue;
      }
      if (ccr & CCR_MINC)
        d->cur_m[n] += msize(ccr);
      if (ccr & CCR_PINC)
        d->cur_p[n] += psize(ccr);
      d->cndtr[n]--;
    } else if (!dma_unit_rx(c, n)) { /* peripheral -> memory */
      continue;
    }
    if (!d->cndtr[n]) {
      d->isr |= (1u << (4 * n)) | (1u << (4 * n + 1)); /* GIF, TCIF */
      if (ccr & CCR_CIRC) { /* start the next lap */
        d->cndtr[n] = d->reload[n];
        d->cur_p[n] = d->cpar[n];
        d->cur_m[n] = d->cmar[n];
      } else {
        d->ccr[n] &= ~CCR_EN;
      }
    }
  }
  d->busy = false;
  dma_update_irq(c);
}
