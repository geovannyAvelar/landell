#include "soc.h"

#include "exc.h"
#include "mmio.h"
#include "socdesc.h"

#define CPUID_CORTEX_M0 0x410CC200u
#define CPUID_CORTEX_M0_PLUS 0x410CC601u
#define AIRCR_VECTKEY 0x05FA0000u

#define ST_ENABLE (1u << 0)
#define ST_TICKINT (1u << 1)
#define ST_CLKSOURCE (1u << 2)
#define ST_COUNTFLAG (1u << 16)
#define ST_MASK 0x00FFFFFFu

void dp32g030_scs_reset(dp32g030_cpu_t *c) {
  c->scs.pending = 0;
  c->scs.active = 0;
  c->scs.nvic_enabled = 0;
  for (size_t i = 0; i < sizeof c->scs.prio; i++)
    c->scs.prio[i] = 0;
  c->scs.scr = 0;
  c->scs.st_ctrl = 0;
  c->scs.st_load = 0;
  c->scs.st_val = 0;
  /* irq_lines belongs to peripherals: re-latch lines that stay high. */
  c->scs.pending = (uint64_t)c->scs.irq_lines << 16;
}

void dp32g030_systick_tick(dp32g030_cpu_t *c, uint64_t n) {
  dp32g030_scs_t *s = &c->scs;
  uint32_t val = s->st_val;

  if (!(s->st_ctrl & ST_ENABLE))
    return;

  while (n) {
    if (val == 0) {
      if (s->st_load == 0)
        break;
      val = s->st_load;
      n--;
      continue;
    }
    uint64_t step = n < val ? n : val;
    val -= (uint32_t)step;
    n -= step;
    if (val == 0) {
      s->st_ctrl |= ST_COUNTFLAG;
      if (s->st_ctrl & ST_TICKINT)
        dp32g030_exc_pend(c, DP32G030_EXC_SYSTICK);
    }
  }
  s->st_val = val;
}

uint64_t dp32g030_systick_next_event(const dp32g030_cpu_t *c) {
  const dp32g030_scs_t *s = &c->scs;

  if ((s->st_ctrl & (ST_ENABLE | ST_TICKINT)) != (ST_ENABLE | ST_TICKINT))
    return DP32G030_NO_EVENT;
  if (s->st_val)
    return s->st_val;
  if (s->st_load == 0)
    return DP32G030_NO_EVENT;
  return (uint64_t)s->st_load + 1;
}

static bool dp32g030_scs_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                              uint32_t off, unsigned size, uint32_t *out) {
  dp32g030_scs_t *s = &c->scs;
  uint32_t v = 0;

  (void)r;
  if (size != 4)
    return false;

  switch (off) {
  case 0x010:
    v = s->st_ctrl;
    s->st_ctrl &= ~ST_COUNTFLAG;
    break;
  case 0x014:
    v = s->st_load;
    break;
  case 0x018:
    v = s->st_val;
    break;
  case 0x100:
  case 0x180:
    v = s->nvic_enabled;
    break;
  case 0x200:
  case 0x280:
    v = (uint32_t)(s->pending >> 16);
    break;
  case 0xD00:
    v = c->soc->has_vtor ? CPUID_CORTEX_M0_PLUS : CPUID_CORTEX_M0;
    break;
  case 0xD08:
    v = c->soc->has_vtor ? c->vtor : 0;
    break;
  case 0xD04:
    v = (c->xpsr & 0x1FFu) | (dp32g030_exc_vectpending(c) << 12);
    if (s->pending >> 16)
      v |= 1u << 22;
    if (s->pending & (1ull << DP32G030_EXC_SYSTICK))
      v |= 1u << 26;
    if (s->pending & (1ull << DP32G030_EXC_PENDSV))
      v |= 1u << 28;
    if (s->pending & (1ull << DP32G030_EXC_NMI))
      v |= 1u << 31;
    break;
  case 0xD0C:
    v = 0xFA050000u;
    break;
  case 0xD10:
    v = s->scr;
    break;
  case 0xD14:
    v = 1u << 9; /* STKALIGN is fixed */
    break;
  case 0xD1C:
    v = (uint32_t)(s->prio[DP32G030_EXC_SVCALL] & 0xC0) << 24;
    break;
  case 0xD20:
    v = ((uint32_t)(s->prio[DP32G030_EXC_PENDSV] & 0xC0) << 16) |
        ((uint32_t)(s->prio[DP32G030_EXC_SYSTICK] & 0xC0) << 24);
    break;
  default:
    if (off >= 0x400 && off < 0x420) {
      unsigned base = DP32G030_EXC_IRQ0 + (off - 0x400);
      for (unsigned i = 0; i < 4; i++)
        v |= (uint32_t)(s->prio[base + i] & 0xC0) << (8 * i);
    }
    break; /* everything else in the SCS reads as zero */
  }
  *out = v;
  return true;
}

