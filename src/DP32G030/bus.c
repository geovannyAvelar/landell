#include "bus.h"

#include <stddef.h>

#include "mmio.h"
#include "socdesc.h"

static bool dp32g030_bus_valid(uint32_t addr, unsigned size) {
  if (size != 1 && size != 2 && size != 4)
    return false;
  return (addr & (size - 1)) == 0;
}

static uint8_t *dp32g030_bus_flash(dp32g030_cpu_t *c, uint32_t addr,
                                   unsigned size) {
  const dp32g030_soc_t *s = c->soc;
  uint32_t off = addr - s->flash_base;

  if (off <= s->flash_size - size)
    return &c->flash[off];
  off = addr - s->flash_alias_base;
  if (s->flash_alias_size && off <= s->flash_alias_size - size)
    return &c->flash[off];
  return NULL;
}

static uint8_t *dp32g030_bus_sram(dp32g030_cpu_t *c, uint32_t addr,
                                  unsigned size) {
  uint32_t off = addr - c->soc->sram_base;

  if (off > c->soc->sram_size - size)
    return NULL;
  return &c->sram[off];
}

static bool bus_read_inner(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                           uint32_t *out) {
  if (!dp32g030_bus_valid(addr, size))
    return false;

  const uint8_t *p = dp32g030_bus_flash(c, addr, size);
  if (!p)
    p = dp32g030_bus_sram(c, addr, size);

  if (!p) {
    const dp32g030_region_t *r = c->soc->region_find(addr);
    if (!r || !r->read)
      return false;
    return r->read(c, r, addr - r->base, size, out);
  }

  uint32_t v = 0;
  for (unsigned i = 0; i < size; i++)
    v |= (uint32_t)p[i] << (8 * i);
  *out = v;
  return true;
}

static bool bus_write_inner(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                            uint32_t val) {
  if (!dp32g030_bus_valid(addr, size))
    return false;

  uint8_t *p = dp32g030_bus_sram(c, addr, size);
  if (!p) {
    const dp32g030_region_t *r = c->soc->region_find(addr);
    if (!r || !r->write)
      return false;
    return r->write(c, r, addr - r->base, size, val);
  }

  for (unsigned i = 0; i < size; i++)
    p[i] = (uint8_t)(val >> (8 * i));
  return true;
}

bool dp32g030_bus_read(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                       uint32_t *out) {
  if (bus_read_inner(c, addr, size, out))
    return true;
  c->bus_fault_addr = addr;
  c->bus_fault_write = false;
  return false;
}

bool dp32g030_bus_write(dp32g030_cpu_t *c, uint32_t addr, unsigned size,
                        uint32_t val) {
  if (bus_write_inner(c, addr, size, val))
    return true;
  c->bus_fault_addr = addr;
  c->bus_fault_write = true;
  return false;
}
