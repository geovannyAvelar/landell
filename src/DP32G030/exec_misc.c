#include "exec_internal.h"

#include "bus.h"
#include "exc.h"

#include <stdbool.h>
#include <stdint.h>

DP32G030_EXEC(undef) {
  (void)in;
  dp32g030_hard_fault(c);
}

DP32G030_EXEC(nop) {
  (void)c;
  (void)in;
}

DP32G030_EXEC(sxtb) { c->r[in->rd] = (uint32_t)(int32_t)(int8_t)c->r[in->rm]; }

DP32G030_EXEC(sxth) { c->r[in->rd] = (uint32_t)(int32_t)(int16_t)c->r[in->rm]; }

DP32G030_EXEC(uxtb) { c->r[in->rd] = c->r[in->rm] & 0xFFu; }

DP32G030_EXEC(uxth) { c->r[in->rd] = c->r[in->rm] & 0xFFFFu; }

DP32G030_EXEC(rev) { c->r[in->rd] = __builtin_bswap32(c->r[in->rm]); }

DP32G030_EXEC(rev16) {
  uint32_t x = c->r[in->rm];
  c->r[in->rd] = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
}

DP32G030_EXEC(revsh) {
  uint32_t x = c->r[in->rm];
  uint16_t s = (uint16_t)(((x & 0xFFu) << 8) | ((x >> 8) & 0xFFu));
  c->r[in->rd] = (uint32_t)(int32_t)(int16_t)s;
}

DP32G030_EXEC(wfi) {
  (void)in;
  c->sleeping = true;
}

DP32G030_EXEC(wfe) {
  (void)in;
  if (c->event)
    c->event = false;
  else
    c->sleeping = true;
}

DP32G030_EXEC(sev) {
  (void)in;
  c->event = true;
}

DP32G030_EXEC(svc) {
  (void)in;
  /* SVCall at or below the running priority escalates to HardFault. */
  if (c->scs.prio[DP32G030_EXC_SVCALL] >= dp32g030_exec_priority(c)) {
    dp32g030_hard_fault(c);
    return;
  }
  dp32g030_exc_pend(c, DP32G030_EXC_SVCALL);
}

DP32G030_EXEC(bkpt) {
  (void)in;
  c->halted = true;
}

DP32G030_EXEC(cpsid) {
  (void)in;
  c->primask = 1;
}

DP32G030_EXEC(cpsie) {
  (void)in;
  c->primask = 0;
}

DP32G030_EXEC(mrs) {
  uint32_t v = 0;

  switch (in->imm) {
  case 0:
  case 1:
  case 2:
    v = c->xpsr & 0xF0000000u;
    break;
  case 3:
    v = c->xpsr;
    break;
  case 5:
    v = c->xpsr & 0x3Fu;
    break;
  case 8:
    v = dp32g030_msp_get(c);
    break;
  case 9:
    v = dp32g030_psp_get(c);
    break;
  case 16:
    v = c->primask;
    break;
  case 20:
    v = c->control;
    break;
  default:
    break;
  }
  c->r[in->rd] = v;
}

DP32G030_EXEC(msr) {
  uint32_t v = c->r[in->rn];

  switch (in->imm) {
  case 0:
  case 1:
  case 2:
  case 3:
    c->xpsr = (c->xpsr & 0x0FFFFFFFu) | (v & 0xF0000000u);
    break;
  case 8:
    dp32g030_msp_set(c, v);
    break;
  case 9:
    dp32g030_psp_set(c, v);
    break;
  case 16:
    c->primask = v & 1u;
    break;
  case 20:
    /* SPSEL is writable in thread mode only. */
    if (!c->handler) {
      c->control = v & 2u;
      dp32g030_sp_sync(c);
    }
    break;
  default:
    break;
  }
}

const dp32g030_exec_entry_t dp32g030_exec_misc_entries[] = {
    {OP_UDF, dp32g030_exec_undef},
    {OP_NOP, dp32g030_exec_nop},
    {OP_YIELD, dp32g030_exec_nop},
    {OP_SEV, dp32g030_exec_sev},
    {OP_DMB, dp32g030_exec_nop},
    {OP_DSB, dp32g030_exec_nop},
    {OP_ISB, dp32g030_exec_nop},
    {OP_SXTB, dp32g030_exec_sxtb},
    {OP_SXTH, dp32g030_exec_sxth},
    {OP_UXTB, dp32g030_exec_uxtb},
    {OP_UXTH, dp32g030_exec_uxth},
    {OP_REV, dp32g030_exec_rev},
    {OP_REV16, dp32g030_exec_rev16},
    {OP_REVSH, dp32g030_exec_revsh},
    {OP_WFE, dp32g030_exec_wfe},
    {OP_WFI, dp32g030_exec_wfi},
    {OP_SVC, dp32g030_exec_svc},
    {OP_BKPT, dp32g030_exec_bkpt},
    {OP_CPSID, dp32g030_exec_cpsid},
    {OP_CPSIE, dp32g030_exec_cpsie},
    {OP_MRS, dp32g030_exec_mrs},
    {OP_MSR, dp32g030_exec_msr},
};
const size_t dp32g030_exec_misc_count =
    sizeof dp32g030_exec_misc_entries / sizeof *dp32g030_exec_misc_entries;
