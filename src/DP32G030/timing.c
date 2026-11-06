#include "soc.h"

#include "periph.h"
#include "socdesc.h"

#include <stdint.h>

#ifndef DP32G030_MUL_CYCLES
#define DP32G030_MUL_CYCLES 1u
#endif

static unsigned dp32g030_popcount16(uint16_t v) {
  return (unsigned)__builtin_popcount(v);
}

unsigned dp32g030_insn_cycles(const dp32g030_insn_t *in, bool taken) {
  switch (in->op) {
  case OP_LDR_IMM:
  case OP_STR_IMM:
  case OP_LDRB_IMM:
  case OP_STRB_IMM:
  case OP_LDRH_IMM:
  case OP_STRH_IMM:
  case OP_LDR_SP:
  case OP_STR_SP:
  case OP_LDR_LIT:
  case OP_LDR_REG:
  case OP_STR_REG:
  case OP_LDRB_REG:
  case OP_STRB_REG:
  case OP_LDRH_REG:
  case OP_STRH_REG:
  case OP_LDRSB_REG:
  case OP_LDRSH_REG:
    return 2;
  case OP_LDM:
  case OP_STM:
    return 1 + dp32g030_popcount16(in->reglist);
  case OP_PUSH:
    return 1 + dp32g030_popcount16(in->reglist);
  case OP_POP:
    return 1 + dp32g030_popcount16(in->reglist) +
           ((in->reglist & 0x8000u) ? 3u : 0u);
  case OP_B:
    return 3;
  case OP_B_COND:
    return taken ? 3 : 1;
  case OP_BL:
    return 4;
  case OP_BX:
  case OP_BLX_REG:
    return 3;
  case OP_MRS:
  case OP_MSR:
  case OP_DMB:
  case OP_DSB:
  case OP_ISB:
    return 4;
  case OP_MULS:
    return DP32G030_MUL_CYCLES;
  default:
    return 1;
  }
}

void dp32g030_soc_tick(dp32g030_cpu_t *c, uint64_t cycles) {
  dp32g030_systick_tick(c, cycles);
  dp32g030_uart_tick(c);
  dp32g030_timer_pwm_tick(c, cycles);
  dp32g030_rtc_wdt_tick(c, cycles);
  if (c->soc->tick)
    c->soc->tick(c, cycles);
}

uint64_t dp32g030_soc_next_event(const dp32g030_cpu_t *c) {
  uint64_t a = dp32g030_systick_next_event(c);
  uint64_t b = dp32g030_uart_next_event(c);
  uint64_t d = dp32g030_timer_pwm_next_event(c);
  uint64_t e = dp32g030_rtc_wdt_next_event(c);

  if (b < a)
    a = b;
  if (d < a)
    a = d;
  if (e < a)
    a = e;
  if (c->soc->next_event) {
    uint64_t f = c->soc->next_event(c);

    if (f < a)
      a = f;
  }
  return a;
}
