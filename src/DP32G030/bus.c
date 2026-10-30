#include "bus.h"

#include <stddef.h>

static bool dp32g030_bus_valid(uint32_t addr, unsigned size) {
  if (size != 1 && size != 2 && size != 4)
    return false;
  return (addr & (size - 1)) == 0;
}

static uint8_t *dp32g030_bus_flash(dp32g030_cpu_t *c, uint32_t addr,
                                   unsigned size) {
  uint32_t off = addr - DP32G030_FLASH_BASE;

  if (off > DP32G030_FLASH_SIZE - size)
    return NULL;
  return &c->flash[off];
}

static uint8_t *dp32g030_bus_sram(dp32g030_cpu_t *c, uint32_t addr,
                                  unsigned size) {
  uint32_t off = addr - DP32G030_SRAM_BASE;

  if (off > DP32G030_SRAM_SIZE - size)
    return NULL;
  return &c->sram[off];
}

bool dp32g030_bus_read(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                       uint32_t *out) {
  if (!dp32g030_bus_valid(addr, size))
    return false;

  const uint8_t *p = dp32g030_bus_flash(c, addr, size);
  if (!p)
    p = dp32g030_bus_sram(c, addr, size);
  if (!p)
    return false;

  uint32_t v = 0;
  for (unsigned i = 0; i < size; i++)
    v |= (uint32_t)p[i] << (8 * i);
  *out = v;
  return true;
}

bool dp32g030_bus_write(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                        uint32_t val) {
  if (!dp32g030_bus_valid(addr, size))
    return false;

  uint8_t *p = dp32g030_bus_sram(c, addr, size);
  if (!p)
    return false;

  for (unsigned i = 0; i < size; i++)
    p[i] = (uint8_t)(val >> (8 * i));
  return true;
}
