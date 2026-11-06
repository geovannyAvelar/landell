#include "periph.h"
#include "soc.h"
#include "exc.h"

/* IRQ numbers must match the region table in mmio.c. */
static const int timer_plus_irq[2] = {7, 8};

#define TP_EN_LOW (1u << 0)
#define TP_EN_HIGH (1u << 1)

#define TP_MODE_HIGH(ctr) (((ctr) >> 16) & 3u)
#define TP_MODE_LOW(ctr) ((ctr) & 3u)
#define TP_EXTSEL_HIGH(ctr) (((ctr) >> 20) & 1u)
#define TP_EXTSEL_LOW(ctr) (((ctr) >> 4) & 1u)
#define TP_EXTEDGE_HIGH(ctr) (((ctr) >> 21) & 3u)
#define TP_EXTEDGE_LOW(ctr) (((ctr) >> 5) & 3u)

#define TP_IF_LOW_TO (1u << 0)
#define TP_IF_LOW_PR (1u << 1)
#define TP_IF_LOW_PF (1u << 2)
#define TP_IF_HIGH_TO (1u << 8)
#define TP_IF_HIGH_PR (1u << 9)
#define TP_IF_HIGH_PF (1u << 10)
#define TP_IF_HALL0_R (1u << 16)
#define TP_IF_HALL0_F (1u << 17)
#define TP_IF_HALL1_R (1u << 18)
#define TP_IF_HALL1_F (1u << 19)
#define TP_IF_HALL2_R (1u << 20)
#define TP_IF_HALL2_F (1u << 21)
#define TP_IF_MASK 0x3F070Fu

static unsigned timer_plus_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x40067000u) / DP32G030_MMIO_REGION_SIZE;
}

static void timer_plus_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_timer_plus_t *t = &c->periph.timer_plus[n];

  dp32g030_irq_set(c, (unsigned)timer_plus_irq[n],
                   (t->iflags & t->ie & TP_IF_MASK) != 0);
}

/* True if `edge_cfg` (0 rising, 1 falling, 2 either) matches old->new. */
static bool edge_matches(unsigned edge_cfg, bool old, bool now) {
  if (old == now)
    return false;
  if (edge_cfg == 0)
    return now;
  if (edge_cfg == 1)
    return !now;
  return true;
}

static void timer_plus_tick_one(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_timer_plus_t *t = &c->periph.timer_plus[n];
  unsigned hmode = TP_MODE_HIGH(t->ctr);
  unsigned lmode = TP_MODE_LOW(t->ctr);

  if ((t->en & TP_EN_HIGH) &&
      (hmode == DP32G030_TIMERPLUS_MODE_TIMING ||
       hmode == DP32G030_TIMERPLUS_MODE_CAPTURE)) {
    t->high_cnt = (t->high_cnt + 1) & 0xFFFFu;
    if (t->high_cnt >= (t->high_load & 0xFFFFu)) {
      t->high_cnt = 0;
      t->iflags |= TP_IF_HIGH_TO;
    }
  }
  if ((t->en & TP_EN_LOW) &&
      (lmode == DP32G030_TIMERPLUS_MODE_TIMING ||
       lmode == DP32G030_TIMERPLUS_MODE_CAPTURE)) {
    t->low_cnt = (t->low_cnt + 1) & 0xFFFFu;
    if (t->low_cnt >= (t->low_load & 0xFFFFu)) {
      t->low_cnt = 0;
      t->iflags |= TP_IF_LOW_TO;
    }
  }
  timer_plus_update_irq(c, n);
}

void dp32g030_timer_plus_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  for (unsigned n = 0; n < 2; n++) {
    dp32g030_timer_plus_t *t = &c->periph.timer_plus[n];
    unsigned hmode = TP_MODE_HIGH(t->ctr);
    unsigned lmode = TP_MODE_LOW(t->ctr);
    bool high_clocked = (t->en & TP_EN_HIGH) &&
                        (hmode == DP32G030_TIMERPLUS_MODE_TIMING ||
                         hmode == DP32G030_TIMERPLUS_MODE_CAPTURE);
    bool low_clocked = (t->en & TP_EN_LOW) &&
                       (lmode == DP32G030_TIMERPLUS_MODE_TIMING ||
                        lmode == DP32G030_TIMERPLUS_MODE_CAPTURE);

    if (high_clocked || low_clocked)
      dp32g030_prescaled_run(c, t->div, &t->presc, cycles, n,
                             timer_plus_tick_one);
  }
}

static uint64_t ticks_to_match(uint32_t cnt, uint32_t load) {
  uint32_t delta = load >= cnt ? load - cnt : 0u;

  return delta ? delta : 1u;
}

