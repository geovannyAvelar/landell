#include "DP32G030.h"

#include "bus.h"
#include "decode.h"
#include "exc.h"
#include "exec.h"
#include "periph.h"
#include "soc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t fetch32(const uint8_t *buf, size_t off) {
  return (uint32_t)buf[off] | (uint32_t)buf[off + 1] << 8 |
         (uint32_t)buf[off + 2] << 16 | (uint32_t)buf[off + 3] << 24;
}

void dp32g030_cpu_reset(dp32g030_cpu_t *c) {
  memset(c->r, 0, sizeof c->r);
  c->r[13] = fetch32(c->flash, 0);
  c->r[15] = fetch32(c->flash, 4) & ~1u;
  c->insn_pc = c->r[15];
  c->xpsr = 1u << 24;
  c->primask = 0;
  c->control = 0;
  c->halted = false;
  c->sleeping = false;
  c->locked_up = false;
  c->event = false;
  c->reset_req = false;
  c->handler = false;
  c->using_psp = false;
  c->msp = c->r[13];
  c->psp = 0;
  c->scs.irq_lines = 0;
  dp32g030_scs_reset(c);
  dp32g030_periph_reset(c);
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
  dp32g030_cpu_reset(c);

  return c;
}

/* Fetch one instruction. Returns false on a bus error. */
static bool dp32g030_fetch(dp32g030_cpu_t *c, uint32_t pc, uint16_t *hw1,
                           uint16_t *hw2, unsigned *n) {
  uint32_t v;

  if ((pc & 1) || !dp32g030_bus_read(c, pc, 2, &v))
    return false;
  *hw1 = (uint16_t)v;
  *hw2 = 0;
  *n = (*hw1 >> 11) >= 0x1Du ? 4 : 2;
  if (*n == 4) {
    if (!dp32g030_bus_read(c, pc + 2, 2, &v))
      return false;
    *hw2 = (uint16_t)v;
  }
  return true;
}

uint8_t dp32g030_cpu_run(dp32g030_cpu_t **cpu, size_t size) {
  dp32g030_cpu_t *c = *cpu;

  for (uint64_t step = 0; step < size; step++) {
    uint64_t start = c->cycles;

    if (c->reset_req)
      dp32g030_cpu_reset(c);

    if (c->scs.pending) {
      int exc = dp32g030_exc_pick(c);
      if (exc >= 0)
        dp32g030_exc_enter(c, (unsigned)exc);
      if (c->halted)
        return EMU_OK;
    }

    if (c->sleeping) {
      if (dp32g030_exc_wake(c)) {
        c->sleeping = false;
      } else {
        uint64_t n = dp32g030_soc_next_event(c);
        if (n == DP32G030_NO_EVENT)
          return EMU_OK; /* nothing can wake the core */
        c->cycles += n;
        dp32g030_soc_tick(c, n);
        continue;
      }
    }

    uint32_t pc = c->r[15];
    uint16_t hw1, hw2;
    unsigned n;

    if (!dp32g030_fetch(c, pc, &hw1, &hw2, &n)) {
      c->insn_pc = pc;
      dp32g030_hard_fault(c);
      if (c->halted)
        return EMU_FAULT;
      goto account;
    }

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

    c->cycles += dp32g030_insn_cycles(&in, c->r[15] != pc + in.size);
  account:
    dp32g030_soc_tick(c, c->cycles - start);
  }

  return EMU_OK;
}

void dp32g030_cpu_free(dp32g030_cpu_t *c) { free(c); }
