#include "periph.h"

#include "bus.h"
#include "exc.h"

/* PY32F071 DMA1, STM32-style. A transfer runs to completion the moment it is
   started. Memory-to-memory copies CPAR -> CMAR. A memory-to-peripheral
   channel (DIR = 1) clocks the peripheral: each unit is written to CPAR and,
   for every enabled peripheral-to-memory channel (DIR = 0) with the same CPAR,
   one unit is read back into its memory buffer. A peripheral-to-memory channel
   on its own waits until a matching transmit channel runs. Circular mode and
   half-transfer flags are not modeled. */

#define CCR_EN (1u << 0)
#define CCR_TCIE (1u << 1)
#define CCR_HTIE (1u << 2)
#define CCR_TEIE (1u << 3)
#define CCR_DIR (1u << 4)
#define CCR_PINC (1u << 6)
#define CCR_MINC (1u << 7)
#define CCR_MEM2MEM (1u << 14)

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

    if (!(ccr & CCR_EN) || !d->cndtr[n])
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

    if ((ccr & CCR_EN) && !(ccr & (CCR_DIR | CCR_MEM2MEM)) && !d->cndtr[n])
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
  if (r == &d->ccr[chan] && !(old & CCR_EN) && (*r & CCR_EN)) {
    d->cur_p[chan] = d->cpar[chan];
    d->cur_m[chan] = d->cmar[chan];
  }
  dma_run(c);
  dma_update_irq(c);
  return true;
}

void py32_dma_request(dp32g030_cpu_t *c, unsigned req) {
  (void)c;
  (void)req;
}
