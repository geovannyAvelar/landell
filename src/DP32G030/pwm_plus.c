#include "periph.h"
#include "soc.h"
#include "exc.h"

/* IRQ numbers must match the region table in mmio.c. */
static const int pwm_plus_irq[2] = {11, 12};

#define PP_CFG_AUTO_RELOAD(cfg) (((cfg) >> 8) & 0xFFu)
#define PP_CFG_OUT_MODE (1u << 3) /* 0 edge-aligned, 1 center-aligned */
#define PP_CFG_CNT_REP (1u << 2)  /* 0 single-shot, 1 loop */
#define PP_CFG_CNT_TYPE (1u << 1) /* 0 up, 1 down (edge) / first half (ctr) */
#define PP_CFG_COUNTER_EN (1u << 0)

#define PP_GEN_OE(ch) (1u << (24 + 2 * (ch)))
#define PP_GEN_OEN(ch) (1u << (25 + 2 * (ch)))
#define PP_GEN_OUTINV(ch) (1u << (16 + 2 * (ch)))
#define PP_GEN_OUTINVN(ch) (1u << (17 + 2 * (ch)))
#define PP_GEN_START(ch) (1u << (8 + (ch)))
#define PP_GEN_IDLE(ch) (1u << (2 * (ch)))
#define PP_GEN_IDLEN(ch) (1u << (1 + 2 * (ch)))

#define PP_BRAKE_POL(ch) (1u << (16 + 2 * (ch)))
#define PP_BRAKE_POLN(ch) (1u << (17 + 2 * (ch)))
#define PP_BRAKE_LEV_SHIFT 12
#define PP_BRAKE_SEL(ch) (7u << (6 + 3 * (ch)))
#define PP_BRAKE_SEL_SHIFT(ch) (6 + 3 * (ch))

#define PP_IF_UP_CH0COMP (1u << 0)
#define PP_IF_UP_CH1COMP (1u << 1)
#define PP_IF_UP_CH2COMP (1u << 2)
#define PP_IF_UP_POF (1u << 3)
#define PP_IF_UP_TRIG (1u << 4)
#define PP_IF_DOWN_CH0COMP (1u << 8)
#define PP_IF_DOWN_CH1COMP (1u << 9)
#define PP_IF_DOWN_CH2COMP (1u << 10)
#define PP_IF_DOWN_POF (1u << 11)
#define PP_IF_DOWN_TRIG (1u << 12)
#define PP_IF_BRAK0 (1u << 16)
#define PP_IF_BRAK1 (1u << 17)
#define PP_IF_BRAK2 (1u << 18)
#define PP_IF_AUTORELOAD (1u << 19)
#define PP_IF_MASK 0xFFFFFu

static unsigned pwm_plus_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x400B4000u) / DP32G030_MMIO_REGION_SIZE;
}

static void pwm_plus_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];

  dp32g030_irq_set(c, (unsigned)pwm_plus_irq[n],
                   (p->iflags & p->ie & PP_IF_MASK) != 0);
}

/* Recompute channel outputs (idle, dead-time-free compare, mask, brake). */
static void pwm_plus_update_outputs(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];
  bool running = (p->cfg & PP_CFG_COUNTER_EN) != 0;
  uint32_t asserted = 0; /* which brake lines are currently active */
  uint32_t level = 0;

  for (unsigned line = 0; line < 3; line++) {
    bool raw = (p->brake_level >> line) & 1u;
    bool pol = (p->brake_cfg >> (PP_BRAKE_LEV_SHIFT + line)) & 1u;

    if (raw == pol)
      asserted |= 1u << line;
  }

  for (unsigned ch = 0; ch < 3; ch++) {
    bool start = (p->gen & PP_GEN_START(ch)) != 0;
    bool raw = running ? (p->cnt < (p->comp[ch] & 0xFFFFu) ? start : !start)
                       : (p->gen & PP_GEN_IDLE(ch)) != 0;
    bool raw_n = running ? !raw : (p->gen & PP_GEN_IDLEN(ch)) != 0;
    bool lvl = raw ^ ((p->gen & PP_GEN_OUTINV(ch)) != 0);
    bool lvl_n = raw_n ^ ((p->gen & PP_GEN_OUTINVN(ch)) != 0);
    uint32_t sel = (p->brake_cfg >> PP_BRAKE_SEL_SHIFT(ch)) & 7u;
    bool brake_active = (sel & asserted) != 0;

    if (p->mask_en & (1u << (2 * ch)))
      lvl = (p->mask_lev & (1u << (2 * ch))) != 0;
    if (p->mask_en & (1u << (2 * ch + 1)))
      lvl_n = (p->mask_lev & (1u << (2 * ch + 1))) != 0;
    if (brake_active) {
      lvl = (p->brake_cfg & PP_BRAKE_POL(ch)) != 0;
      lvl_n = (p->brake_cfg & PP_BRAKE_POLN(ch)) != 0;
      for (unsigned line = 0; line < 3; line++)
        if ((sel & (1u << line)) && (asserted & (1u << line)))
          p->iflags |= (1u << (16 + line));
    }
    level |= (lvl ? 1u : 0u) << (2 * ch);
    level |= (lvl_n ? 1u : 0u) << (2 * ch + 1);
  }
  p->ch_level = level;
  pwm_plus_update_irq(c, n);
}

