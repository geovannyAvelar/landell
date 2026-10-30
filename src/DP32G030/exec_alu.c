#include "exec_internal.h"

#include "bus.h"
#include "exc.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  DP32G030_SHIFT_LSL,
  DP32G030_SHIFT_LSR,
  DP32G030_SHIFT_ASR,
  DP32G030_SHIFT_ROR,
} dp32g030_shift_t;

static void dp32g030_logic(dp32g030_cpu_t *c, unsigned rd, uint32_t v) {
  c->r[rd] = v;
  dp32g030_set_nz(c, v);
}

static uint32_t dp32g030_add_c(dp32g030_cpu_t *c, uint32_t x, uint32_t y, uint32_t cin,
                               bool setf) {
  uint64_t u = (uint64_t)x + y + cin;
  uint32_t r = (uint32_t)u;

  if (setf) {
    dp32g030_set_nz(c, r);
    dp32g030_set_flag(c, DP32G030_FLAG_C, (u >> 32) != 0);
    dp32g030_set_flag(c, DP32G030_FLAG_V, (((x ^ r) & (y ^ r)) >> 31) != 0);
  }
  return r;
}

static uint32_t dp32g030_shift(dp32g030_cpu_t *c, dp32g030_shift_t kind, uint32_t x,
                               uint32_t n) {
  bool carry = dp32g030_flag(c, DP32G030_FLAG_C);
  uint32_t r = x;

  if (n != 0) {
    switch (kind) {
    case DP32G030_SHIFT_LSL:
      if (n < 32) {
        r = x << n;
        carry = ((x >> (32 - n)) & 1) != 0;
      } else {
        r = 0;
        carry = (n == 32) && (x & 1);
      }
      break;
    case DP32G030_SHIFT_LSR:
      if (n < 32) {
        r = x >> n;
        carry = ((x >> (n - 1)) & 1) != 0;
      } else {
        r = 0;
        carry = (n == 32) && (x >> 31);
      }
      break;
    case DP32G030_SHIFT_ASR:
      if (n < 32) {
        r = (uint32_t)((int32_t)x >> n);
        carry = ((x >> (n - 1)) & 1) != 0;
      } else {
        r = (x >> 31) ? 0xFFFFFFFFu : 0;
        carry = (x >> 31) != 0;
      }
      break;
    case DP32G030_SHIFT_ROR: {
      uint32_t m = n & 31;
      if (m == 0) {
        carry = (x >> 31) != 0;
      } else {
        r = (x >> m) | (x << (32 - m));
        carry = (r >> 31) != 0;
      }
      break;
    }
    }
  }

  dp32g030_set_nz(c, r);
  dp32g030_set_flag(c, DP32G030_FLAG_C, carry);
  return r;
}

DP32G030_EXEC(lsls_imm) {
  c->r[in->rd] = dp32g030_shift(c, DP32G030_SHIFT_LSL, c->r[in->rn], in->imm);
}

DP32G030_EXEC(lsrs_imm) {
  c->r[in->rd] = dp32g030_shift(c, DP32G030_SHIFT_LSR, c->r[in->rn], in->imm);
}

DP32G030_EXEC(asrs_imm) {
  c->r[in->rd] = dp32g030_shift(c, DP32G030_SHIFT_ASR, c->r[in->rn], in->imm);
}

DP32G030_EXEC(lsls_reg) {
  c->r[in->rd] =
      dp32g030_shift(c, DP32G030_SHIFT_LSL, c->r[in->rn], c->r[in->rm] & 0xFF);
}

DP32G030_EXEC(lsrs_reg) {
  c->r[in->rd] =
      dp32g030_shift(c, DP32G030_SHIFT_LSR, c->r[in->rn], c->r[in->rm] & 0xFF);
}

DP32G030_EXEC(asrs_reg) {
  c->r[in->rd] =
      dp32g030_shift(c, DP32G030_SHIFT_ASR, c->r[in->rn], c->r[in->rm] & 0xFF);
}

DP32G030_EXEC(rors_reg) {
  c->r[in->rd] =
      dp32g030_shift(c, DP32G030_SHIFT_ROR, c->r[in->rn], c->r[in->rm] & 0xFF);
}

DP32G030_EXEC(adds_reg) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], c->r[in->rm], 0, true);
}

DP32G030_EXEC(subs_reg) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], ~c->r[in->rm], 1, true);
}

DP32G030_EXEC(adds_imm) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], in->imm, 0, true);
}

DP32G030_EXEC(subs_imm) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], ~in->imm, 1, true);
}

DP32G030_EXEC(movs_imm) { dp32g030_logic(c, in->rd, in->imm); }

DP32G030_EXEC(movs_reg) { dp32g030_logic(c, in->rd, c->r[in->rm]); }

DP32G030_EXEC(mov_reg) {
  dp32g030_reg_write(c, in->rd, dp32g030_reg(c, in->rm));
}

DP32G030_EXEC(cmp_imm) { dp32g030_add_c(c, c->r[in->rn], ~in->imm, 1, true); }

