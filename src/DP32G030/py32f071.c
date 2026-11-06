#include "socdesc.h"

#include "periph.h"

/* PY32F071 (UV-K5 V3 / UV-K1). Bases and IRQ numbers come from the CMSIS
   device header of the pinned firmware (py32f071xB.h). Every peripheral is a
   plain register file until running the real firmware shows it needs
   behavior. */

extern const dp32g030_region_t dp32g030_scs_region;

static int py32_region_index(const dp32g030_region_t *r);

static bool py32_regfile_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                              uint32_t off, unsigned size, uint32_t *out) {
  int idx = py32_region_index(r);

  if (idx < 0 || off >= PY32_REGION_SIZE)
    return false;
  uint32_t w = c->py32.regfile[idx][off / 4];
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (w >> shift) & mask;
  return true;
}

static bool py32_regfile_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                               uint32_t off, unsigned size, uint32_t val) {
  int idx = py32_region_index(r);

  if (idx < 0 || off >= PY32_REGION_SIZE)
    return false;
  uint32_t *w = &c->py32.regfile[idx][off / 4];
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *w = (*w & ~(mask << shift)) | ((val & mask) << shift);
  return true;
}

static bool py32_gpio_region_read(dp32g030_cpu_t *c,
                                  const dp32g030_region_t *r, uint32_t off,
                                  unsigned size, uint32_t *out) {
  unsigned port = r->base == 0x50001400u ? 3u : (r->base - 0x50000000u) / 0x400u;

  if (off < 0x2C)
    return py32_gpio_read(c, port, off, size, out);
  return py32_regfile_read(c, r, off, size, out);
}

static bool py32_gpio_region_write(dp32g030_cpu_t *c,
                                   const dp32g030_region_t *r, uint32_t off,
                                   unsigned size, uint32_t val) {
  unsigned port = r->base == 0x50001400u ? 3u : (r->base - 0x50000000u) / 0x400u;

  if (off < 0x2C)
    return py32_gpio_write(c, port, off, size, val);
  return py32_regfile_write(c, r, off, size, val);
}

static bool py32_spi_region_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                                 uint32_t off, unsigned size, uint32_t *out) {
  unsigned n = r->base == 0x40013000u ? 1u : 2u;

  if (off < 0x10)
    return py32_spi_read(c, n, off, size, out);
  return py32_regfile_read(c, r, off, size, out);
}

static bool py32_spi_region_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                                  uint32_t off, unsigned size, uint32_t val) {
  unsigned n = r->base == 0x40013000u ? 1u : 2u;

  if (off < 0x10)
    return py32_spi_write(c, n, off, size, val);
  return py32_regfile_write(c, r, off, size, val);
}

static bool py32_adc_region_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                                 uint32_t off, unsigned size, uint32_t *out) {
  if (py32_adc_read(c, off, size, out))
    return true;
  return py32_regfile_read(c, r, off, size, out);
}

static bool py32_adc_region_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                                  uint32_t off, unsigned size, uint32_t val) {
  if (py32_adc_write(c, off, size, val))
    return true;
  return py32_regfile_write(c, r, off, size, val);
}

