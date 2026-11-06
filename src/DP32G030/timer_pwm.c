#include "periph.h"
#include "soc.h"
#include "exc.h"

/* IRQ numbers must match the region table in mmio.c. */
static const int timer_base_irq[3] = {5, 6, 21};
static const int pwm_base_irq[2] = {9, 10};

#define TB_EN_LOW (1u << 0)
#define TB_EN_HIGH (1u << 1)
#define TB_IF_LOW (1u << 0)
#define TB_IF_HIGH (1u << 1)

#define PB_EN_COUNTER (1u << 0)
#define PB_IF_CH0 (1u << 0)
#define PB_IF_CH1 (1u << 1)
#define PB_IF_CH2 (1u << 2)
#define PB_IF_POF (1u << 3)
#define PB_IF_MASK 0xFu

static unsigned timer_base_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x40064000u) / DP32G030_MMIO_REGION_SIZE;
}

static unsigned pwm_base_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x400B1000u) / DP32G030_MMIO_REGION_SIZE;
}

static void timer_base_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_timer_base_t *t = &c->periph.timer_base[n];

  dp32g030_irq_set(c, (unsigned)timer_base_irq[n],
                   (t->iflags & t->ie & (TB_IF_LOW | TB_IF_HIGH)) != 0);
}

static void pwm_base_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_pwm_base_t *p = &c->periph.pwm_base[n];

  dp32g030_irq_set(c, (unsigned)pwm_base_irq[n],
                   (p->iflags & p->ie & PB_IF_MASK) != 0);
}

static void timer_base_tick_one(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_timer_base_t *t = &c->periph.timer_base[n];

  if (t->en & TB_EN_HIGH) {
    t->high_cnt = (t->high_cnt + 1) & 0xFFFFu;
    if (t->high_cnt >= (t->high_load & 0xFFFFu)) {
      t->high_cnt = 0;
      t->iflags |= TB_IF_HIGH;
    }
  }
  if (t->en & TB_EN_LOW) {
    t->low_cnt = (t->low_cnt + 1) & 0xFFFFu;
    if (t->low_cnt >= (t->low_load & 0xFFFFu)) {
      t->low_cnt = 0;
      t->iflags |= TB_IF_LOW;
    }
  }
  timer_base_update_irq(c, n);
}

static void pwm_base_tick_one(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_pwm_base_t *p = &c->periph.pwm_base[n];
  uint32_t period = p->period & 0xFFFFu;

  p->cnt = (p->cnt + 1) & 0xFFFFu;
  if (p->cnt > period) {
    p->cnt = 0;
    p->iflags |= PB_IF_POF;
  }
  for (unsigned ch = 0; ch < 3; ch++)
    if (p->cnt == (p->comp[ch] & 0xFFFFu))
      p->iflags |= (uint32_t)1u << ch;
  pwm_base_update_irq(c, n);
}

/* Run `cycles` core clocks through one unit's shared prescaler, firing
   `tick_one` once per elapsed (div+1)-cycle period. */
void dp32g030_prescaled_run(dp32g030_cpu_t *c, uint32_t div, uint32_t *presc,
                           uint64_t cycles, unsigned n,
                           void (*tick_one)(dp32g030_cpu_t *, unsigned)) {
  uint32_t period = (div & 0xFFFFu) + 1u;

  if (*presc == 0 || *presc > period)
    *presc = period;
  while (cycles) {
    uint64_t step = cycles < *presc ? cycles : *presc;

    *presc -= (uint32_t)step;
    cycles -= step;
    if (*presc == 0) {
      tick_one(c, n);
      *presc = period;
    }
  }
}

void dp32g030_timer_pwm_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  for (unsigned n = 0; n < 3; n++) {
    dp32g030_timer_base_t *t = &c->periph.timer_base[n];

    if (t->en & (TB_EN_HIGH | TB_EN_LOW))
      dp32g030_prescaled_run(c, t->div, &t->presc, cycles, n, timer_base_tick_one);
  }
  for (unsigned n = 0; n < 2; n++) {
    dp32g030_pwm_base_t *p = &c->periph.pwm_base[n];

    if (p->en & PB_EN_COUNTER)
      dp32g030_prescaled_run(c, p->div, &p->presc, cycles, n, pwm_base_tick_one);
  }
  dp32g030_timer_plus_tick(c, cycles);
  dp32g030_pwm_plus_tick(c, cycles);
}

/* Prescaled ticks until a free-running up-counter next equals `load`
   (TIMER_BASE semantics: reset-on-match, no "+1"). */
static uint64_t ticks_to_load(uint32_t cnt, uint32_t load) {
  uint32_t l = load & 0xFFFFu;
  uint32_t delta = l >= cnt ? l - cnt : 0u;

  return delta ? delta : 1u;
}

/* Core clocks until a 0..period counter (wrapping to 0 past period) next
   equals `target`, which may be unreachable (target > period). */
static uint64_t ticks_to_pwm_match(uint32_t cnt, uint32_t period,
                                   uint32_t target) {
  uint64_t span = (uint64_t)period + 1u;
  uint64_t delta;

  if (target > period)
    return DP32G030_NO_EVENT;
  /* CNT resets to 0xFFFF; the first tick masks it back to 0, then it's
     `target` ticks to the match (even if that is 0). */
  if (cnt > period)
    return (uint64_t)(0x10000u - cnt) + target;
  delta = target >= cnt ? (uint64_t)(target - cnt) : span - cnt + target;
  return delta ? delta : span;
}

