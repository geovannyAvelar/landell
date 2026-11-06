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
  /* Cheap check that an image belongs to this chip (NULL = anything goes). */
  bool (*accepts_image)(const uint8_t *fw, size_t size);
  /* What image this chip loads, for error messages. */
  const char *image_hint;
  /* Cycle-driven peripherals beyond the DP32G030 ones (NULL = none): advance by
     `cycles` core clocks / core clocks until the next event that can raise an
     interrupt or a DMA request. */
  void (*tick)(dp32g030_cpu_t *c, uint64_t cycles);
  uint64_t (*next_event)(const dp32g030_cpu_t *c);
  /* Board presets, see dp32g030_soc_defaults. */
  void (*defaults)(dp32g030_cpu_t *c);
  /* Chip-specific power-on state, run at the end of dp32g030_cpu_reset. */
  void (*reset)(dp32g030_cpu_t *c);
  /* The LCD controller sits on SPI number `spi` (as the hooks number them);
     GPIO port `a0_port` (gpio_pins port index) pin `a0_pin` tells a command
     byte (low) from pixel data (high). */
  struct {
    unsigned spi, a0_port, a0_pin;
  } lcd;
  /* GPIO behind dp32g030_gpio_set_input / dp32g030_gpio_pins. */
  void (*gpio_set_input)(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                         uint32_t value);
  uint32_t (*gpio_pins)(const dp32g030_cpu_t *c, unsigned port);
  /* Direction of a GPIO port: 1 = output. */
  uint32_t (*gpio_dir)(const dp32g030_cpu_t *c, unsigned port);
  /* The board's backlight pin (GPIO port index and pin); `present` false = not
     modeled on this chip. */
  struct {
    bool present;
    unsigned port, pin;
  } backlight;
  /* The board's external SPI NOR flash on SPI number `spi` (as the hooks
     number them); GPIO port `cs_port` pin `cs_pin` is its active-low chip
     select. `present` false = no flash model on this chip. */
  struct {
    bool present;
    unsigned spi, cs_port, cs_pin;
  } extflash;
  /* The BK4819 answers on the data line by driving it as an input level. */
  void (*bk4819_drive_sda)(dp32g030_cpu_t *c, bool level);
  /* Analog level on ADC channel `ch` (dp32g030_adc_set_input). */
  void (*adc_set_input)(dp32g030_cpu_t *c, unsigned ch, uint32_t value);
};

extern const dp32g030_soc_t dp32g030_soc_dp32g030;
extern const dp32g030_soc_t dp32g030_soc_py32f071;
