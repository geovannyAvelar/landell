#pragma once

#include "DP32G030.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP32G030_MMIO_REGION_SIZE 0x800u

struct dp32g030_region;

typedef bool (*dp32g030_mmio_read_fn)(dp32g030_cpu_t *c,
                                      const struct dp32g030_region *r,
                                      uint32_t off, unsigned size,
                                      uint32_t *out);
typedef bool (*dp32g030_mmio_write_fn)(dp32g030_cpu_t *c,
                                       const struct dp32g030_region *r,
                                       uint32_t off, unsigned size,
                                       uint32_t val);

typedef struct dp32g030_region {
  const char *name;
  uint32_t base;
  uint32_t size;
  int8_t clock_bit;
  int8_t irq;
  dp32g030_mmio_read_fn read;
  dp32g030_mmio_write_fn write;
} dp32g030_region_t;

const dp32g030_region_t *dp32g030_mmio_find(uint32_t addr);
/* Position of `r` in the peripheral table, or -1 for the SCS. */
int dp32g030_mmio_index(const dp32g030_region_t *r);
