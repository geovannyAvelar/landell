#include "exc.h"

#include "bus.h"

#include <limits.h>

#define EXC_ENTRY_CYCLES 16u
#define XPSR_IPSR 0x3Fu
#define XPSR_FLAGS 0xF0000000u
#define XPSR_THUMB (1u << 24)
#define XPSR_ALIGN (1u << 9)

static const uint64_t dp32g030_sys_exc_mask = (1ull << DP32G030_EXC_NMI) |
                                              (1ull << DP32G030_EXC_HARDFAULT) |
                                              (1ull << DP32G030_EXC_SVCALL) |
                                              (1ull << DP32G030_EXC_PENDSV) |
                                              (1ull << DP32G030_EXC_SYSTICK);

static void dp32g030_lockup(dp32g030_cpu_t *c) {
  c->locked_up = true;
  c->halted = true;
}

static int dp32g030_exc_prio(const dp32g030_cpu_t *c, unsigned n) {
  switch (n) {
  case 1:
    return -3;
  case 2:
    return -2;
  case 3:
    return -1;
  default:
    return c->scs.prio[n] & 0xC0;
  }
}

static uint64_t dp32g030_exc_enabled(const dp32g030_cpu_t *c) {
  return dp32g030_sys_exc_mask | ((uint64_t)c->scs.nvic_enabled << 16);
}

int dp32g030_exec_priority(const dp32g030_cpu_t *c) {
  int p = 256;
  uint64_t a = c->scs.active;

  while (a) {
    unsigned n = (unsigned)__builtin_ctzll(a);
    int q = dp32g030_exc_prio(c, n);
    if (q < p)
      p = q;
    a &= a - 1;
  }
  if (c->primask && p > 0)
    p = 0;
  return p;
}

static int dp32g030_exc_best(const dp32g030_cpu_t *c, int limit) {
  uint64_t cand = c->scs.pending & dp32g030_exc_enabled(c);
  int best = -1;
  int bestp = limit;

  while (cand) {
    unsigned n = (unsigned)__builtin_ctzll(cand);
    int p = dp32g030_exc_prio(c, n);
    if (p < bestp) {
      best = (int)n;
      bestp = p;
    }
    cand &= cand - 1;
  }
  return best;
}

int dp32g030_exc_pick(const dp32g030_cpu_t *c) {
  if (!c->scs.pending)
    return -1;
  return dp32g030_exc_best(c, dp32g030_exec_priority(c));
}

bool dp32g030_exc_wake(const dp32g030_cpu_t *c) {
  return (c->scs.pending & dp32g030_exc_enabled(c)) != 0;
}

unsigned dp32g030_exc_vectpending(const dp32g030_cpu_t *c) {
  int n = dp32g030_exc_best(c, INT_MAX);
  return n < 0 ? 0u : (unsigned)n;
}

void dp32g030_exc_pend(dp32g030_cpu_t *c, unsigned exc) {
  c->scs.pending |= 1ull << exc;
}

void dp32g030_irq_set(dp32g030_cpu_t *c, unsigned irq, bool level) {
  uint32_t bit = 1u << (irq & 31u);

  if (level) {
    if (!(c->scs.irq_lines & bit))
      c->scs.pending |= (uint64_t)bit << 16;
    c->scs.irq_lines |= bit;
  } else {
    c->scs.irq_lines &= ~bit;
  }
}

uint32_t dp32g030_msp_get(const dp32g030_cpu_t *c) {
  return c->using_psp ? c->msp : c->r[13];
}

uint32_t dp32g030_psp_get(const dp32g030_cpu_t *c) {
  return c->using_psp ? c->r[13] : c->psp;
}

void dp32g030_msp_set(dp32g030_cpu_t *c, uint32_t v) {
  v &= ~3u;
  if (c->using_psp)
    c->msp = v;
  else
    c->r[13] = v;
}

void dp32g030_psp_set(dp32g030_cpu_t *c, uint32_t v) {
  v &= ~3u;
  if (c->using_psp)
    c->r[13] = v;
  else
    c->psp = v;
}

void dp32g030_sp_sync(dp32g030_cpu_t *c) {
  bool want_psp = !c->handler && (c->control & 2u) != 0;

  if (want_psp == c->using_psp)
    return;
  if (c->using_psp) {
    c->psp = c->r[13];
    c->r[13] = c->msp;
  } else {
    c->msp = c->r[13];
    c->r[13] = c->psp;
  }
  c->using_psp = want_psp;
}

