#include "mmio.h"

#include "periph.h"

#define REGION_RW(n, b, clk, irq_n, rd, wr) \
  {#n, (b), DP32G030_MMIO_REGION_SIZE, (clk), (irq_n), (rd), (wr)}
#define REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_regfile_read, dp32g030_regfile_write)
#define GPIO_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_gpio_read, dp32g030_gpio_write)
#define SPI_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_spi_read, dp32g030_spi_write)
#define ADC_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_adc_read, dp32g030_adc_write)
#define CRC_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_crc_read, dp32g030_crc_write)
#define UART_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_uart_read, dp32g030_uart_write)
#define TIMER_BASE_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_timer_base_read, dp32g030_timer_base_write)
#define TIMER_PLUS_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_timer_plus_read, dp32g030_timer_plus_write)
#define PWM_BASE_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_pwm_base_read, dp32g030_pwm_base_write)
#define PWM_PLUS_REGION(n, b, clk, irq_n) \
  REGION_RW(n, b, clk, irq_n, dp32g030_pwm_plus_read, dp32g030_pwm_plus_write)

static const dp32g030_region_t dp32g030_regions[] = {
    REGION(SYSCON, 0x40000000u, -1, -1),
    REGION(PMU, 0x40000800u, -1, -1),
    REGION(DMA, 0x40001000u, -1, 3),
    CRC_REGION(CRC, 0x40003000u, 27, -1),
    GPIO_REGION(GPIOA, 0x40060000u, 0, 29),
    GPIO_REGION(GPIOB, 0x40060800u, 1, 30),
    GPIO_REGION(GPIOC, 0x40061000u, 2, 31),
    TIMER_BASE_REGION(TIMER_BASE0, 0x40064000u, 12, 5),
    TIMER_BASE_REGION(TIMER_BASE1, 0x40064800u, 13, 6),
    TIMER_BASE_REGION(TIMER_BASE2, 0x40065000u, 14, 21),
    TIMER_PLUS_REGION(TIMER_PLUS0, 0x40067000u, 15, 7),
    TIMER_PLUS_REGION(TIMER_PLUS1, 0x40067800u, 16, 8),
    REGION(RTC, 0x40069000u, 22, 2),
    REGION(IWDT, 0x4006A000u, 23, 1),
    REGION(WWDT, 0x4006A800u, 24, 0),
    UART_REGION(UART0, 0x4006B000u, 6, 13),
    UART_REGION(UART1, 0x4006B800u, 7, 14),
    UART_REGION(UART2, 0x4006C000u, 8, 15),
    REGION(FLASH_CTRL, 0x4006F000u, -1, -1),
    REGION(PORTCON, 0x400B0000u, -1, -1),
    PWM_BASE_REGION(PWM_BASE0, 0x400B1000u, 17, 9),
    PWM_BASE_REGION(PWM_BASE1, 0x400B1800u, 18, 10),
    PWM_PLUS_REGION(PWM_PLUS0, 0x400B4000u, 20, 11),
    PWM_PLUS_REGION(PWM_PLUS1, 0x400B4800u, 21, 12),
    SPI_REGION(SPI0, 0x400B8000u, 10, 16),
    SPI_REGION(SPI1, 0x400B8800u, 11, 17),
    REGION(IIC0, 0x400B9000u, 4, 18),
    REGION(IIC1, 0x400B9800u, 5, 19),
    ADC_REGION(SARADC, 0x400BA000u, 25, 4),
    REGION(AES128, 0x400BD000u, 28, -1),
};

extern const dp32g030_region_t dp32g030_scs_region;

_Static_assert(sizeof dp32g030_regions / sizeof *dp32g030_regions ==
                   DP32G030_NREGIONS,
               "DP32G030_NREGIONS must match the region table");

int dp32g030_mmio_index(const dp32g030_region_t *r) {
  if (r < dp32g030_regions ||
      r >= dp32g030_regions + sizeof dp32g030_regions / sizeof *dp32g030_regions)
    return -1;
  return (int)(r - dp32g030_regions);
}

const dp32g030_region_t *dp32g030_mmio_find(uint32_t addr) {
  if (addr - dp32g030_scs_region.base < dp32g030_scs_region.size)
    return &dp32g030_scs_region;
  for (size_t i = 0; i < sizeof dp32g030_regions / sizeof *dp32g030_regions;
       i++) {
    if (addr - dp32g030_regions[i].base < dp32g030_regions[i].size)
      return &dp32g030_regions[i];
  }
  return NULL;
}
