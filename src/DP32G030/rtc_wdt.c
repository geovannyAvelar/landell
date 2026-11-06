#include "periph.h"

#include "exc.h"
#include "soc.h"

/* ---- RTC: BCD calendar clock --------------------------------------- */

#define RTC_CFG_LOAD_EN (1u << 2)
#define RTC_CFG_ALM_EN (1u << 1)
#define RTC_CFG_RTC_EN (1u << 0)

#define RTC_IF_ALM_ERR (1u << 9)
#define RTC_IF_TIME_ERR (1u << 8)
#define RTC_IF_MS (1u << 5)
#define RTC_IF_ALM (1u << 4)
#define RTC_IF_DATE (1u << 3)
#define RTC_IF_HOUR (1u << 2)
#define RTC_IF_MIN (1u << 1)
#define RTC_IF_SEC (1u << 0)
#define RTC_IF_W1C_MASK 0x3Fu /* MS..SEC: write-1-clear; ALM/TIME_ERR are status-only */
#define RTC_IE_MASK 0x3Fu

static bool is_leap(unsigned year) { return year % 4 == 0; }

static unsigned days_in_month(unsigned year, unsigned month) {
  static const unsigned char days[12] = {31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};

  if (month < 1 || month > 12)
    return 31;
  if (month == 2 && is_leap(year))
    return 29;
  return days[month - 1];
}

/* Pack/unpack the split BCD-digit fields used by TR/DR/AR/TSTR/TSDR. */
static unsigned bcd_pack(unsigned tens, unsigned units) {
  return tens * 10u + units;
}

static void rtc_update_irq(dp32g030_cpu_t *c) {
  const dp32g030_rtc_t *rtc = &c->periph.rtc;

  dp32g030_irq_set(c, 2, (rtc->iflags & rtc->ie & RTC_IE_MASK) != 0);
}

/* Try to load `sec/min/hour/weekday` from RTC_TR; returns false (and sets
   TIME_ERR) if any field is out of range. Date load is separate. */
static bool rtc_load_time(dp32g030_rtc_t *rtc) {
  unsigned week = (rtc->tr >> 24) & 7u;
  unsigned hour = bcd_pack((rtc->tr >> 20) & 3u, (rtc->tr >> 16) & 0xFu);
  unsigned min = bcd_pack((rtc->tr >> 12) & 0xFu, (rtc->tr >> 8) & 0xFu);
  unsigned sec = bcd_pack((rtc->tr >> 4) & 0xFu, rtc->tr & 0xFu);

  if (week > 6 || hour > 23 || min > 59 || sec > 59)
    return false;
  rtc->weekday = (uint8_t)week;
  rtc->hour = (uint8_t)hour;
  rtc->min = (uint8_t)min;
  rtc->sec = (uint8_t)sec;
  return true;
}

static bool rtc_load_date(dp32g030_rtc_t *rtc) {
  unsigned year = bcd_pack((rtc->dr >> 20) & 0xFu, (rtc->dr >> 16) & 0xFu);
  unsigned month = bcd_pack((rtc->dr >> 12) & 1u, (rtc->dr >> 8) & 0xFu);
  unsigned date = bcd_pack((rtc->dr >> 4) & 3u, rtc->dr & 0xFu);

  if (month < 1 || month > 12 || date < 1 || date > days_in_month(year, month))
    return false;
  rtc->year = (uint8_t)year;
  rtc->month = (uint8_t)month;
  rtc->date = (uint8_t)date;
  return true;
}

static bool rtc_alarm_valid(const dp32g030_rtc_t *rtc) {
  unsigned hour = bcd_pack((rtc->ar >> 20) & 3u, (rtc->ar >> 16) & 0xFu);
  unsigned min = bcd_pack((rtc->ar >> 12) & 0xFu, (rtc->ar >> 8) & 0xFu);
  unsigned sec = bcd_pack((rtc->ar >> 4) & 0xFu, rtc->ar & 0xFu);

  return hour <= 23 && min <= 59 && sec <= 59;
}

/* Advance the calendar by one second; sets the *_IF bits for every unit
   that rolled over. */
