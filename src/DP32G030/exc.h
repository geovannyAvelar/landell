#pragma once

#include "DP32G030.h"

#include <stdint.h>

#define DP32G030_EXC_NMI 2u
#define DP32G030_EXC_HARDFAULT 3u
#define DP32G030_EXC_SVCALL 11u
#define DP32G030_EXC_PENDSV 14u
#define DP32G030_EXC_SYSTICK 15u
#define DP32G030_EXC_IRQ0 16u

#define DP32G030_EXC_RETURN_HANDLER 0xFFFFFFF1u
#define DP32G030_EXC_RETURN_THREAD_MSP 0xFFFFFFF9u
#define DP32G030_EXC_RETURN_THREAD_PSP 0xFFFFFFFDu

void dp32g030_hard_fault(dp32g030_cpu_t *c);
void dp32g030_exc_enter(dp32g030_cpu_t *c, unsigned exc);
void dp32g030_exc_return(dp32g030_cpu_t *c, uint32_t exc_return);

/* Pending exception that may be taken now, or -1. */
int dp32g030_exc_pick(const dp32g030_cpu_t *c);
/* True if an enabled exception is pending, even if masked (wakes WFI). */
bool dp32g030_exc_wake(const dp32g030_cpu_t *c);
/* Highest-priority pending enabled exception ignoring masking, or 0. */
unsigned dp32g030_exc_vectpending(const dp32g030_cpu_t *c);
/* Priority of the running context; lower is more urgent. */
int dp32g030_exec_priority(const dp32g030_cpu_t *c);
/* Request exception `exc` through the pending latch. */
void dp32g030_exc_pend(dp32g030_cpu_t *c, unsigned exc);

/* Re-select the active stack pointer after mode or CONTROL changes. */
void dp32g030_sp_sync(dp32g030_cpu_t *c);
uint32_t dp32g030_msp_get(const dp32g030_cpu_t *c);
uint32_t dp32g030_psp_get(const dp32g030_cpu_t *c);
void dp32g030_msp_set(dp32g030_cpu_t *c, uint32_t v);
void dp32g030_psp_set(dp32g030_cpu_t *c, uint32_t v);
