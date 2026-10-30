#include "exc.h"

void dp32g030_hard_fault(dp32g030_cpu_t *c) { c->halted = true; }

void dp32g030_exc_enter(dp32g030_cpu_t *c, unsigned exc) {
  (void)exc;
  c->halted = true;
}

void dp32g030_exc_return(dp32g030_cpu_t *c, uint32_t exc_return) {
  (void)exc_return;
  c->halted = true;
}
