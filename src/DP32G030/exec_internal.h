#pragma once

#include "DP32G030.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP32G030_FLAG_N (1u << 31)
#define DP32G030_FLAG_Z (1u << 30)
#define DP32G030_FLAG_C (1u << 29)
#define DP32G030_FLAG_V (1u << 28)

#define DP32G030_SP(c) ((c)->r[13])
#define DP32G030_LR(c) ((c)->r[14])
#define DP32G030_PC(c) ((c)->r[15])

#define DP32G030_EXEC(name)                                                    \
  static void dp32g030_exec_##name(dp32g030_cpu_t *c, const dp32g030_insn_t *in)

typedef void (*dp32g030_exec_fn)(dp32g030_cpu_t *, const dp32g030_insn_t *);

typedef struct {
  dp32g030_op_t op;
  dp32g030_exec_fn fn;
} dp32g030_exec_entry_t;

extern const dp32g030_exec_entry_t dp32g030_exec_alu_entries[];
extern const size_t dp32g030_exec_alu_count;
extern const dp32g030_exec_entry_t dp32g030_exec_mem_entries[];
extern const size_t dp32g030_exec_mem_count;
extern const dp32g030_exec_entry_t dp32g030_exec_branch_entries[];
extern const size_t dp32g030_exec_branch_count;
extern const dp32g030_exec_entry_t dp32g030_exec_misc_entries[];
extern const size_t dp32g030_exec_misc_count;

void dp32g030_bx_write(dp32g030_cpu_t *c, uint32_t target);

static inline bool dp32g030_flag(const dp32g030_cpu_t *c, uint32_t f) {
  return (c->xpsr & f) != 0;
}

static inline void dp32g030_set_flag(dp32g030_cpu_t *c, uint32_t f, bool v) {
  if (v)
    c->xpsr |= f;
  else
    c->xpsr &= ~f;
}

static inline void dp32g030_set_nz(dp32g030_cpu_t *c, uint32_t v) {
  dp32g030_set_flag(c, DP32G030_FLAG_N, (v >> 31) != 0);
  dp32g030_set_flag(c, DP32G030_FLAG_Z, v == 0);
}

static inline uint32_t dp32g030_reg(const dp32g030_cpu_t *c, unsigned n) {
  return n == 15 ? c->insn_pc + 4 : c->r[n];
}

static inline void dp32g030_reg_write(dp32g030_cpu_t *c, unsigned n, uint32_t v) {
  if (n == 15)
    DP32G030_PC(c) = v & ~1u;
  else if (n == 13)
    DP32G030_SP(c) = v & ~3u;
  else
    c->r[n] = v;
}
