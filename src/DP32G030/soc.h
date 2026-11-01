#pragma once

#include "DP32G030.h"

#include <stdint.h>

#define DP32G030_NO_EVENT UINT64_MAX

/* Advance all time-based blocks (SysTick, timers) by `cycles` core clocks. */
void dp32g030_soc_tick(dp32g030_cpu_t *c, uint64_t cycles);
/* Core clocks until the next event that can raise an interrupt. */
uint64_t dp32g030_soc_next_event(const dp32g030_cpu_t *c);

/* Cortex-M0 SCS (SysTick, NVIC, SCB). */
void dp32g030_scs_reset(dp32g030_cpu_t *c);
void dp32g030_systick_tick(dp32g030_cpu_t *c, uint64_t cycles);
uint64_t dp32g030_systick_next_event(const dp32g030_cpu_t *c);

/* Approximate Cortex-M0 cycle cost of an instruction. */
unsigned dp32g030_insn_cycles(const dp32g030_insn_t *in, bool taken);
