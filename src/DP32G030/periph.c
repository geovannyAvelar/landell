#include "periph.h"

#include "exc.h"

#include <string.h>

void dp32g030_periph_reset(dp32g030_cpu_t *c) {
  dp32g030_periph_t *p = &c->periph;
  dp32g030_hooks_t hooks = p->hooks;
  uint32_t adc_in[DP32G030_ADC_CHANNELS];
  uint32_t in[3] = {p->gpio[0].in, p->gpio[1].in, p->gpio[2].in};
  uint32_t driven[3] = {p->gpio[0].driven, p->gpio[1].driven, p->gpio[2].driven};

  memcpy(adc_in, p->adc.input, sizeof adc_in);
  memset(p, 0, sizeof *p);
  memcpy(p->adc.input, adc_in, sizeof adc_in);
  p->hooks = hooks;
  for (unsigned i = 0; i < 3; i++) {
    p->gpio[i].in = in[i];
    p->gpio[i].driven = driven[i];
  }
  for (unsigned i = 0; i < 3; i++) {
    p->timer_base[i].high_load = 0xFFFFu;
    p->timer_base[i].low_load = 0xFFFFu;
  }
  for (unsigned i = 0; i < 2; i++) {
    p->timer_plus[i].ctr = 0x600060u;
    p->timer_plus[i].high_load = 0xFFFFu;
    p->timer_plus[i].low_load = 0xFFFFu;
  }
  for (unsigned i = 0; i < 2; i++)
    p->pwm_base[i].cnt = 0xFFFFu;
  for (unsigned i = 0; i < 2; i++)
    p->pwm_plus[i].period = 0xFFFFu;
  p->rtc.pre = 0x7FFFu;
  p->rtc.dr = 0x101u;
  p->rtc.date = 1;
  p->rtc.month = 1;
  p->iwdt.load = 0xFFFFu;
  p->iwdt.cnt = 0xFFFFu;
  p->wwdt.load = 0x40u;
  p->wwdt.value = 0x7Fu;
  for (unsigned i = 0; i < 2; i++) {
    p->iic[i].ccfg = 0x18u;
    p->iic[i].iflags = 0x01u; /* TXE */
    p->iic[i].sctrl = 0x08u;
  }
  p->bk4819.regs[0x0D] = 1u << 15; /* frequency scan: not finished */
  p->bk4819.regs[0x02] = 1u << 14; /* FSK FIFO: almost empty (it's empty) */
}

/* ---- generic register file ---------------------------------------- */

static uint32_t *dp32g030_regfile_slot(dp32g030_cpu_t *c,
                                       const dp32g030_region_t *r,
                                       uint32_t off) {
  int idx = dp32g030_mmio_index(r);

  if (idx < 0 || off >= DP32G030_MMIO_REGION_SIZE)
    return NULL;
  return &c->periph.regfile[idx][off / 4];
}

bool dp32g030_regfile_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                           uint32_t off, unsigned size, uint32_t *out) {
  uint32_t *w = dp32g030_regfile_slot(c, r, off & ~3u);

  if (!w)
    return false;
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (*w >> shift) & mask;
  return true;
}

bool dp32g030_regfile_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                            uint32_t off, unsigned size, uint32_t val) {
  uint32_t *w = dp32g030_regfile_slot(c, r, off & ~3u);

  if (!w)
    return false;
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *w = (*w & ~(mask << shift)) | ((val & mask) << shift);
  return true;
}

/* ---- GPIO ---------------------------------------------------------- */

static unsigned dp32g030_gpio_port(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x40060000u) / DP32G030_MMIO_REGION_SIZE;
}

/* PORTCON's PORTx_PU pull-up-enable word for `port` (0=A,1=B,2=C), read
   from its generic register-file storage; 0 if PORTCON isn't mapped. */
static uint32_t dp32g030_portcon_pullup(const dp32g030_cpu_t *c, unsigned port) {
  const dp32g030_region_t *r = dp32g030_mmio_find(0x400B0000u);
  int idx = r ? dp32g030_mmio_index(r) : -1;

  if (idx < 0)
    return 0;
  return c->periph.regfile[idx][(0x200 + 4 * port) / 4];
}

uint32_t dp32g030_gpio_pins_dp32(const dp32g030_cpu_t *c, unsigned port) {
  const dp32g030_gpio_t *g = &c->periph.gpio[port];
  uint32_t level = (g->out & g->dir) | (g->in & ~g->dir);
  /* An input pin nothing has ever driven floats to its pull-up, if any
     (PORTCON has no behaviour of its own otherwise; see README). A
     pull-down needs no extra handling: that is already the default. */
  uint32_t floating = ~g->dir & ~g->driven;

  return level | (dp32g030_portcon_pullup(c, port) & floating);
}