static void pwm_plus_tick_one(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];
  uint32_t period = p->period & 0xFFFFu;
  bool center = (p->cfg & PP_CFG_OUT_MODE) != 0;
  bool down = center ? p->counting_down : (p->cfg & PP_CFG_CNT_TYPE) != 0;
  bool wrapped = false;

  if (!down) {
    p->cnt = (p->cnt + 1) & 0xFFFFu;
    for (unsigned ch = 0; ch < 3; ch++)
      if (p->cnt == (p->comp[ch] & 0xFFFFu))
        p->iflags |= (uint32_t)1u << ch;
    if (p->cnt == (p->trig_comp & 0xFFFFu))
      p->iflags |= PP_IF_UP_TRIG;
    if (p->cnt > period) {
      if (center) {
        p->counting_down = true;
        p->cnt = period;
      } else {
        p->cnt = 0;
        wrapped = true;
      }
      p->iflags |= PP_IF_UP_POF;
    }
  } else if (p->cnt > period) {
    /* CNT resets to 0xFFFF; snap into range instead of counting down
       through the whole 16-bit space. */
    p->cnt = period;
  } else {
    if (p->cnt == 0) {
      p->cnt = center ? 0 : period;
      if (center)
        p->counting_down = false;
      wrapped = !center;
      p->iflags |= PP_IF_DOWN_POF;
    } else {
      p->cnt--;
      for (unsigned ch = 0; ch < 3; ch++)
        if (p->cnt == (p->comp[ch] & 0xFFFFu))
          p->iflags |= (uint32_t)1u << (8 + ch);
      if (p->cnt == (p->trig_comp & 0xFFFFu))
        p->iflags |= PP_IF_DOWN_TRIG;
    }
  }
  if (!(p->cfg & PP_CFG_CNT_REP)) {
    bool full_cycle_done = center ? (down && p->cnt == 0 && !p->counting_down)
                                  : wrapped;
    if (full_cycle_done)
      p->cfg &= ~PP_CFG_COUNTER_EN;
  }
  pwm_plus_update_outputs(c, n);
}

void dp32g030_pwm_plus_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  for (unsigned n = 0; n < 2; n++) {
    dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];

    if (p->cfg & PP_CFG_COUNTER_EN)
      dp32g030_prescaled_run(c, p->clksrc >> 16, &p->presc, cycles, n,
                             pwm_plus_tick_one);
  }
}

static uint64_t ticks_wrap(uint32_t cnt, uint32_t period, uint32_t target,
                           bool down) {
  uint64_t span = (uint64_t)period + 1u;
  uint64_t delta;

  if (target > period)
    return DP32G030_NO_EVENT;
  if (cnt > period) {
    /* CNT resets to 0xFFFF, outside [0, period]. One tick snaps it into
       range (checking for a match while counting up, but not while
       counting down; see pwm_plus_tick_one) before normal progress
       resumes. */
    if (down)
      return 1u + ticks_wrap(period, period, target, true);
    return (uint64_t)(0x10000u - cnt) + target;
  }
  if (!down)
    delta = target >= cnt ? (uint64_t)(target - cnt) : span - cnt + target;
  else
    delta = cnt >= target ? (uint64_t)(cnt - target) : cnt + (span - target);
  return delta ? delta : span;
}

