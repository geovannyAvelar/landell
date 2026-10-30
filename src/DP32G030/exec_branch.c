#include "exec_internal.h"

#include "bus.h"
#include "exc.h"

#include <stdbool.h>
#include <stdint.h>

static bool dp32g030_cond(const dp32g030_cpu_t *c, unsigned cond) {
  bool n = dp32g030_flag(c, DP32G030_FLAG_N);
  bool z = dp32g030_flag(c, DP32G030_FLAG_Z);
  bool cf = dp32g030_flag(c, DP32G030_FLAG_C);
  bool v = dp32g030_flag(c, DP32G030_FLAG_V);

  switch (cond) {
  case 0x0:
    return z;
  case 0x1:
    return !z;
  case 0x2:
    return cf;
  case 0x3:
    return !cf;
  case 0x4:
    return n;
  case 0x5:
    return !n;
  case 0x6:
    return v;
  case 0x7:
    return !v;
  case 0x8:
    return cf && !z;
  case 0x9:
    return !cf || z;
  case 0xA:
    return n == v;
  case 0xB:
    return n != v;
  case 0xC:
    return !z && n == v;
  case 0xD:
    return z || n != v;
  default:
    return false;
  }
}

void dp32g030_bx_write(dp32g030_cpu_t *c, uint32_t target) {
  if ((target >> 4) == 0x0FFFFFFFu)
    dp32g030_exc_return(c, target);
  else if (!(target & 1))
    dp32g030_hard_fault(c);
  else
    DP32G030_PC(c) = target & ~1u;
}

DP32G030_EXEC(b_cond) {
  if (dp32g030_cond(c, in->cond))
    DP32G030_PC(c) = c->insn_pc + 4 + in->imm;
}

DP32G030_EXEC(b) { DP32G030_PC(c) = c->insn_pc + 4 + in->imm; }

DP32G030_EXEC(bl) {
  DP32G030_LR(c) = DP32G030_PC(c) | 1u;
  DP32G030_PC(c) = c->insn_pc + 4 + in->imm;
}

DP32G030_EXEC(bx) { dp32g030_bx_write(c, dp32g030_reg(c, in->rm)); }

DP32G030_EXEC(blx_reg) {
  uint32_t target = dp32g030_reg(c, in->rm);

  DP32G030_LR(c) = DP32G030_PC(c) | 1u;
  dp32g030_bx_write(c, target);
}

const dp32g030_exec_entry_t dp32g030_exec_branch_entries[] = {
    {OP_B_COND, dp32g030_exec_b_cond},
    {OP_B, dp32g030_exec_b},
    {OP_BL, dp32g030_exec_bl},
    {OP_BX, dp32g030_exec_bx},
    {OP_BLX_REG, dp32g030_exec_blx_reg},
};
const size_t dp32g030_exec_branch_count =
    sizeof dp32g030_exec_branch_entries / sizeof *dp32g030_exec_branch_entries;