uint64_t dp32g030_timer_plus_next_event(const dp32g030_cpu_t *c) {
  uint64_t best = DP32G030_NO_EVENT;

  for (unsigned n = 0; n < 2; n++) {
    const dp32g030_timer_plus_t *t = &c->periph.timer_plus[n];
    unsigned hmode = TP_MODE_HIGH(t->ctr);
    unsigned lmode = TP_MODE_LOW(t->ctr);
    bool high_clocked = (t->en & TP_EN_HIGH) &&
                        (hmode == DP32G030_TIMERPLUS_MODE_TIMING ||
                         hmode == DP32G030_TIMERPLUS_MODE_CAPTURE);
    bool low_clocked = (t->en & TP_EN_LOW) &&
                       (lmode == DP32G030_TIMERPLUS_MODE_TIMING ||
                        lmode == DP32G030_TIMERPLUS_MODE_CAPTURE);
    uint64_t ticks = DP32G030_NO_EVENT;
    uint64_t ev;

    if (!high_clocked && !low_clocked)
      continue;
    if (high_clocked)
      ticks = ticks_to_match(t->high_cnt, t->high_load & 0xFFFFu);
    if (low_clocked) {
      uint64_t lo = ticks_to_match(t->low_cnt, t->low_load & 0xFFFFu);
      if (lo < ticks)
        ticks = lo;
    }
    ev = dp32g030_presc_event(t->presc, t->div, ticks);
    if (ev < best)
      best = ev;
  }
  return best;
}

void dp32g030_timer_plus_set_ext(dp32g030_cpu_t *c, unsigned unit,
                                 unsigned which, bool level) {
  dp32g030_timer_plus_t *t;
  bool old;

  if (unit >= 2 || which >= 2)
    return;
  t = &c->periph.timer_plus[unit];
  old = t->ext_level[which] != 0;
  if (old == level)
    return;
  t->ext_level[which] = level ? 1u : 0u;

  unsigned hmode = TP_MODE_HIGH(t->ctr);
  if (TP_EXTSEL_HIGH(t->ctr) == which &&
      (hmode == DP32G030_TIMERPLUS_MODE_COUNT ||
       hmode == DP32G030_TIMERPLUS_MODE_CAPTURE) &&
      edge_matches(TP_EXTEDGE_HIGH(t->ctr), old, level)) {
    if (hmode == DP32G030_TIMERPLUS_MODE_COUNT) {
      t->high_cnt = (t->high_cnt + 1) & 0xFFFFu;
      if (t->high_cnt >= (t->high_load & 0xFFFFu)) {
        t->high_cnt = 0;
        t->iflags |= TP_IF_HIGH_TO;
      }
    } else {
      t->high_cval = t->high_cnt;
      t->iflags |= level ? TP_IF_HIGH_PR : TP_IF_HIGH_PF;
    }
  }

  unsigned lmode = TP_MODE_LOW(t->ctr);
  if (TP_EXTSEL_LOW(t->ctr) == which &&
      (lmode == DP32G030_TIMERPLUS_MODE_COUNT ||
       lmode == DP32G030_TIMERPLUS_MODE_CAPTURE) &&
      edge_matches(TP_EXTEDGE_LOW(t->ctr), old, level)) {
    if (lmode == DP32G030_TIMERPLUS_MODE_COUNT) {
      t->low_cnt = (t->low_cnt + 1) & 0xFFFFu;
      if (t->low_cnt >= (t->low_load & 0xFFFFu)) {
        t->low_cnt = 0;
        t->iflags |= TP_IF_LOW_TO;
      }
    } else {
      t->low_cval = t->low_cnt;
      t->iflags |= level ? TP_IF_LOW_PR : TP_IF_LOW_PF;
    }
  }
  timer_plus_update_irq(c, unit);
}

void dp32g030_timer_plus_set_hall(dp32g030_cpu_t *c, unsigned unit,
                                  uint32_t mask) {
  dp32g030_timer_plus_t *t;
  uint32_t old, now;

  if (unit >= 2)
    return;
  t = &c->periph.timer_plus[unit];
  old = t->hall_val & 7u;
  now = mask & 7u;
  if (old == now)
    return;
  for (unsigned bit = 0; bit < 3; bit++) {
    bool o = (old >> bit) & 1u, nw = (now >> bit) & 1u;

    if (o == nw)
      continue;
    t->iflags |= nw ? (TP_IF_HALL0_R << (2 * bit)) : (TP_IF_HALL0_F << (2 * bit));
  }
  t->hall_val = now;
  timer_plus_update_irq(c, unit);
}

bool dp32g030_timer_plus_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                              uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_timer_plus_t *t =
      &c->periph.timer_plus[timer_plus_index(r)];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = t->en; break;
  case 0x04: v = t->div; break;
  case 0x08: v = t->ctr; break;
  case 0x10: v = t->ie; break;
  case 0x14: v = t->iflags; break;
  case 0x20: v = t->high_load; break;
  case 0x24: v = t->high_cnt; break;
  case 0x28: v = t->high_cval; break;
  case 0x30: v = t->low_load; break;
  case 0x34: v = t->low_cnt; break;
  case 0x38: v = t->low_cval; break;
  case 0x40: v = t->hall_val; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_timer_plus_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                               uint32_t off, unsigned size, uint32_t val) {
  unsigned n = timer_plus_index(r);
  dp32g030_timer_plus_t *t = &c->periph.timer_plus[n];

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: t->en = val & 3u; break;
  case 0x04: t->div = val & 0xFFFFu; break;
  case 0x08: t->ctr = val & 0x1FF01FFu; break;
  case 0x10: t->ie = val & TP_IF_MASK; break;
  case 0x14: t->iflags &= ~val; break;
  case 0x20: t->high_load = val & 0xFFFFu; break;
  case 0x24: case 0x28: break; /* read-only */
  case 0x30: t->low_load = val & 0xFFFFu; break;
  case 0x34: case 0x38: break; /* read-only */
  case 0x40: break;            /* read-only */
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  timer_plus_update_irq(c, n);
  return true;
}
