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
DP32G030_DECL_RW(timer_base);
DP32G030_DECL_RW(timer_plus);
DP32G030_DECL_RW(pwm_base);
DP32G030_DECL_RW(pwm_plus);
DP32G030_DECL_RW(rtc);
DP32G030_DECL_RW(iwdt);
DP32G030_DECL_RW(wwdt);
DP32G030_DECL_RW(aes);
DP32G030_DECL_RW(iic);

/* Advance all TIMER_BASE/TIMER_PLUS/PWM_BASE/PWM_PLUS units by `cycles`
   core clocks (called from dp32g030_soc_tick). */
void dp32g030_timer_pwm_tick(dp32g030_cpu_t *c, uint64_t cycles);
uint64_t dp32g030_timer_pwm_next_event(const dp32g030_cpu_t *c);

/* Internal to timer_pwm.c/timer_plus.c/pwm_plus.c. */
void dp32g030_timer_plus_tick(dp32g030_cpu_t *c, uint64_t cycles);
uint64_t dp32g030_timer_plus_next_event(const dp32g030_cpu_t *c);
void dp32g030_pwm_plus_tick(dp32g030_cpu_t *c, uint64_t cycles);
uint64_t dp32g030_pwm_plus_next_event(const dp32g030_cpu_t *c);

/* RTC/IWDT/WWDT (rtc_wdt.c). */
void dp32g030_rtc_wdt_tick(dp32g030_cpu_t *c, uint64_t cycles);
uint64_t dp32g030_rtc_wdt_next_event(const dp32g030_cpu_t *c);

/* Shared prescaler helper (defined in timer_pwm.c): run a shared-divider
   up-counter block forward by `cycles` core clocks, firing `tick_one` once
   per elapsed (div+1)-cycle period; and compute the core clocks until
   `ticks` such firings from now. */
void dp32g030_prescaled_run(dp32g030_cpu_t *c, uint32_t div, uint32_t *presc,
                            uint64_t cycles, unsigned n,
                            void (*tick_one)(dp32g030_cpu_t *, unsigned));
uint64_t dp32g030_presc_event(uint32_t presc, uint32_t div, uint64_t ticks);
