#pragma once

#include "DP32G030.h"

#include <stdbool.h>
#include <stdint.h>

bool dp32g030_bus_read(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                       uint32_t *out);
bool dp32g030_bus_write(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                        uint32_t val);