uint64_t dp32g030_pwm_plus_next_event(const dp32g030_cpu_t *c) {
  uint64_t best = DP32G030_NO_EVENT;

  for (unsigned n = 0; n < 2; n++) {
    const dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];
    uint32_t period = p->period & 0xFFFFu;
    bool center = (p->cfg & PP_CFG_OUT_MODE) != 0;
    bool down;
    uint64_t ticks, ev;

    if (!(p->cfg & PP_CFG_COUNTER_EN))
      continue;
    if (center) {
      /* Direction reverses at each end; fall back to a 1-tick step. */
      ev = dp32g030_presc_event(p->presc, p->clksrc >> 16, 1);
      if (ev < best)
        best = ev;
      continue;
    }
    down = (p->cfg & PP_CFG_CNT_TYPE) != 0;
    /* POF: up wraps just past `period`, down wraps at 0. */
    ticks = ticks_wrap(p->cnt, period, down ? 0u : period, down);
    for (unsigned ch = 0; ch < 3; ch++) {
      uint64_t t2 = ticks_wrap(p->cnt, period, p->comp[ch] & 0xFFFFu, down);
      if (t2 < ticks)
        ticks = t2;
    }
    ev = dp32g030_presc_event(p->presc, p->clksrc >> 16, ticks);
    if (ev < best)
      best = ev;
  }
  return best;
}

void dp32g030_pwm_plus_set_brake(dp32g030_cpu_t *c, unsigned unit,
                                 uint32_t mask) {
  dp32g030_pwm_plus_t *p;

  if (unit >= 2)
    return;
  p = &c->periph.pwm_plus[unit];
  p->brake_level = mask & 7u;
  pwm_plus_update_outputs(c, unit);
}

bool dp32g030_pwm_plus_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                            uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[pwm_plus_index(r)];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = p->cfg; break;
  case 0x04: v = p->gen; break;
  case 0x08: v = p->clksrc; break;
  case 0x0C: v = p->brake_cfg; break;
  case 0x10: v = p->mask_lev; break;
  case 0x1C: v = p->period; break;
  case 0x20: v = p->comp[0]; break;
  case 0x24: v = p->comp[1]; break;
  case 0x28: v = p->comp[2]; break;
  case 0x30: v = p->dt[0]; break;
  case 0x34: v = p->dt[1]; break;
  case 0x38: v = p->dt[2]; break;
  case 0x40: v = p->trig_comp; break;
  case 0x44: v = p->trig_cfg; break;
  case 0x60: v = p->ie; break;
  case 0x64: v = p->iflags; break;
  case 0x84: v = 0; break; /* SWLOAD self-clears */
  case 0x88: v = p->mask_en; break;
  case 0xE0:
    v = (p->cfg & PP_CFG_COUNTER_EN ? 1u << 17 : 0u) |
        (p->counting_down ? 1u << 16 : 0u) | (p->cnt & 0xFFFFu);
    break;
  case 0xE4: v = p->brake_level & 3u; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_pwm_plus_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                             uint32_t off, unsigned size, uint32_t val) {
  unsigned n = pwm_plus_index(r);
  dp32g030_pwm_plus_t *p = &c->periph.pwm_plus[n];
  bool outputs = true;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: p->cfg = val & 0xFFFFu; break;
  case 0x04: p->gen = val & 0x3F3F073Fu; break;
  case 0x08: p->clksrc = val & 0xFFFF0037u; break;
  case 0x0C: p->brake_cfg = val & 0x33F71FFu; break;
  case 0x10: p->mask_lev = val & 0x3Fu; break;
  case 0x1C: p->period = val & 0xFFFFu; outputs = false; break;
  case 0x20: p->comp[0] = val & 0xFFFFu; outputs = false; break;
  case 0x24: p->comp[1] = val & 0xFFFFu; outputs = false; break;
  case 0x28: p->comp[2] = val & 0xFFFFu; outputs = false; break;
  case 0x30: p->dt[0] = val & 0x3FFu; outputs = false; break;
  case 0x34: p->dt[1] = val & 0x3FFu; outputs = false; break;
  case 0x38: p->dt[2] = val & 0x3FFu; outputs = false; break;
  case 0x40: p->trig_comp = val & 0xFFFFu; outputs = false; break;
  case 0x44: p->trig_cfg = val & 0xFu; outputs = false; break;
  case 0x60: p->ie = val & PP_IF_MASK; outputs = false; break;
  case 0x64: p->iflags &= ~val; break;
  case 0x84: outputs = false; break; /* SWLOAD: no shadow regs modeled */
  case 0x88: p->mask_en = val & 0x3Fu; break;
  case 0xE0: case 0xE4: outputs = false; break; /* read-only */
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  if (outputs)
    pwm_plus_update_outputs(c, n);
  else
    pwm_plus_update_irq(c, n);
  return true;
}
