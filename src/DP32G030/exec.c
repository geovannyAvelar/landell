#include "exec.h"

#include "exc.h"
#include "exec_internal.h"

static dp32g030_exec_fn dp32g030_exec_table[OP_COUNT];
static bool dp32g030_exec_ready;

static void dp32g030_exec_register(const dp32g030_exec_entry_t *entries,
                                   size_t count) {
  for (size_t i = 0; i < count; i++)
    dp32g030_exec_table[entries[i].op] = entries[i].fn;
}

static void dp32g030_exec_init(void) {
  dp32g030_exec_register(dp32g030_exec_alu_entries, dp32g030_exec_alu_count);
  dp32g030_exec_register(dp32g030_exec_mem_entries, dp32g030_exec_mem_count);
  dp32g030_exec_register(dp32g030_exec_branch_entries,
                         dp32g030_exec_branch_count);
  dp32g030_exec_register(dp32g030_exec_misc_entries,
                         dp32g030_exec_misc_count);
  dp32g030_exec_ready = true;
}

void dp32g030_insn_exec(dp32g030_cpu_t *c, const dp32g030_insn_t *in) {
  if (!dp32g030_exec_ready)
    dp32g030_exec_init();

  dp32g030_exec_fn fn = NULL;
  if ((size_t)in->op < OP_COUNT)
    fn = dp32g030_exec_table[in->op];
  if (!fn) {
    dp32g030_hard_fault(c);
    return;
  }
  fn(c, in);
}