uint64_t dp32g030_presc_event(uint32_t presc, uint32_t div, uint64_t ticks) {
  uint32_t period = (div & 0xFFFFu) + 1u;
  uint32_t rem = (presc == 0 || presc > period) ? period : presc;

  return rem + (ticks - 1u) * (uint64_t)period;
}

uint64_t dp32g030_timer_pwm_next_event(const dp32g030_cpu_t *c) {
  uint64_t best = DP32G030_NO_EVENT;

  for (unsigned n = 0; n < 3; n++) {
    const dp32g030_timer_base_t *t = &c->periph.timer_base[n];
    uint64_t ticks = DP32G030_NO_EVENT;

    if (!(t->en & (TB_EN_HIGH | TB_EN_LOW)))
      continue;
    if (t->en & TB_EN_HIGH)
      ticks = ticks_to_load(t->high_cnt, t->high_load);
    if (t->en & TB_EN_LOW) {
      uint64_t lo = ticks_to_load(t->low_cnt, t->low_load);
      if (lo < ticks)
        ticks = lo;
    }
    uint64_t ev = dp32g030_presc_event(t->presc, t->div, ticks);
    if (ev < best)
      best = ev;
  }
  for (unsigned n = 0; n < 2; n++) {
    const dp32g030_pwm_base_t *p = &c->periph.pwm_base[n];
    uint32_t period = p->period & 0xFFFFu;
    uint64_t ticks; /* POF: cycles until the counter wraps past `period` */

    if (!(p->en & PB_EN_COUNTER))
      continue;
    /* CNT resets to 0xFFFF; the first tick just masks it back into range. */
    ticks = p->cnt > period
                ? (uint64_t)(0x10000u - p->cnt) + period + 1u
                : (uint64_t)period + 1u - p->cnt;
    for (unsigned ch = 0; ch < 3; ch++) {
      uint64_t t2 = ticks_to_pwm_match(p->cnt, period, p->comp[ch] & 0xFFFFu);
      if (t2 < ticks)
        ticks = t2;
    }
    uint64_t ev = dp32g030_presc_event(p->presc, p->div, ticks);
    if (ev < best)
      best = ev;
  }
  {
    uint64_t a = dp32g030_timer_plus_next_event(c);
    uint64_t b = dp32g030_pwm_plus_next_event(c);

    if (a < best)
      best = a;
    if (b < best)
      best = b;
  }
  return best;
}

bool dp32g030_timer_base_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                              uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_timer_base_t *t = &c->periph.timer_base[timer_base_index(r)];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = t->en; break;
  case 0x04: v = t->div; break;
  case 0x10: v = t->ie; break;
  case 0x14: v = t->iflags; break;
  case 0x20: v = t->high_load; break;
  case 0x24: v = t->high_cnt; break;
  case 0x30: v = t->low_load; break;
  case 0x34: v = t->low_cnt; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_timer_base_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                               uint32_t off, unsigned size, uint32_t val) {
  unsigned n = timer_base_index(r);
  dp32g030_timer_base_t *t = &c->periph.timer_base[n];

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: t->en = val & 3u; break;
  case 0x04: t->div = val & 0xFFFFu; break;
  case 0x10: t->ie = val & 3u; break;
  case 0x14: t->iflags &= ~val; break;
  case 0x20: t->high_load = val & 0xFFFFu; break;
  case 0x24: break; /* read-only */
  case 0x30: t->low_load = val & 0xFFFFu; break;
  case 0x34: break; /* read-only */
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  timer_base_update_irq(c, n);
  return true;
}

bool dp32g030_pwm_base_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                            uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_pwm_base_t *p = &c->periph.pwm_base[pwm_base_index(r)];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = p->en; break;
  case 0x04: v = p->div; break;
  case 0x08: v = p->con; break;
  case 0x0C: v = p->period; break;
  case 0x10: v = p->ie; break;
  case 0x14: v = p->iflags; break;
  case 0x18: v = p->cnt; break;
  case 0x20: v = p->comp[0]; break;
  case 0x30: v = p->comp[1]; break;
  case 0x40: v = p->comp[2]; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_pwm_base_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                             uint32_t off, unsigned size, uint32_t val) {
  unsigned n = pwm_base_index(r);
  dp32g030_pwm_base_t *p = &c->periph.pwm_base[n];

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: p->en = val & 1u; break;
  case 0x04: p->div = val & 0xFFFFu; break;
  case 0x08: p->con = val & 0x7Fu; break;
  case 0x0C: p->period = val & 0xFFFFu; break;
  case 0x10: p->ie = val & PB_IF_MASK; break;
  case 0x14: p->iflags &= ~val; break;
  case 0x18: break; /* read-only */
  case 0x20: p->comp[0] = val & 0xFFFFu; break;
  case 0x30: p->comp[1] = val & 0xFFFFu; break;
  case 0x40: p->comp[2] = val & 0xFFFFu; break;
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  pwm_base_update_irq(c, n);
  return true;
}
