#include "periph.h"

#include "exc.h"
#include "soc.h"

/* PY32F071 basic timers TIM6 and TIM7: a 16-bit up counter that overflows at
   ARR into an update event. Registers as in STM32: CR1 (CEN 0, UDIS 1, URS 2,
   OPM 3, ARPE 7), DIER (UIE 0, UDE 8), SR (UIF 0, cleared by writing 0), EGR
   (UG 0), CNT, PSC, ARR. The period is (ARR + 1) * (PSC + 1) core clocks. An
   update event sets UIF, raises the timer's interrupt when UIE is set and a
   DMA request when UDE is set. PSC takes effect at once (the hardware
   buffers it until the next update). */

#define CR1_CEN (1u << 0)
#define CR1_UDIS (1u << 1)
#define CR1_URS (1u << 2)
#define CR1_OPM (1u << 3)
#define CR1_ARPE (1u << 7)
#define DIER_UIE (1u << 0)
#define DIER_UDE (1u << 8)
#define SR_UIF (1u << 0)
#define EGR_UG (1u << 0)

static const unsigned kIrq[2] = {17, 18};     /* TIM6_LPTIM1_DAC, TIM7 */
static const unsigned kDmaReq[2] = {0x26, 0x27}; /* SYSCFG map: TIM6_UP, TIM7_UP */

static void tim_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_py32_tim_t *t = &c->py32.tim[n];

  dp32g030_irq_set(c, kIrq[n], (t->sr & SR_UIF) && (t->dier & DIER_UIE));
}

static void tim_dma_request(dp32g030_cpu_t *c, unsigned n) {
  if (c->py32.tim[n].dier & DIER_UDE)
    py32_dma_request(c, kDmaReq[n]);
}

/* The counter overflowed (or software forced an update). */
static void tim_update_event(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_py32_tim_t *t = &c->py32.tim[n];

  t->cnt = 0;
  if (t->cr1 & CR1_UDIS)
    return; /* the counter reloads but no update event is generated */
  t->arr_shadow = t->arr;
  t->sr |= SR_UIF;
  tim_update_irq(c, n);
  tim_dma_request(c, n);
}

void py32_tim_reset(dp32g030_cpu_t *c) {
  for (unsigned n = 0; n < 2; n++) {
    c->py32.tim[n] = (dp32g030_py32_tim_t){0};
    tim_update_irq(c, n);
  }
}

bool py32_tim_read(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                   uint32_t *out) {
  const dp32g030_py32_tim_t *t = &c->py32.tim[n];
  uint32_t v;

  switch (off & ~3u) {
  case 0x00: v = t->cr1; break;
  case 0x0C: v = t->dier; break;
  case 0x10: v = t->sr; break;
  case 0x14: v = 0; break; /* EGR is write-only */
  case 0x24: v = t->cnt; break;
  case 0x28: v = t->psc; break;
  case 0x2C: v = t->arr; break;
  default: return false;
  }
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (v >> shift) & mask;
  return true;
}

bool py32_tim_write(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                    uint32_t val) {
  dp32g030_py32_tim_t *t = &c->py32.tim[n];
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  uint32_t v = (val & mask) << shift;
  uint32_t keep = ~(mask << shift);

  switch (off & ~3u) {
  case 0x00:
    t->cr1 = ((t->cr1 & keep) | v) & 0xFFFFu;
    break;
  case 0x0C:
    t->dier = ((t->dier & keep) | v) & 0xFFFFu;
    tim_update_irq(c, n);
    break;
  case 0x10: /* SR: a flag is cleared by writing 0 to it */
    t->sr &= (v | keep);
    tim_update_irq(c, n);
    break;
  case 0x14:
    if (v & EGR_UG) { /* software update: reload everything */
      t->cnt = 0;
      t->presc_cnt = 0;
      t->arr_shadow = t->arr;
      if (!(t->cr1 & CR1_URS) && !(t->cr1 & CR1_UDIS)) {
        t->sr |= SR_UIF;
        tim_update_irq(c, n);
        tim_dma_request(c, n);
      }
    }
    break;
  case 0x24:
    t->cnt = ((t->cnt & keep) | v) & 0xFFFFu;
    break;
  case 0x28:
    t->psc = ((t->psc & keep) | v) & 0xFFFFu;
    break;
  case 0x2C:
    t->arr = ((t->arr & keep) | v) & 0xFFFFu;
    if (!(t->cr1 & CR1_ARPE))
      t->arr_shadow = t->arr;
    break;
  default:
    return false;
  }
  return true;
}

void py32_tim_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  for (unsigned n = 0; n < 2; n++) {
    dp32g030_py32_tim_t *t = &c->py32.tim[n];

    if (!(t->cr1 & CR1_CEN))
      continue;
    uint64_t per = (uint64_t)t->psc + 1;
    uint64_t total = t->presc_cnt + cycles;
    uint64_t counts = total / per;

    t->presc_cnt = (uint32_t)(total % per);
    while (counts) {
      uint64_t to_update = t->cnt <= t->arr_shadow
                               ? (uint64_t)t->arr_shadow - t->cnt + 1
                               : 1;

      if (counts < to_update) {
        t->cnt += (uint32_t)counts;
        break;
      }
      counts -= to_update;
      tim_update_event(c, n);
      if (t->cr1 & CR1_OPM) {
        t->cr1 &= ~CR1_CEN;
        break;
      }
    }
  }
}

uint64_t py32_tim_next_event(const dp32g030_cpu_t *c) {
  uint64_t best = DP32G030_NO_EVENT;

  for (unsigned n = 0; n < 2; n++) {
    const dp32g030_py32_tim_t *t = &c->py32.tim[n];

    if (!(t->cr1 & CR1_CEN) || (t->cr1 & CR1_UDIS) ||
        !(t->dier & (DIER_UIE | DIER_UDE)))
      continue;
    uint64_t per = (uint64_t)t->psc + 1;
    uint64_t to_update = t->cnt <= t->arr_shadow
                             ? (uint64_t)t->arr_shadow - t->cnt + 1
                             : 1;
    uint64_t cycles = (per - t->presc_cnt) + (to_update - 1) * per;

    if (cycles < best)
      best = cycles;
  }
  return best;
}