DP32G030_EXEC(cmp_reg) {
  dp32g030_add_c(c, dp32g030_reg(c, in->rn), ~dp32g030_reg(c, in->rm), 1, true);
}

DP32G030_EXEC(cmn) { dp32g030_add_c(c, c->r[in->rn], c->r[in->rm], 0, true); }

DP32G030_EXEC(ands) { dp32g030_logic(c, in->rd, c->r[in->rn] & c->r[in->rm]); }

DP32G030_EXEC(eors) { dp32g030_logic(c, in->rd, c->r[in->rn] ^ c->r[in->rm]); }

DP32G030_EXEC(orrs) { dp32g030_logic(c, in->rd, c->r[in->rn] | c->r[in->rm]); }

DP32G030_EXEC(bics) { dp32g030_logic(c, in->rd, c->r[in->rn] & ~c->r[in->rm]); }

DP32G030_EXEC(mvns) { dp32g030_logic(c, in->rd, ~c->r[in->rm]); }

DP32G030_EXEC(tst) { dp32g030_set_nz(c, c->r[in->rn] & c->r[in->rm]); }

DP32G030_EXEC(adcs) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], c->r[in->rm],
                                dp32g030_flag(c, DP32G030_FLAG_C), true);
}

DP32G030_EXEC(sbcs) {
  c->r[in->rd] = dp32g030_add_c(c, c->r[in->rn], ~c->r[in->rm],
                                dp32g030_flag(c, DP32G030_FLAG_C), true);
}

DP32G030_EXEC(rsbs) {
  c->r[in->rd] = dp32g030_add_c(c, ~c->r[in->rn], 0, 1, true);
}

DP32G030_EXEC(muls) { dp32g030_logic(c, in->rd, c->r[in->rn] * c->r[in->rm]); }

DP32G030_EXEC(add_reg) {
  dp32g030_reg_write(c, in->rd,
                     dp32g030_reg(c, in->rn) + dp32g030_reg(c, in->rm));
}

DP32G030_EXEC(add_sp_imm) { c->r[in->rd] = DP32G030_SP(c) + in->imm; }

DP32G030_EXEC(add_sp_sp_imm) { DP32G030_SP(c) += in->imm; }

DP32G030_EXEC(sub_sp_sp_imm) { DP32G030_SP(c) -= in->imm; }

DP32G030_EXEC(adr) { c->r[in->rd] = ((c->insn_pc + 4) & ~3u) + in->imm; }

const dp32g030_exec_entry_t dp32g030_exec_alu_entries[] = {
    {OP_LSLS_IMM, dp32g030_exec_lsls_imm},
    {OP_LSRS_IMM, dp32g030_exec_lsrs_imm},
    {OP_ASRS_IMM, dp32g030_exec_asrs_imm},
    {OP_LSLS_REG, dp32g030_exec_lsls_reg},
    {OP_LSRS_REG, dp32g030_exec_lsrs_reg},
    {OP_ASRS_REG, dp32g030_exec_asrs_reg},
    {OP_RORS_REG, dp32g030_exec_rors_reg},
    {OP_ADDS_REG, dp32g030_exec_adds_reg},
    {OP_SUBS_REG, dp32g030_exec_subs_reg},
    {OP_ADDS_IMM, dp32g030_exec_adds_imm},
    {OP_SUBS_IMM, dp32g030_exec_subs_imm},
    {OP_MOVS_IMM, dp32g030_exec_movs_imm},
    {OP_MOVS_REG, dp32g030_exec_movs_reg},
    {OP_MOV_REG, dp32g030_exec_mov_reg},
    {OP_CMP_IMM, dp32g030_exec_cmp_imm},
    {OP_CMP_REG, dp32g030_exec_cmp_reg},
    {OP_CMN, dp32g030_exec_cmn},
    {OP_ANDS, dp32g030_exec_ands},
    {OP_EORS, dp32g030_exec_eors},
    {OP_ORRS, dp32g030_exec_orrs},
    {OP_BICS, dp32g030_exec_bics},
    {OP_MVNS, dp32g030_exec_mvns},
    {OP_TST, dp32g030_exec_tst},
    {OP_ADCS, dp32g030_exec_adcs},
    {OP_SBCS, dp32g030_exec_sbcs},
    {OP_RSBS, dp32g030_exec_rsbs},
    {OP_MULS, dp32g030_exec_muls},
    {OP_ADD_REG, dp32g030_exec_add_reg},
    {OP_ADD_SP_IMM, dp32g030_exec_add_sp_imm},
    {OP_ADD_SP_SP_IMM, dp32g030_exec_add_sp_sp_imm},
    {OP_SUB_SP_SP_IMM, dp32g030_exec_sub_sp_sp_imm},
    {OP_ADR, dp32g030_exec_adr},
};
const size_t dp32g030_exec_alu_count =
    sizeof dp32g030_exec_alu_entries / sizeof *dp32g030_exec_alu_entries;
