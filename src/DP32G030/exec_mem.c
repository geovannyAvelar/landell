#include "exec_internal.h"

#include "bus.h"
#include "exc.h"

#include <stdbool.h>
#include <stdint.h>

static void dp32g030_load(dp32g030_cpu_t *c, unsigned rd, uint32_t addr, unsigned size,
                          bool sign) {
  uint32_t v;

  if (!dp32g030_bus_read(c, addr, size, &v)) {
    dp32g030_hard_fault(c);
    return;
  }
  if (sign)
    v = (size == 1) ? (uint32_t)(int32_t)(int8_t)v
                    : (uint32_t)(int32_t)(int16_t)v;
  c->r[rd] = v;
}

static void dp32g030_store(dp32g030_cpu_t *c, unsigned rt, uint32_t addr,
                           unsigned size) {
  if (!dp32g030_bus_write(c, addr, size, c->r[rt]))
    dp32g030_hard_fault(c);
}

DP32G030_EXEC(ldr_imm) {
  dp32g030_load(c, in->rd, c->r[in->rn] + in->imm, 4, false);
}

DP32G030_EXEC(str_imm) { dp32g030_store(c, in->rd, c->r[in->rn] + in->imm, 4); }

DP32G030_EXEC(ldrb_imm) {
  dp32g030_load(c, in->rd, c->r[in->rn] + in->imm, 1, false);
}

DP32G030_EXEC(strb_imm) {
  dp32g030_store(c, in->rd, c->r[in->rn] + in->imm, 1);
}

DP32G030_EXEC(ldrh_imm) {
  dp32g030_load(c, in->rd, c->r[in->rn] + in->imm, 2, false);
}

DP32G030_EXEC(strh_imm) {
  dp32g030_store(c, in->rd, c->r[in->rn] + in->imm, 2);
}

DP32G030_EXEC(ldr_sp) {
  dp32g030_load(c, in->rd, DP32G030_SP(c) + in->imm, 4, false);
}

DP32G030_EXEC(str_sp) {
  dp32g030_store(c, in->rd, DP32G030_SP(c) + in->imm, 4);
}

DP32G030_EXEC(ldr_lit) {
  dp32g030_load(c, in->rd, ((c->insn_pc + 4) & ~3u) + in->imm, 4, false);
}

DP32G030_EXEC(ldr_reg) {
  dp32g030_load(c, in->rd, c->r[in->rn] + c->r[in->rm], 4, false);
}

DP32G030_EXEC(str_reg) {
  dp32g030_store(c, in->rd, c->r[in->rn] + c->r[in->rm], 4);
}

DP32G030_EXEC(ldrb_reg) {
  dp32g030_load(c, in->rd, c->r[in->rn] + c->r[in->rm], 1, false);
}

DP32G030_EXEC(strb_reg) {
  dp32g030_store(c, in->rd, c->r[in->rn] + c->r[in->rm], 1);
}

DP32G030_EXEC(ldrh_reg) {
  dp32g030_load(c, in->rd, c->r[in->rn] + c->r[in->rm], 2, false);
}

DP32G030_EXEC(strh_reg) {
  dp32g030_store(c, in->rd, c->r[in->rn] + c->r[in->rm], 2);
}

DP32G030_EXEC(ldrsb_reg) {
  dp32g030_load(c, in->rd, c->r[in->rn] + c->r[in->rm], 1, true);
}

DP32G030_EXEC(ldrsh_reg) {
  dp32g030_load(c, in->rd, c->r[in->rn] + c->r[in->rm], 2, true);
}

DP32G030_EXEC(ldm) {
  uint32_t addr = c->r[in->rn];

  for (unsigned i = 0; i < 8; i++) {
    if (!(in->reglist & (1u << i)))
      continue;
    uint32_t v;
    if (!dp32g030_bus_read(c, addr, 4, &v)) {
      dp32g030_hard_fault(c);
      return;
    }
    c->r[i] = v;
    addr += 4;
  }
  if (!(in->reglist & (1u << in->rn)))
    c->r[in->rn] = addr;
}

DP32G030_EXEC(stm) {
  uint32_t addr = c->r[in->rn];

  for (unsigned i = 0; i < 8; i++) {
    if (!(in->reglist & (1u << i)))
      continue;
    if (!dp32g030_bus_write(c, addr, 4, c->r[i])) {
      dp32g030_hard_fault(c);
      return;
    }
    addr += 4;
  }
  c->r[in->rn] = addr;
}

DP32G030_EXEC(push) {
  uint32_t addr =
      DP32G030_SP(c) - 4u * (uint32_t)__builtin_popcount(in->reglist);
  uint32_t sp = addr;

  for (unsigned i = 0; i < 15; i++) {
    if (!(in->reglist & (1u << i)))
      continue;
    if (!dp32g030_bus_write(c, addr, 4, c->r[i])) {
      dp32g030_hard_fault(c);
      return;
    }
    addr += 4;
  }
  DP32G030_SP(c) = sp;
}

DP32G030_EXEC(pop) {
  uint32_t addr = DP32G030_SP(c);
  uint32_t v;

  for (unsigned i = 0; i < 8; i++) {
    if (!(in->reglist & (1u << i)))
      continue;
    if (!dp32g030_bus_read(c, addr, 4, &v)) {
      dp32g030_hard_fault(c);
      return;
    }
    c->r[i] = v;
    addr += 4;
  }

  if (in->reglist & (1u << 15)) {
    if (!dp32g030_bus_read(c, addr, 4, &v)) {
      dp32g030_hard_fault(c);
      return;
    }
    addr += 4;
    DP32G030_SP(c) = addr;
    dp32g030_bx_write(c, v);
    return;
  }
  DP32G030_SP(c) = addr;
}

const dp32g030_exec_entry_t dp32g030_exec_mem_entries[] = {
    {OP_LDR_IMM, dp32g030_exec_ldr_imm},
    {OP_STR_IMM, dp32g030_exec_str_imm},
    {OP_LDRB_IMM, dp32g030_exec_ldrb_imm},
    {OP_STRB_IMM, dp32g030_exec_strb_imm},
    {OP_LDRH_IMM, dp32g030_exec_ldrh_imm},
    {OP_STRH_IMM, dp32g030_exec_strh_imm},
    {OP_LDR_SP, dp32g030_exec_ldr_sp},
    {OP_STR_SP, dp32g030_exec_str_sp},
    {OP_LDR_LIT, dp32g030_exec_ldr_lit},
    {OP_LDR_REG, dp32g030_exec_ldr_reg},
    {OP_STR_REG, dp32g030_exec_str_reg},
    {OP_LDRB_REG, dp32g030_exec_ldrb_reg},
    {OP_STRB_REG, dp32g030_exec_strb_reg},
    {OP_LDRH_REG, dp32g030_exec_ldrh_reg},
    {OP_STRH_REG, dp32g030_exec_strh_reg},
    {OP_LDRSB_REG, dp32g030_exec_ldrsb_reg},
    {OP_LDRSH_REG, dp32g030_exec_ldrsh_reg},
    {OP_LDM, dp32g030_exec_ldm},
    {OP_STM, dp32g030_exec_stm},
    {OP_PUSH, dp32g030_exec_push},
    {OP_POP, dp32g030_exec_pop},
};
const size_t dp32g030_exec_mem_count =
    sizeof dp32g030_exec_mem_entries / sizeof *dp32g030_exec_mem_entries;
