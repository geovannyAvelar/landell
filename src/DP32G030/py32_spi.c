#include "periph.h"

#include "socdesc.h"

/* PY32F071 SPI, STM32-style: CR1 (MSTR bit 2, SPE bit 6), CR2, SR (RXNE bit
   0, TXE bit 1, BSY bit 7), DR. A transfer completes instantly through the
   host hook; TXE is always set and BSY never. */

#define CR1_MSTR (1u << 2)
#define CR1_SPE (1u << 6)
#define SR_RXNE (1u << 0)
#define SR_TXE (1u << 1)

void py32_spi_reset(dp32g030_cpu_t *c) {
  c->py32.spi[0] = (dp32g030_py32_spi_t){0};
  c->py32.spi[1] = (dp32g030_py32_spi_t){0};
}

bool py32_spi_read(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                   uint32_t *out) {
  dp32g030_py32_spi_t *s = &c->py32.spi[n - 1];
  uint32_t v;

  switch (off & ~3u) {
  case 0x00: v = s->cr1; break;
  case 0x04: v = s->cr2; break;
  case 0x08: v = SR_TXE | (s->rxne ? SR_RXNE : 0); break;
  case 0x0C:
    v = s->rx;
    s->rxne = false;
    break;
  default: return false;
  }
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (v >> shift) & mask;
  return true;
}

bool py32_spi_write(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                    uint32_t val) {
  dp32g030_py32_spi_t *s = &c->py32.spi[n - 1];
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);

  switch (off & ~3u) {
  case 0x00:
    s->cr1 = (s->cr1 & ~(mask << shift)) | ((val & mask) << shift);
    break;
  case 0x04:
    s->cr2 = (s->cr2 & ~(mask << shift)) | ((val & mask) << shift);
    break;
  case 0x08: /* SR: flags are cleared by reading DR */
    break;
  case 0x0C:
    if ((s->cr1 & CR1_SPE) && (s->cr1 & CR1_MSTR)) {
      const dp32g030_hooks_t *h = &c->periph.hooks;
      uint8_t tx = (uint8_t)(val & mask);
      uint8_t rx = h->spi_xfer ? h->spi_xfer(h->user, n, tx) : 0;

      if (n == c->soc->lcd.spi) {
        bool a0 = (c->soc->gpio_pins(c, c->soc->lcd.a0_port) &
                   (1u << c->soc->lcd.a0_pin)) != 0;

        dp32g030_lcd_spi_byte(c, tx, a0);
      }
      s->rx = rx;
      s->rxne = true;
    }
    break;
  default: return false;
  }
  return true;
}