static void rtc_advance_second(dp32g030_cpu_t *c) {
  dp32g030_rtc_t *rtc = &c->periph.rtc;

  rtc->cnt = (rtc->cnt + 1) & 0xFFFFFu;
  rtc->iflags |= RTC_IF_SEC;
  if (++rtc->sec < 60)
    goto check_alarm;
  rtc->sec = 0;
  rtc->iflags |= RTC_IF_MIN;
  if (++rtc->min < 60)
    goto check_alarm;
  rtc->min = 0;
  rtc->iflags |= RTC_IF_HOUR;
  if (++rtc->hour < 24)
    goto check_alarm;
  rtc->hour = 0;
  rtc->weekday = (rtc->weekday + 1) % 7;
  rtc->iflags |= RTC_IF_DATE;
  if (++rtc->date <= days_in_month(rtc->year, rtc->month))
    goto check_alarm;
  rtc->date = 1;
  if (++rtc->month <= 12)
    goto check_alarm;
  rtc->month = 1;
  rtc->year = (rtc->year + 1) % 100;

check_alarm:
  if (!(rtc->cfg & RTC_CFG_ALM_EN))
    return;
  {
    unsigned week_mask = (rtc->ar >> 24) & 0x7Fu;
    unsigned hour = bcd_pack((rtc->ar >> 20) & 3u, (rtc->ar >> 16) & 0xFu);
    unsigned min = bcd_pack((rtc->ar >> 12) & 0xFu, (rtc->ar >> 8) & 0xFu);
    unsigned sec = bcd_pack((rtc->ar >> 4) & 0xFu, rtc->ar & 0xFu);

    if (rtc->hour != hour || rtc->min != min || rtc->sec != sec)
      return;
    if (week_mask != 0 && !((week_mask >> rtc->weekday) & 1u))
      return;
    rtc->iflags |= RTC_IF_ALM;
    if (week_mask == 0)
      rtc->cfg &= ~RTC_CFG_ALM_EN;
  }
}

static void rtc_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  dp32g030_rtc_t *rtc = &c->periph.rtc;
  uint32_t period = (rtc->pre & 0xFFFFFu) + 1u;
  uint32_t half = period / 2u;

  if (!(rtc->cfg & RTC_CFG_RTC_EN))
    return;
  if (rtc->presc == 0 || rtc->presc > period)
    rtc->presc = period;
  while (cycles) {
    uint64_t step = cycles < rtc->presc ? cycles : rtc->presc;
    uint32_t before = rtc->presc;

    rtc->presc -= (uint32_t)step;
    cycles -= step;
    /* Crossing the halfway point (counting down) raises MS_IF once. */
    if (before > half && rtc->presc <= half && rtc->presc != 0)
      rtc->iflags |= RTC_IF_MS;
    if (rtc->presc == 0) {
      rtc->iflags |= RTC_IF_MS;
      rtc_advance_second(c);
      rtc->presc = period;
    }
  }
  rtc_update_irq(c);
}

static uint64_t rtc_next_event(const dp32g030_cpu_t *c) {
  const dp32g030_rtc_t *rtc = &c->periph.rtc;
  uint32_t period = (rtc->pre & 0xFFFFFu) + 1u;
  uint32_t half = period / 2u;
  uint32_t rem = (rtc->presc == 0 || rtc->presc > period) ? period : rtc->presc;

  if (!(rtc->cfg & RTC_CFG_RTC_EN))
    return DP32G030_NO_EVENT;
  if (half && rem > half)
    return rem - half;
  return rem;
}