static void dp32g030_gpio_notify(dp32g030_cpu_t *c, unsigned port,
                                 uint32_t before) {
  const dp32g030_hooks_t *h = &c->periph.hooks;
  uint32_t now = dp32g030_gpio_pins_dp32(c, port);

  if (now != before && h->gpio_change)
    h->gpio_change(h->user, port, now, c->periph.gpio[port].dir);
}

uint32_t dp32g030_gpio_dir_dp32(const dp32g030_cpu_t *c, unsigned port) {
  return c->periph.gpio[port].dir;
}

void dp32g030_gpio_set_input_dp32(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                             uint32_t value) {
  dp32g030_gpio_t *g;

  if (port >= 3)
    return;
  g = &c->periph.gpio[port];
  uint32_t before = dp32g030_gpio_pins_dp32(c, port);
  g->in = (g->in & ~mask) | (value & mask);
  g->driven |= mask;
  dp32g030_gpio_notify(c, port, before);
}

bool dp32g030_gpio_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out) {
  unsigned port = dp32g030_gpio_port(r);
  uint32_t v;

  if (off >= 8)
    return dp32g030_regfile_read(c, r, off, size, out);
  if (size != 4)
    return false;
  v = off == 0 ? dp32g030_gpio_pins_dp32(c, port) : c->periph.gpio[port].dir;
  *out = v;
  return true;
}

bool dp32g030_gpio_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val) {
  unsigned port = dp32g030_gpio_port(r);
  dp32g030_gpio_t *g = &c->periph.gpio[port];

  if (off >= 8)
    return dp32g030_regfile_write(c, r, off, size, val);
  if (size != 4)
    return false;
  uint32_t before = dp32g030_gpio_pins_dp32(c, port);
  if (off == 0)
    g->out = val;
  else
    g->dir = val;
  dp32g030_gpio_notify(c, port, before);
  if (port == 2)
    dp32g030_bk4819_gpio_edge(c, before, dp32g030_gpio_pins_dp32(c, port), g->dir);
  return true;
}

/* ---- UART ---------------------------------------------------------- */

#define UART_IF_TXDONE (1u << 2)
#define UART_IF_RXTO (1u << 5)
#define UART_IF_RXFIFO (1u << 6)
#define UART_IF_TXFIFO (1u << 7)
#define UART_IF_RXFIFO_OVF (1u << 8)
#define UART_IF_RXFIFO_EMPTY (1u << 10)
#define UART_IF_RXFIFO_FULL (1u << 11)
#define UART_IF_RXFIFO_HFULL (1u << 12)
#define UART_IF_TXFIFO_EMPTY (1u << 13)
#define UART_STICKY_MASK 0x0000033Cu /* bits 2-5, 8, 9 */
#define UART_IRQ_MASK 0x000003FCu
#define UART_FIFO_RF_CLR (1u << 6)
#define UART_FIFO_TF_CLR (1u << 7)

#define UART_CYCLES_PER_BIT_DEFAULT 1u

static unsigned dp32g030_uart_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x4006B000u) / DP32G030_MMIO_REGION_SIZE;
}

static unsigned dp32g030_uart_threshold(const dp32g030_uart_t *u) {
  return (u->fifo & 7u) + 1u;
}

static uint32_t dp32g030_uart_flags(const dp32g030_uart_t *u) {
  uint32_t f = u->sticky;

  if (u->rx_count >= dp32g030_uart_threshold(u))
    f |= UART_IF_RXFIFO;
  f |= UART_IF_TXFIFO | UART_IF_TXFIFO_EMPTY;
  if (u->rx_count == 0)
    f |= UART_IF_RXFIFO_EMPTY;
  if (u->rx_count == DP32G030_UART_FIFO_DEPTH)
    f |= UART_IF_RXFIFO_FULL;
  if (u->rx_count >= DP32G030_UART_FIFO_DEPTH / 2)
    f |= UART_IF_RXFIFO_HFULL;
  f |= (u->rx_count & 7u) << 17;
  return f;
}

static void dp32g030_uart_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_uart_t *u = &c->periph.uart[n];
  bool level = (dp32g030_uart_flags(u) & u->ie & UART_IRQ_MASK) != 0;

  dp32g030_irq_set(c, 13 + n, level);
}

