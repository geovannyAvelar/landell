#pragma once

#include "DP32G030.h"
#include "mmio.h"

/* Describes one supported chip: memory map and the pieces of the core that
   differ between chips. Descriptors are static and live in soc.c / the chip's
   own file. */
struct dp32g030_soc {
  const char *name;
  uint32_t flash_base, flash_size;
  /* Second window onto the same flash (boot remap); size 0 = none. */
  uint32_t flash_alias_base, flash_alias_size;
  uint32_t sram_base, sram_size;
  uint32_t image_base; /* where a raw image is loaded; its vector table */
  bool has_vtor;
  const dp32g030_region_t *(*region_find)(uint32_t addr);
  /* Chip-specific power-on state, run at the end of dp32g030_cpu_reset. */
  void (*reset)(dp32g030_cpu_t *c);
};

extern const dp32g030_soc_t dp32g030_soc_dp32g030;
extern const dp32g030_soc_t dp32g030_soc_py32f071;