bool dp32g030_rtc_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_rtc_t *rtc = &c->periph.rtc;
  uint32_t v;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = rtc->cfg; break;
  case 0x04: v = rtc->ie; break;
  case 0x08: v = rtc->iflags; break;
  case 0x10: v = rtc->pre; break;
  case 0x14: v = rtc->tr; break;
  case 0x18: v = rtc->dr; break;
  case 0x1C: v = rtc->ar; break;
  case 0x20:
    v = ((uint32_t)rtc->weekday << 24) | ((uint32_t)(rtc->hour / 10) << 20) |
        ((uint32_t)(rtc->hour % 10) << 16) | ((uint32_t)(rtc->min / 10) << 12) |
        ((uint32_t)(rtc->min % 10) << 8) | ((uint32_t)(rtc->sec / 10) << 4) |
        (uint32_t)(rtc->sec % 10);
    break;
  case 0x24:
    v = ((uint32_t)(is_leap(rtc->year) ? 1 : 0) << 24) |
        ((uint32_t)(rtc->year / 10) << 20) | ((uint32_t)(rtc->year % 10) << 16) |
        ((uint32_t)(rtc->month / 10) << 12) | ((uint32_t)(rtc->month % 10) << 8) |
        ((uint32_t)(rtc->date / 10) << 4) | (uint32_t)(rtc->date % 10);
    break;
  case 0x28: v = rtc->cnt; break;
  case 0x2C: v = (rtc->cfg & RTC_CFG_RTC_EN) ? 1u : 0u; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_rtc_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  dp32g030_rtc_t *rtc = &c->periph.rtc;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00:
    rtc->cfg = val & (RTC_CFG_ALM_EN | RTC_CFG_RTC_EN);
    if (val & RTC_CFG_LOAD_EN) {
      bool ok_t = rtc_load_time(rtc);
      bool ok_d = rtc_load_date(rtc);

      rtc->iflags = (rtc->iflags & ~RTC_IF_TIME_ERR) |
                    (!ok_t || !ok_d ? RTC_IF_TIME_ERR : 0u);
    }
    if (rtc->cfg & RTC_CFG_ALM_EN)
      rtc->iflags = (rtc->iflags & ~RTC_IF_ALM_ERR) |
                    (rtc_alarm_valid(rtc) ? 0u : RTC_IF_ALM_ERR);
    break;
  case 0x04: rtc->ie = val & RTC_IE_MASK; break;
  case 0x08: rtc->iflags &= ~(val & RTC_IF_W1C_MASK); break;
  case 0x10: rtc->pre = val & 0x1FFFFFFu; break;
  case 0x14: rtc->tr = val & 0x7FFFFFFu; break;
  case 0x18: rtc->dr = val & 0xFFFFFFu; break;
  case 0x1C: rtc->ar = val & 0x7FFFFFFFu; break;
  case 0x20: case 0x24: case 0x28: case 0x2C: break; /* read-only */
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  rtc_update_irq(c);
  return true;
}

/* ---- IWDT: 20-bit down-counter watchdog ----------------------------- */

#define IWDT_CTRL_INTEN (1u << 1)
#define IWDT_CTRL_EN (1u << 0)

static void iwdt_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  dp32g030_iwdt_t *w = &c->periph.iwdt;

  if (!(w->ctrl & IWDT_CTRL_EN))
    return;
  while (cycles) {
    uint64_t step = cycles < w->cnt ? cycles : w->cnt;

    w->cnt -= (uint32_t)step;
    cycles -= step;
    if (w->cnt == 0) {
      if (w->expired_once) {
        c->reset_req = true;
        return;
      }
      w->expired_once = true;
      w->iflags |= 1u;
      w->cnt = w->load & 0xFFFFFu;
      dp32g030_irq_set(c, 1, (w->iflags & 1u) && (w->ctrl & IWDT_CTRL_INTEN));
    }
  }
}

static uint64_t iwdt_next_event(const dp32g030_cpu_t *c) {
  const dp32g030_iwdt_t *w = &c->periph.iwdt;

  if (!(w->ctrl & IWDT_CTRL_EN))
    return DP32G030_NO_EVENT;
  return w->cnt ? w->cnt : 1u;
}

bool dp32g030_iwdt_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_iwdt_t *w = &c->periph.iwdt;
  uint32_t v;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = w->load; break;
  case 0x08: v = w->ctrl; break;
  case 0x0C: v = w->iflags; break;
  case 0x10: v = 0; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_iwdt_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val) {
  dp32g030_iwdt_t *w = &c->periph.iwdt;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00: w->load = val & 0xFFFFFu; break;
  case 0x08: {
    uint32_t new_ctrl = val & 3u;

    if ((new_ctrl & IWDT_CTRL_EN) && !(w->ctrl & IWDT_CTRL_EN)) {
      w->cnt = w->load & 0xFFFFFu;
      w->expired_once = false;
    }
    w->ctrl = new_ctrl;
    break;
  }
  case 0x0C: w->iflags &= ~val; break;
  case 0x10:
    if ((val & 0xFFu) == 0x55u) {
      w->cnt = w->load & 0xFFFFFu;
      w->expired_once = false;
    }
    break;
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  dp32g030_irq_set(c, 1, (w->iflags & 1u) && (w->ctrl & IWDT_CTRL_INTEN));
  return true;
}

/* ---- WWDT: windowed 7-bit down-counter watchdog --------------------- */

