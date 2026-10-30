#include "mmio.h"

#define REGION(n, b, clk, irq_n) \
  {#n, (b), DP32G030_MMIO_REGION_SIZE, (clk), (irq_n), NULL, NULL}

static const dp32g030_region_t dp32g030_regions[] = {
    REGION(SYSCON, 0x40000000u, -1, -1),
    REGION(PMU, 0x40000800u, -1, -1),
    REGION(DMA, 0x40001000u, -1, 3),
    REGION(CRC, 0x40003000u, 27, -1),
    REGION(GPIOA, 0x40060000u, 0, 29),
    REGION(GPIOB, 0x40060800u, 1, 30),
    REGION(GPIOC, 0x40061000u, 2, 31),
    REGION(TIMER_BASE0, 0x40064000u, 12, 5),
    REGION(TIMER_BASE1, 0x40064800u, 13, 6),
    REGION(TIMER_BASE2, 0x40065000u, 14, 21),
    REGION(TIMER_PLUS0, 0x40067000u, 15, 7),
    REGION(TIMER_PLUS1, 0x40067800u, 16, 8),
    REGION(RTC, 0x40069000u, 22, 2),
    REGION(IWDT, 0x4006A000u, 23, 1),
    REGION(WWDT, 0x4006A800u, 24, 0),
    REGION(UART0, 0x4006B000u, 6, 13),
    REGION(UART1, 0x4006B800u, 7, 14),
    REGION(UART2, 0x4006C000u, 8, 15),
    REGION(FLASH_CTRL, 0x4006F000u, -1, -1),
    REGION(PORTCON, 0x400B0000u, -1, -1),
    REGION(PWM_BASE0, 0x400B1000u, 17, 9),
    REGION(PWM_BASE1, 0x400B1800u, 18, 10),
    REGION(PWM_PLUS0, 0x400B4000u, 20, 11),
    REGION(PWM_PLUS1, 0x400B4800u, 21, 12),
    REGION(SPI0, 0x400B8000u, 10, 16),
    REGION(SPI1, 0x400B8800u, 11, 17),
    REGION(IIC0, 0x400B9000u, 4, 18),
    REGION(IIC1, 0x400B9800u, 5, 19),
    REGION(SARADC, 0x400BA000u, 25, 4),
    REGION(AES128, 0x400BD000u, 28, -1),
};

const dp32g030_region_t *dp32g030_mmio_find(uint32_t addr) {
  for (size_t i = 0; i < sizeof dp32g030_regions / sizeof *dp32g030_regions;
       i++) {
    if (addr - dp32g030_regions[i].base < dp32g030_regions[i].size)
      return &dp32g030_regions[i];
  }
  return NULL;
}
