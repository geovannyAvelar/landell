#pragma once

#include "DP32G030.h"

#include <stdint.h>

void dp32g030_hard_fault(dp32g030_cpu_t *c);
void dp32g030_exc_enter(dp32g030_cpu_t *c, unsigned exc);
void dp32g030_exc_return(dp32g030_cpu_t *c, uint32_t exc_return);
