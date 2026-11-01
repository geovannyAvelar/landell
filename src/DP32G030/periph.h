#pragma once

#include "DP32G030.h"
#include "mmio.h"

/* Plain read/write storage; used by blocks without a behavioural model. */
bool dp32g030_regfile_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                           uint32_t off, unsigned size, uint32_t *out);
bool dp32g030_regfile_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                            uint32_t off, unsigned size, uint32_t val);

bool dp32g030_gpio_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out);
bool dp32g030_gpio_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val);

bool dp32g030_uart_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out);
bool dp32g030_uart_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val);

void dp32g030_uart_tick(dp32g030_cpu_t *c);
uint64_t dp32g030_uart_next_event(const dp32g030_cpu_t *c);

/* Reset peripheral state; host hooks and externally driven pins persist. */
void dp32g030_periph_reset(dp32g030_cpu_t *c);

#define DP32G030_DECL_RW(name)                                                 \
  bool dp32g030_##name##_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,   \
                              uint32_t off, unsigned size, uint32_t *out);     \
  bool dp32g030_##name##_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,  \
                               uint32_t off, unsigned size, uint32_t val)
DP32G030_DECL_RW(spi);
DP32G030_DECL_RW(adc);
DP32G030_DECL_RW(crc);