void dp32g030_hard_fault(dp32g030_cpu_t *c) {
  uint32_t vec;

  if (c->halted)
    return;
  /* A fault while already at HardFault/NMI priority locks the core up. */
  if (dp32g030_exec_priority(c) <= -1) {
    dp32g030_lockup(c);
    return;
  }
  /* Stack the address of the faulting instruction. */
  c->r[15] = c->insn_pc;
  if (!dp32g030_bus_read(c, 4u * DP32G030_EXC_HARDFAULT, 4, &vec) ||
      !(vec & 1u)) {
    dp32g030_lockup(c);
    return;
  }
  dp32g030_exc_enter(c, DP32G030_EXC_HARDFAULT);
}

void dp32g030_exc_enter(dp32g030_cpu_t *c, unsigned exc) {
  uint32_t vec;
  uint32_t sp = c->r[13];
  uint32_t aligned = (sp & 4u) ? XPSR_ALIGN : 0;
  uint32_t excret;
  uint32_t frame[8];

  if (!dp32g030_bus_read(c, 4u * exc, 4, &vec) || !(vec & 1u)) {
    dp32g030_lockup(c);
    return;
  }

  if (c->handler)
    excret = DP32G030_EXC_RETURN_HANDLER;
  else if (c->using_psp)
    excret = DP32G030_EXC_RETURN_THREAD_PSP;
  else
    excret = DP32G030_EXC_RETURN_THREAD_MSP;

  frame[0] = c->r[0];
  frame[1] = c->r[1];
  frame[2] = c->r[2];
  frame[3] = c->r[3];
  frame[4] = c->r[12];
  frame[5] = c->r[14];
  frame[6] = c->r[15];
  frame[7] = (c->xpsr & ~XPSR_ALIGN) | aligned;

  sp = (sp - 0x20u) & ~4u;
  for (unsigned i = 0; i < 8; i++) {
    if (!dp32g030_bus_write(c, sp + 4u * i, 4, frame[i])) {
      dp32g030_lockup(c);
      return;
    }
  }

  if (c->using_psp) {
    c->psp = sp;
    c->r[13] = c->msp;
    c->using_psp = false;
  } else {
    c->r[13] = sp;
  }

  c->handler = true;
  c->r[14] = excret;
  c->xpsr = (c->xpsr & ~XPSR_IPSR) | exc | XPSR_THUMB;
  c->scs.active |= 1ull << exc;
  c->scs.pending &= ~(1ull << exc);
  c->r[15] = vec & ~1u;
  c->sleeping = false;
  c->cycles += EXC_ENTRY_CYCLES;
}

void dp32g030_exc_return(dp32g030_cpu_t *c, uint32_t exc_return) {
  uint32_t frame[8];
  uint32_t sp;
  unsigned old = c->xpsr & XPSR_IPSR;

  if (!c->handler || (exc_return != DP32G030_EXC_RETURN_HANDLER &&
                      exc_return != DP32G030_EXC_RETURN_THREAD_MSP &&
                      exc_return != DP32G030_EXC_RETURN_THREAD_PSP)) {
    dp32g030_hard_fault(c);
    return;
  }

  sp = (exc_return == DP32G030_EXC_RETURN_THREAD_PSP) ? c->psp : c->r[13];
  for (unsigned i = 0; i < 8; i++) {
    if (!dp32g030_bus_read(c, sp + 4u * i, 4, &frame[i])) {
      dp32g030_lockup(c);
      return;
    }
  }

  c->scs.active &= ~(1ull << old);
  if (old >= DP32G030_EXC_IRQ0 &&
      (c->scs.irq_lines >> (old - DP32G030_EXC_IRQ0)) & 1u)
    c->scs.pending |= 1ull << old;

  c->r[0] = frame[0];
  c->r[1] = frame[1];
  c->r[2] = frame[2];
  c->r[3] = frame[3];
  c->r[12] = frame[4];
  c->r[14] = frame[5];
  c->r[15] = frame[6] & ~1u;
  c->xpsr = (frame[7] & (XPSR_FLAGS | XPSR_IPSR)) | XPSR_THUMB;
  sp += 0x20u + ((frame[7] & XPSR_ALIGN) ? 4u : 0u);

  if (exc_return == DP32G030_EXC_RETURN_HANDLER) {
    c->r[13] = sp;
    return;
  }

  c->handler = false;
  if (exc_return == DP32G030_EXC_RETURN_THREAD_PSP) {
    c->msp = c->r[13];
    c->r[13] = sp;
    c->using_psp = true;
    c->control |= 2u;
  } else {
    c->r[13] = sp;
    c->using_psp = false;
    c->control &= ~2u;
  }

  if ((c->scs.scr & 2u) && !c->scs.pending)
    c->sleeping = true;
}