static bool dp32g030_scs_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                               uint32_t off, unsigned size, uint32_t v) {
  dp32g030_scs_t *s = &c->scs;

  (void)r;
  if (size != 4)
    return false;

  switch (off) {
  case 0x010:
    s->st_ctrl = (s->st_ctrl & ST_COUNTFLAG) | (v & (ST_ENABLE | ST_TICKINT |
                                                      ST_CLKSOURCE));
    break;
  case 0x014:
    s->st_load = v & ST_MASK;
    break;
  case 0x018:
    s->st_val = 0;
    s->st_ctrl &= ~ST_COUNTFLAG;
    break;
  case 0x100:
    s->nvic_enabled |= v;
    break;
  case 0x180:
    s->nvic_enabled &= ~v;
    break;
  case 0x200:
    s->pending |= (uint64_t)v << 16;
    break;
  case 0x280:
    s->pending &= ~((uint64_t)v << 16);
    break;
  case 0xD04:
    if (v & (1u << 31))
      dp32g030_exc_pend(c, DP32G030_EXC_NMI);
    if (v & (1u << 28))
      dp32g030_exc_pend(c, DP32G030_EXC_PENDSV);
    if (v & (1u << 27))
      s->pending &= ~(1ull << DP32G030_EXC_PENDSV);
    if (v & (1u << 26))
      dp32g030_exc_pend(c, DP32G030_EXC_SYSTICK);
    if (v & (1u << 25))
      s->pending &= ~(1ull << DP32G030_EXC_SYSTICK);
    break;
  case 0xD08:
    if (c->soc->has_vtor)
      c->vtor = v & 0xFFFFFF80u;
    break;
  case 0xD0C:
    if ((v & 0xFFFF0000u) == AIRCR_VECTKEY && (v & 4u))
      c->reset_req = true;
    break;
  case 0xD10:
    s->scr = v & 0x16u;
    break;
  case 0xD1C:
    s->prio[DP32G030_EXC_SVCALL] = (uint8_t)((v >> 24) & 0xC0);
    break;
  case 0xD20:
    s->prio[DP32G030_EXC_PENDSV] = (uint8_t)((v >> 16) & 0xC0);
    s->prio[DP32G030_EXC_SYSTICK] = (uint8_t)((v >> 24) & 0xC0);
    break;
  default:
    if (off >= 0x400 && off < 0x420) {
      unsigned base = DP32G030_EXC_IRQ0 + (off - 0x400);
      for (unsigned i = 0; i < 4; i++)
        s->prio[base + i] = (uint8_t)((v >> (8 * i)) & 0xC0);
    }
    break; /* writes elsewhere are ignored */
  }
  return true;
}

const dp32g030_region_t dp32g030_scs_region = {
    "SCS", 0xE000E000u, 0x1000u, -1, -1, dp32g030_scs_read, dp32g030_scs_write};