/* Time to receive `chars` characters of 10 bits each. */
static uint64_t dp32g030_uart_char_cycles(const dp32g030_uart_t *u) {
  uint64_t bit = (uint64_t)u->baud + UART_CYCLES_PER_BIT_DEFAULT;

  return 10u * bit;
}

void dp32g030_uart_rx(dp32g030_cpu_t *c, unsigned n, uint8_t byte) {
  dp32g030_uart_t *u;

  if (n >= 3)
    return;
  u = &c->periph.uart[n];
  if (u->rx_count == DP32G030_UART_FIFO_DEPTH) {
    u->sticky |= UART_IF_RXFIFO_OVF;
  } else {
    u->rx[(u->rx_head + u->rx_count) % DP32G030_UART_FIFO_DEPTH] = byte;
    u->rx_count++;
  }
  u->rxto_deadline =
      c->cycles + (uint64_t)(u->rxto & 0x1FFu) * dp32g030_uart_char_cycles(u);
  if (u->rxto_deadline == 0)
    u->rxto_deadline = 1;
  dp32g030_uart_update_irq(c, n);
}

void dp32g030_uart_tick(dp32g030_cpu_t *c) {
  for (unsigned n = 0; n < 3; n++) {
    dp32g030_uart_t *u = &c->periph.uart[n];

    if (!u->rxto_deadline || c->cycles < u->rxto_deadline)
      continue;
    u->rxto_deadline = 0;
    /* Timeout only fires for data below the FIFO threshold. */
    if (u->rx_count && u->rx_count < dp32g030_uart_threshold(u)) {
      u->sticky |= UART_IF_RXTO;
      dp32g030_uart_update_irq(c, n);
    }
  }
}

uint64_t dp32g030_uart_next_event(const dp32g030_cpu_t *c) {
  uint64_t best = UINT64_MAX;

  for (unsigned n = 0; n < 3; n++) {
    const dp32g030_uart_t *u = &c->periph.uart[n];

    if (!u->rxto_deadline)
      continue;
    uint64_t d = u->rxto_deadline > c->cycles ? u->rxto_deadline - c->cycles : 1;
    if (d < best)
      best = d;
  }
  return best;
}

bool dp32g030_uart_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t *out) {
  unsigned n = dp32g030_uart_index(r);
  dp32g030_uart_t *u = &c->periph.uart[n];
  uint32_t v;

  if (size != 4)
    return false;

  switch (off) {
  case 0x00:
    v = u->ctrl;
    break;
  case 0x04:
    v = u->baud;
    break;
  case 0x08:
    v = 0; /* TDR is write-only */
    break;
  case 0x0C:
    v = 0;
    if (u->rx_count) {
      v = u->rx[u->rx_head];
      u->rx_head = (u->rx_head + 1) % DP32G030_UART_FIFO_DEPTH;
      u->rx_count--;
      if (!u->rx_count)
        u->rxto_deadline = 0;
      dp32g030_uart_update_irq(c, n);
    }
    break;
  case 0x10:
    v = u->ie;
    break;
  case 0x14:
    v = dp32g030_uart_flags(u);
    break;
  case 0x18:
    v = u->fifo & 0x3Fu;
    break;
  case 0x1C:
    v = u->fc;
    break;
  case 0x20:
    v = u->rxto;
    break;
  default:
    return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_uart_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                         uint32_t off, unsigned size, uint32_t val) {
  unsigned n = dp32g030_uart_index(r);
  dp32g030_uart_t *u = &c->periph.uart[n];

  if (size != 4)
    return false;

  switch (off) {
  case 0x00:
    u->ctrl = val;
    break;
  case 0x04:
    u->baud = val;
    break;
  case 0x08:
    /* The shifter is modelled as instantaneous. */
    if (c->periph.hooks.uart_tx)
      c->periph.hooks.uart_tx(c->periph.hooks.user, n, (uint8_t)val);
    u->sticky |= UART_IF_TXDONE;
    break;
  case 0x10:
    u->ie = val;
    break;
  case 0x14:
    u->sticky &= ~(val & UART_STICKY_MASK);
    break;
  case 0x18:
    u->fifo = val & 0x3Fu;
    if (val & UART_FIFO_RF_CLR) {
      u->rx_head = 0;
      u->rx_count = 0;
      u->rxto_deadline = 0;
    }
    (void)UART_FIFO_TF_CLR; /* TX FIFO is always empty */
    break;
  case 0x1C:
    u->fc = val;
    break;
  case 0x20:
    u->rxto = val;
    break;
  default:
    return dp32g030_regfile_write(c, r, off, size, val);
  }
  dp32g030_uart_update_irq(c, n);
  return true;
}
