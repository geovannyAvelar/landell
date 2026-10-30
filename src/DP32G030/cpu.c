#include "DP32G030.h"

#include "decode.h"
#include "exec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline size_t thumb_insn_size(const uint8_t *code, size_t i) {
  uint8_t hi = code[i + 1];
  return (hi >= 0xE8) ? 4 : 2;
}

static inline uint16_t fetch16(const uint8_t *code, size_t i) {
  return (uint16_t)(code[i] | (code[i + 1] << 8));
}

static inline uint32_t fetch32(const uint8_t *buf, size_t off) {
  return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8 |
         (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

dp32g030_cpu_t *dp32g030_cpu_new(const uint8_t *fw, size_t size) {
  if (!fw || size < 8 || size > DP32G030_FLASH_SIZE)
    return NULL;

  dp32g030_cpu_t *c = calloc(1, sizeof *c);
  if (!c) {
    return NULL;
  }

  memset(c->flash, 0xFF, DP32G030_FLASH_SIZE);
  memcpy(c->flash, fw, size);

  c->r[13] = fetch32(c->flash, 0);
  c->r[15] = fetch32(c->flash, 4) & ~1u;
  c->xpsr = 1u << 24;

  return c;
}

uint8_t dp32g030_cpu_run(dp32g030_cpu_t **cpu, size_t size) {
  dp32g030_cpu_t *c = *cpu;

  for (uint64_t step = 0; step < size; step++) {
    uint32_t pc = c->r[15];

    if ((pc & 1) || pc + 2 > DP32G030_FLASH_SIZE) {
      return EMU_FAULT;
    }

    size_t n = thumb_insn_size(c->flash, pc);
    if (pc + n > DP32G030_FLASH_SIZE) {
      return EMU_FAULT;
    }

    uint16_t hw1 = fetch16(c->flash, pc);
    uint16_t hw2 = (n == 4) ? fetch16(c->flash, pc + 2) : 0;

    dp32g030_insn_t in = dp32g030_insn_decode(hw1, hw2);
    if (in.op == OP_UNDEF) {
      return EMU_UNDEF;
    }

    c->insn_pc = pc;
    c->r[15] = pc + in.size;
    dp32g030_insn_exec(c, &in);

    if (c->halted) {
      return EMU_OK;
    }
  }

  return EMU_OK;
}

void dp32g030_cpu_free(dp32g030_cpu_t *c) { free(c); }
