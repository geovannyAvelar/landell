#include "socdesc.h"

#include "periph.h"

#include <string.h>

/* UV-K5 (V1/V2): the battery is on SARADC channel 9; without a level the
   firmware shows no UI. */
static void dp32g030_defaults_dp32(dp32g030_cpu_t *c) {
  dp32g030_adc_set_input_dp32(c, 9, 2000);
}

const dp32g030_soc_t dp32g030_soc_dp32g030 = {
    .name = "dp32g030",
    .flash_base = DP32G030_FLASH_BASE,
    .flash_size = DP32G030_FLASH_SIZE,
    .sram_base = DP32G030_SRAM_BASE,
    .sram_size = DP32G030_SRAM_SIZE,
    .image_base = DP32G030_FLASH_BASE,
    .has_vtor = false,
    .region_find = dp32g030_mmio_find,
    .lcd = {.spi = 0, .a0_port = 1, .a0_pin = 9},
    .gpio_set_input = dp32g030_gpio_set_input_dp32,
    .gpio_pins = dp32g030_gpio_pins_dp32,
    .adc_set_input = dp32g030_adc_set_input_dp32,
    .bk4819_drive_sda = dp32g030_bk4819_drive_sda_dp32,
    .defaults = dp32g030_defaults_dp32,
};

static const dp32g030_soc_t *const dp32g030_socs[] = {
    &dp32g030_soc_dp32g030,
    &dp32g030_soc_py32f071,
};

const char *dp32g030_soc_name(const dp32g030_soc_t *soc) { return soc->name; }

const dp32g030_soc_t *dp32g030_soc_detect(const uint8_t *fw, size_t size) {
  if (fw && size >= 8 && py32_image_ok(fw, size))
    return &dp32g030_soc_py32f071;
  return &dp32g030_soc_dp32g030;
}

void dp32g030_soc_defaults(dp32g030_cpu_t *c) {
  if (c->soc->defaults)
    c->soc->defaults(c);
}

const dp32g030_soc_t *dp32g030_soc_by_name(const char *name) {
  for (size_t i = 0; i < sizeof dp32g030_socs / sizeof *dp32g030_socs; i++)
    if (!strcmp(dp32g030_socs[i]->name, name))
      return dp32g030_socs[i];
  return NULL;
}

void dp32g030_gpio_set_input(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                             uint32_t value) {
  c->soc->gpio_set_input(c, port, mask, value);
}

uint32_t dp32g030_gpio_pins(const dp32g030_cpu_t *c, unsigned port) {
  return c->soc->gpio_pins(c, port);
}

void dp32g030_adc_set_input(dp32g030_cpu_t *c, unsigned ch, uint32_t value) {
  c->soc->adc_set_input(c, ch, value);
}
