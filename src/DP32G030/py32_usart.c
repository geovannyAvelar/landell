#include "periph.h"

#include "exc.h"

/* PY32F071 USART (STM32F0-style SR/DR/BRR/CR1…). Fusion talks Chirp on USART1. */

#define USART_SR_RXNE (1u << 5)
#define USART_SR_TC (1u << 6)
#define USART_SR_TXE (1u << 7)
#define USART_SR_ORE (1u << 3)

#define USART_CR1_RE (1u << 2)
#define USART_CR1_TE (1u << 3)
#define USART_CR1_RXNEIE (1u << 5)
#define USART_CR1_TCIE (1u << 6)
#define USART_CR1_TXEIE (1u << 7)
#define USART_CR1_UE (1u << 13)

static const int kUsartIrq[4] = {27, 28, 29, 29};

static void usart_update_irq(dp32g030_cpu_t *c, unsigned n) {
  dp32g030_py32_usart_t *u = &c->py32.usart[n];
  bool level = false;

  if ((u->cr1 & USART_CR1_RXNEIE) && (u->sr & USART_SR_RXNE))
    level = true;
  if ((u->cr1 & USART_CR1_TXEIE) && (u->sr & USART_SR_TXE))
    level = true;
  if ((u->cr1 & USART_CR1_TCIE) && (u->sr & USART_SR_TC))
    level = true;
  dp32g030_irq_set(c, kUsartIrq[n], level);
}

void py32_usart_reset(dp32g030_cpu_t *c) {
  for (unsigned n = 0; n < 4; n++) {
    dp32g030_py32_usart_t *u = &c->py32.usart[n];

    u->sr = USART_SR_TXE | USART_SR_TC;
    u->dr_tx = 0;
    u->brr = 0;
    u->cr1 = 0;
    u->cr2 = 0;
    u->cr3 = 0;
    u->gtpr = 0;
    u->rx_head = 0;
    u->rx_count = 0;
  }
}

void py32_usart_rx(dp32g030_cpu_t *c, unsigned n, uint8_t byte) {
  dp32g030_py32_usart_t *u;

  if (n >= 4)
    return;
  u = &c->py32.usart[n];
  if (!(u->cr1 & USART_CR1_UE) || !(u->cr1 & USART_CR1_RE))
    return;
  if (u->rx_count == PY32_USART_FIFO) {
    u->sr |= USART_SR_ORE;
  } else {
    u->rx[(u->rx_head + u->rx_count) % PY32_USART_FIFO] = byte;
    u->rx_count++;
    u->sr |= USART_SR_RXNE;
  }
  usart_update_irq(c, n);
  /* Fusion (and egzumer) pull RX through DMA from DR. */
  {
    static const uint32_t dr[4] = {0x40013804u, 0x40004404u, 0x40004804u,
                                   0x40004C04u};
    py32_dma_usart_rx_kick(c, dr[n]);
  }
}

bool py32_usart_read(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                     uint32_t *out) {
  dp32g030_py32_usart_t *u;

  if (n >= 4 || size < 1 || size > 4)
    return false;
  u = &c->py32.usart[n];
  switch (off & ~3u) {
  case 0x00:
    *out = u->sr;
    break;
  case 0x04:
    if (u->rx_count) {
      *out = u->rx[u->rx_head];
      u->rx_head = (u->rx_head + 1) % PY32_USART_FIFO;
      u->rx_count--;
      if (!u->rx_count)
        u->sr &= ~USART_SR_RXNE;
    } else {
      *out = 0;
    }
    usart_update_irq(c, n);
    break;
  case 0x08:
    *out = u->brr;
    break;
  case 0x0C:
    *out = u->cr1;
    break;
  case 0x10:
    *out = u->cr2;
    break;
  case 0x14:
    *out = u->cr3;
    break;
  case 0x18:
    *out = u->gtpr;
    break;
  default:
    return false;
  }
  return true;
}

bool py32_usart_write(dp32g030_cpu_t *c, unsigned n, uint32_t off, unsigned size,
                      uint32_t val) {
  dp32g030_py32_usart_t *u;
  uint32_t mask;

  if (n >= 4 || size < 1 || size > 4)
    return false;
  u = &c->py32.usart[n];
  mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  switch (off & ~3u) {
  case 0x00:
    /* Software clears some SR bits by writing 0; TXE/TC/RXNE are HW-driven. */
    u->sr = (u->sr & (USART_SR_TXE | USART_SR_TC | USART_SR_RXNE)) |
            (u->sr & val & ~(USART_SR_TXE | USART_SR_TC | USART_SR_RXNE));
    break;
  case 0x04:
    u->dr_tx = val & 0xFFu;
    if ((u->cr1 & USART_CR1_UE) && (u->cr1 & USART_CR1_TE) &&
        c->periph.hooks.uart_tx)
      /* Hook index matches USART number (1..4); GUI bridges USART1. */
      c->periph.hooks.uart_tx(c->periph.hooks.user, n + 1,
                              (uint8_t)u->dr_tx);
    u->sr |= USART_SR_TXE | USART_SR_TC;
    break;
  case 0x08:
    u->brr = (u->brr & ~mask) | (val & mask);
    break;
  case 0x0C:
    u->cr1 = (u->cr1 & ~mask) | (val & mask);
    break;
  case 0x10:
    u->cr2 = (u->cr2 & ~mask) | (val & mask);
    break;
  case 0x14:
    u->cr3 = (u->cr3 & ~mask) | (val & mask);
    break;
  case 0x18:
    u->gtpr = (u->gtpr & ~mask) | (val & mask);
    break;
  default:
    return false;
  }
  usart_update_irq(c, n);
  return true;
}