#define PY32_REGION(n, b, irq_n) \
  {#n, (b), PY32_REGION_SIZE, -1, (irq_n), py32_regfile_read, py32_regfile_write}
#define PY32_SPI_REGION(n, b, irq_n) \
  {#n, (b), PY32_REGION_SIZE, -1, (irq_n), py32_spi_region_read, py32_spi_region_write}
#define PY32_ADC_REGION(n, b, irq_n) \
  {#n, (b), PY32_REGION_SIZE, -1, (irq_n), py32_adc_region_read, py32_adc_region_write}
#define PY32_GPIO_REGION(n, b) \
  {#n, (b), PY32_REGION_SIZE, -1, -1, py32_gpio_region_read, py32_gpio_region_write}

static const dp32g030_region_t py32f071_regions[] = {
    PY32_REGION(TIM2, 0x40000000u, 15),
    PY32_REGION(TIM3, 0x40000400u, 16),
    PY32_REGION(TIM6, 0x40001000u, 17),
    PY32_REGION(TIM7, 0x40001400u, 18),
    PY32_REGION(TIM14, 0x40002000u, 19),
    PY32_REGION(LCD, 0x40002400u, -1),
    PY32_REGION(RTC, 0x40002800u, 2),
    PY32_REGION(WWDG, 0x40002C00u, 0),
    PY32_REGION(IWDG, 0x40003000u, -1),
    PY32_SPI_REGION(SPI2, 0x40003800u, 26),
    PY32_REGION(USART2, 0x40004400u, 28),
    PY32_REGION(USART3, 0x40004800u, 29),
    PY32_REGION(USART4, 0x40004C00u, 29),
    PY32_REGION(I2C1, 0x40005400u, 23),
    PY32_REGION(I2C2, 0x40005800u, 24),
    PY32_REGION(USBD, 0x40005C00u, 31),
    PY32_REGION(USBD_SRAM, 0x40006000u, -1),
    PY32_REGION(CTC, 0x40006C00u, -1),
    PY32_REGION(PWR, 0x40007000u, -1),
    PY32_REGION(DAC1, 0x40007400u, -1),
    PY32_REGION(LPTIM1, 0x40007C00u, -1),
    PY32_REGION(SYSCFG, 0x40010000u, -1),
    PY32_ADC_REGION(ADC1, 0x40012400u, 12),
    PY32_REGION(TIM1, 0x40012C00u, 13),
    PY32_SPI_REGION(SPI1, 0x40013000u, 25),
    PY32_REGION(USART1, 0x40013800u, 27),
    PY32_REGION(TIM15, 0x40014000u, 20),
    PY32_REGION(TIM16, 0x40014400u, 21),
    PY32_REGION(TIM17, 0x40014800u, 22),
    PY32_REGION(DBGMCU, 0x40015800u, -1),
    PY32_REGION(DMA1, 0x40020000u, 9),
    PY32_REGION(RCC, 0x40021000u, 4),
    PY32_REGION(EXTI, 0x40021800u, -1),
    PY32_REGION(FLASH_R, 0x40022000u, 3),
    PY32_REGION(CRC, 0x40023000u, -1),
    PY32_REGION(DIV, 0x40023800u, -1),
    PY32_GPIO_REGION(GPIOA, 0x50000000u),
    PY32_GPIO_REGION(GPIOB, 0x50000400u),
    PY32_GPIO_REGION(GPIOC, 0x50000800u),
    PY32_GPIO_REGION(GPIOF, 0x50001400u),
};

_Static_assert(sizeof py32f071_regions / sizeof *py32f071_regions ==
                   PY32_NREGIONS,
               "PY32_NREGIONS must match the region table");

static int py32_region_index(const dp32g030_region_t *r) {
  if (r < py32f071_regions ||
      r >= py32f071_regions + sizeof py32f071_regions / sizeof *py32f071_regions)
    return -1;
  return (int)(r - py32f071_regions);
}

static const dp32g030_region_t *py32_region_find(uint32_t addr) {
  if (addr - dp32g030_scs_region.base < dp32g030_scs_region.size)
    return &dp32g030_scs_region;
  for (size_t i = 0; i < sizeof py32f071_regions / sizeof *py32f071_regions;
       i++) {
    if (addr - py32f071_regions[i].base < py32f071_regions[i].size)
      return &py32f071_regions[i];
  }
  return NULL;
}

#define RCC_CR_HSION (1u << 8)
#define RCC_CR_HSIRDY (1u << 10)
#define RCC_CR_PLLON (1u << 24)
#define RCC_CR_PLLRDY (1u << 25)

/* State the bootloader leaves behind: clocks running, PLL ready. */
static void py32_reset(dp32g030_cpu_t *c) {
  const dp32g030_region_t *rcc = py32_region_find(0x40021000u);
  int idx = rcc ? py32_region_index(rcc) : -1;

  for (unsigned i = 0; i < PY32_NREGIONS; i++)
    for (unsigned j = 0; j < PY32_REGFILE_WORDS; j++)
      c->py32.regfile[i][j] = 0;
  py32_gpio_reset(c);
  py32_spi_reset(c);
  py32_adc_reset(c);
  if (idx >= 0)
    c->py32.regfile[idx][0] =
        RCC_CR_HSION | RCC_CR_HSIRDY | RCC_CR_PLLON | RCC_CR_PLLRDY;
}

const dp32g030_soc_t dp32g030_soc_py32f071 = {
    .name = "py32f071",
    .flash_base = 0x08000000u,
    .flash_size = 128 * 1024,
    .flash_alias_base = 0x00000000u,
    .flash_alias_size = 128 * 1024,
    .sram_base = 0x20000000u,
    .sram_size = 16 * 1024,
    .image_base = 0x08002800u,
    .has_vtor = true,
    .region_find = py32_region_find,
    .reset = py32_reset,
    .lcd = {.spi = 1, .a0_port = 0, .a0_pin = 6},
    .gpio_set_input = py32_gpio_set_input,
    .gpio_pins = py32_gpio_pins,
    .adc_set_input = py32_adc_set_input,
};
