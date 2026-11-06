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

/* BK4819 (bk4819.c): called from dp32g030_gpio_write for GPIOC, with the
   port's effective pin levels before/after this write and its current
   direction register. */
void dp32g030_bk4819_gpio_edge(dp32g030_cpu_t *c, uint32_t before,
                               uint32_t after, uint32_t dir);

/* LCD (lcd.c): called from dp32g030_spi_write for SPI0 whenever a byte
   is actually clocked out (SPE and MSTR set). `data` is GPIOB pin 9
   (A0)'s current level: false for a command byte, true for pixel data. */
void dp32g030_lcd_spi_byte(dp32g030_cpu_t *c, uint8_t byte, bool data);

/* Shared prescaler helper (defined in timer_pwm.c): run a shared-divider
   up-counter block forward by `cycles` core clocks, firing `tick_one` once
   per elapsed (div+1)-cycle period; and compute the core clocks until
   `ticks` such firings from now. */
void dp32g030_prescaled_run(dp32g030_cpu_t *c, uint32_t div, uint32_t *presc,
                            uint64_t cycles, unsigned n,
                            void (*tick_one)(dp32g030_cpu_t *, unsigned));
uint64_t dp32g030_presc_event(uint32_t presc, uint32_t div, uint64_t ticks);

/* DP32G030 GPIO implementations behind the soc-aware public API. */
void dp32g030_gpio_set_input_dp32(dp32g030_cpu_t *c, unsigned port,
                                  uint32_t mask, uint32_t value);
uint32_t dp32g030_gpio_pins_dp32(const dp32g030_cpu_t *c, unsigned port);

/* PY32F071 GPIO (py32_gpio.c). Port index: 0=A, 1=B, 2=C, 3=F. */
void py32_gpio_reset(dp32g030_cpu_t *c);
void py32_gpio_set_input(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                         uint32_t value);
uint32_t py32_gpio_pins(const dp32g030_cpu_t *c, unsigned port);
bool py32_gpio_read(dp32g030_cpu_t *c, unsigned port, uint32_t off,
                    unsigned size, uint32_t *out);
bool py32_gpio_write(dp32g030_cpu_t *c, unsigned port, uint32_t off,
                     unsigned size, uint32_t val);

/* PY32F071 SPI (py32_spi.c). `n` is the SPI number: 1 or 2. */
bool py32_spi_read(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                   uint32_t *out);
bool py32_spi_write(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                    uint32_t val);
void py32_spi_reset(dp32g030_cpu_t *c);

void dp32g030_adc_set_input_dp32(dp32g030_cpu_t *c, unsigned ch,
                                 uint32_t value);

/* PY32F071 ADC (py32_adc.c), STM32F1-style. */
bool py32_adc_read(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                   uint32_t *out);
bool py32_adc_write(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                    uint32_t val);
void py32_adc_reset(dp32g030_cpu_t *c);
void py32_adc_set_input(dp32g030_cpu_t *c, unsigned ch, uint32_t value);

/* PY32F071 DMA (py32_dma.c), STM32-style: ISR, IFCR, 7 channels. */
bool py32_dma_read(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                   uint32_t *out);
bool py32_dma_write(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                    uint32_t val);
void py32_dma_reset(dp32g030_cpu_t *c);

/* BK4819 serial link decoder (bk4819.c), shared by every SoC. */
void dp32g030_bk4819_pins(dp32g030_cpu_t *c, bool scn, bool scl, bool sda,
                          bool scn_scl_out, bool sda_in);
void dp32g030_bk4819_drive_sda_dp32(dp32g030_cpu_t *c, bool level);
void py32_bk4819_drive_sda(dp32g030_cpu_t *c, bool level);

/* External SPI flash model (extflash.c). */
bool dp32g030_extflash_init(dp32g030_cpu_t *c);
void dp32g030_extflash_reset_bus(dp32g030_cpu_t *c);
/* Call after any write that can change the chip-select pin. */
void dp32g030_extflash_cs(dp32g030_cpu_t *c);
uint8_t dp32g030_extflash_xfer(dp32g030_cpu_t *c, uint8_t tx);

/* PY32F071 image checks (py32f071.c): detection looks only at the reset
   vector's flash window; loading also requires it to be in the app area. */
bool py32_looks_like_py32(const uint8_t *fw, size_t size);
bool py32_image_ok(const uint8_t *fw, size_t size);
