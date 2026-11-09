#include "periph.h"

#include "exc.h"
#include "socdesc.h"

/* PY32F071 SPI, STM32-style: CR1 (MSTR bit 2, SPE bit 6), CR2, SR (RXNE bit
   0, TXE bit 1, BSY bit 7), DR. A transfer completes instantly through the
   host hook; TXE is always set and BSY never. */

#define CR1_MSTR (1u << 2)
#define CR1_SPE (1u << 6)
#define SR_RXNE (1u << 0)
#define SR_TXE (1u << 1)
#define CR2_RXNEIE (1u << 6)
#define CR2_TXEIE (1u << 7)
#define IRQ_SPI1 25u
#define IRQ_SPI2 26u

static void spi_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_py32_spi_t *s = &c->py32.spi[n - 1];
  bool line = ((s->cr2 & CR2_TXEIE) != 0) /* TXE is always set */ ||
              ((s->cr2 & CR2_RXNEIE) && s->rxne);

  dp32g030_irq_set(c, n == 1 ? IRQ_SPI1 : IRQ_SPI2, line);
}

void py32_spi_reset(dp32g030_cpu_t *c) {
  c->py32.spi[0] = (dp32g030_py32_spi_t){0};
  c->py32.spi[1] = (dp32g030_py32_spi_t){0};
  spi_update_irq(c, 1);
  spi_update_irq(c, 2);
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
    spi_update_irq(c, n);
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
  bool kick_dma = false;

  switch (off & ~3u) {
  case 0x00: {
    uint32_t old = s->cr1;

    s->cr1 = (s->cr1 & ~(mask << shift)) | ((val & mask) << shift);
    /* DMA may have been armed while SPE was clear (PY25Q16 bulk xfer). */
    kick_dma = !(old & CR1_SPE) && (s->cr1 & CR1_SPE);
    break;
  }
  case 0x04:
    s->cr2 = (s->cr2 & ~(mask << shift)) | ((val & mask) << shift);
    kick_dma = (s->cr1 & CR1_SPE) != 0;
    break;
  case 0x08: /* SR: flags are cleared by reading DR */
    break;
  case 0x0C:
    if ((s->cr1 & CR1_SPE) && (s->cr1 & CR1_MSTR)) {
      const dp32g030_hooks_t *h = &c->periph.hooks;
      uint8_t tx = (uint8_t)(val & mask);
      uint8_t rx = h->spi_xfer ? h->spi_xfer(h->user, n, tx) : 0;

      if (c->soc->extflash.present && n == c->soc->extflash.spi &&
          c->extflash.selected)
        rx = dp32g030_extflash_xfer(c, tx); /* the flash answers while selected */

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
  spi_update_irq(c, n);
  if (kick_dma)
    py32_dma_spi_kick(c);
  return true;
}