#define WWDT_CTRL_PRERSTINTEN (1u << 4)
#define WWDT_CTRL_INTEN (1u << 3)
#define WWDT_CTRL_EN (1u << 0)
#define WWDT_IF_PRERST (1u << 1)
#define WWDT_IF_WIN (1u << 0)

static uint32_t wwdt_period(const dp32g030_wwdt_t *w) {
  return 4096u << ((w->ctrl >> 1) & 3u);
}

static unsigned wwdt_rst_load(const dp32g030_wwdt_t *w) {
  return (w->load >> 8) & 0x3Fu;
}

static unsigned wwdt_int_load(const dp32g030_wwdt_t *w) {
  return w->load & 0x7Fu;
}

static void wwdt_update_irq(dp32g030_cpu_t *c) {
  const dp32g030_wwdt_t *w = &c->periph.wwdt;
  bool level = ((w->iflags & WWDT_IF_PRERST) && (w->ctrl & WWDT_CTRL_PRERSTINTEN)) ||
               ((w->iflags & WWDT_IF_WIN) && (w->ctrl & WWDT_CTRL_INTEN));

  dp32g030_irq_set(c, 0, level);
}

static void wwdt_step(dp32g030_cpu_t *c) {
  dp32g030_wwdt_t *w = &c->periph.wwdt;
  unsigned rst = wwdt_rst_load(w), win = wwdt_int_load(w);

  if (w->value == 0) {
    c->reset_req = true;
    return;
  }
  w->value--;
  if (w->value == rst) {
    c->reset_req = true;
    return;
  }
  if (w->value == rst + 1)
    w->iflags |= WWDT_IF_PRERST;
  if (w->value == win)
    w->iflags |= WWDT_IF_WIN;
  wwdt_update_irq(c);
}

static void wwdt_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  dp32g030_wwdt_t *w = &c->periph.wwdt;
  uint32_t period = wwdt_period(w);

  if (!(w->ctrl & WWDT_CTRL_EN))
    return;
  if (w->presc == 0 || w->presc > period)
    w->presc = period;
  while (cycles) {
    uint64_t step = cycles < w->presc ? cycles : w->presc;

    w->presc -= (uint32_t)step;
    cycles -= step;
    if (w->presc == 0) {
      wwdt_step(c);
      if (c->reset_req)
        return;
      w->presc = period;
    }
  }
}

static uint64_t wwdt_next_event(const dp32g030_cpu_t *c) {
  const dp32g030_wwdt_t *w = &c->periph.wwdt;
  uint32_t period = wwdt_period(w);
  uint32_t rem = (w->presc == 0 || w->presc > period) ? period : w->presc;

  if (!(w->ctrl & WWDT_CTRL_EN))
    return DP32G030_NO_EVENT;
  return rem;
}

bool dp32g030_wwdt_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out) {
  const dp32g030_wwdt_t *w = &c->periph.wwdt;
  uint32_t v;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = w->load; break;
  case 0x04: v = w->value; break;
  case 0x08: v = w->ctrl; break;
  case 0x0C: v = w->iflags; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_wwdt_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val) {
  dp32g030_wwdt_t *w = &c->periph.wwdt;

  (void)r;
  if (size != 4)
    return false;
  switch (off) {
  case 0x00: w->load = val & 0x3F7Fu; break;
  case 0x04: break; /* read-only */
  case 0x08: w->ctrl = val & 0x1Fu; break;
  case 0x0C: w->iflags &= ~val; break;
  case 0x10:
    if ((val & 0xFFu) == 0x55u) {
      unsigned rst = wwdt_rst_load(w), win = wwdt_int_load(w);

      if (w->value > rst && w->value < win)
        w->value = 0x7Fu;
      else
        c->reset_req = true;
    }
    break;
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  wwdt_update_irq(c);
  return true;
}

/* ---- combined tick / next-event ------------------------------------ */

void dp32g030_rtc_wdt_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  rtc_tick(c, cycles);
  iwdt_tick(c, cycles);
  if (!c->reset_req)
    wwdt_tick(c, cycles);
}

uint64_t dp32g030_rtc_wdt_next_event(const dp32g030_cpu_t *c) {
  uint64_t a = rtc_next_event(c);
  uint64_t b = iwdt_next_event(c);
  uint64_t d = wwdt_next_event(c);

  if (b < a)
    a = b;
  return d < a ? d : a;
}
