#include "socdesc.h"

#include <string.h>

const dp32g030_soc_t dp32g030_soc_dp32g030 = {
    .name = "dp32g030",
    .flash_base = DP32G030_FLASH_BASE,
    .flash_size = DP32G030_FLASH_SIZE,
    .sram_base = DP32G030_SRAM_BASE,
    .sram_size = DP32G030_SRAM_SIZE,
    .image_base = DP32G030_FLASH_BASE,
    .has_vtor = false,
    .region_find = dp32g030_mmio_find,
};

static const dp32g030_soc_t *const dp32g030_socs[] = {
    &dp32g030_soc_dp32g030,
    &dp32g030_soc_py32f071,
};

const dp32g030_soc_t *dp32g030_soc_by_name(const char *name) {
  for (size_t i = 0; i < sizeof dp32g030_socs / sizeof *dp32g030_socs; i++)
    if (!strcmp(dp32g030_socs[i]->name, name))
      return dp32g030_socs[i];
  return NULL;
}
