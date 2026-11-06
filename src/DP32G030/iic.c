#include "periph.h"

#include "exc.h"

/* Master mode only; slave registers are stored but inert (see README).
   Like SPI, a transfer completes instantly through a host hook. */

#define CCFG_MODE (1u << 1)
#define CCFG_EN (1u << 0)

#define CTRANS_RX_ACK (1u << 1)
#define CTRANS_TX_ACK (1u << 0)

#define MCTRL_STO (1u << 3)
#define MCTRL_WR (1u << 2)
#define MCTRL_RD (1u << 1)
#define MCTRL_STA (1u << 0)

#define IF_MLTO (1u << 17)
#define IF_SLV_STO (1u << 9)
#define IF_SLV_STA (1u << 8)
#define IF_RXF (1u << 4)
#define IF_TXF (1u << 3)
#define IF_RXOVF (1u << 2)
#define IF_RXNE (1u << 1)
#define IF_TXE (1u << 0)
#define IF_W1C_MASK (IF_MLTO | IF_SLV_STO | IF_SLV_STA | IF_RXF | IF_TXF | IF_RXOVF)
#define IE_MASK (IF_MLTO | IF_SLV_STO | IF_SLV_STA | IF_RXF | IF_TXF | IF_RXOVF)

static unsigned iic_index(const dp32g030_region_t *r) {
  return (unsigned)(r->base - 0x400B9000u) / DP32G030_MMIO_REGION_SIZE;
}

static void iic_update_irq(dp32g030_cpu_t *c, unsigned n) {
  const dp32g030_iic_t *i = &c->periph.iic[n];

  dp32g030_irq_set(c, 18 + n, (i->iflags & i->ie & IE_MASK) != 0);
}

static void iic_mctrl_write(dp32g030_cpu_t *c, unsigned n, uint32_t val) {
  dp32g030_iic_t *i = &c->periph.iic[n];
  const dp32g030_hooks_t *h = &c->periph.hooks;

  if (!(i->ccfg & CCFG_EN) || !(i->ccfg & CCFG_MODE))
    return; /* disabled, or slave mode (not modeled) */

  if (val & MCTRL_STA) {
    i->busy = true;
    if (h->i2c_start)
      h->i2c_start(h->user, n);
  }
  if (val & MCTRL_WR) {
    bool ack = h->i2c_write ? h->i2c_write(h->user, n, i->txdata) : false;

    i->ctrans = (i->ctrans & ~CTRANS_RX_ACK) | (ack ? 0 : CTRANS_RX_ACK);
    i->iflags |= IF_TXF | IF_TXE;
  }
  if (val & MCTRL_RD) {
    bool ack = !(i->ctrans & CTRANS_TX_ACK);

    uint8_t byte = h->i2c_read ? h->i2c_read(h->user, n, ack) : 0xFFu;

    if (i->iflags & IF_RXNE) {
      i->iflags |= IF_RXOVF; /* previous byte was never read; this one is lost */
    } else {
      i->rxdata = byte;
      i->iflags |= IF_RXNE;
    }
    i->iflags |= IF_RXF;
  }
  if (val & MCTRL_STO) {
    i->busy = false;
    if (h->i2c_stop)
      h->i2c_stop(h->user, n);
  }
  iic_update_irq(c, n);
}

bool dp32g030_iic_read(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                       uint32_t off, unsigned size, uint32_t *out) {
  unsigned n = iic_index(r);
  dp32g030_iic_t *i = &c->periph.iic[n];
  uint32_t v;

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: v = i->ccfg; break;
  case 0x04: v = i->busy ? 1u : (1u << 2) | (1u << 1); /* idle: SDA=SCL=1 */
    break;
  case 0x08: v = i->ctrans; break;
  case 0x0C:
    v = i->rxdata;
    i->iflags &= ~IF_RXNE;
    iic_update_irq(c, n);
    break;
  case 0x10: v = i->txdata; break;
  case 0x14: v = i->ie; break;
  case 0x18: v = i->iflags; break;
  case 0x20: v = 0; break; /* MCTRL bits self-clear */
  case 0x30: v = i->sctrl; break;
  case 0x34: v = i->saddr; break;
  default: return dp32g030_regfile_read(c, r, off, size, out);
  }
  *out = v;
  return true;
}

bool dp32g030_iic_write(dp32g030_cpu_t *c, const dp32g030_region_t *r,
                        uint32_t off, unsigned size, uint32_t val) {
  unsigned n = iic_index(r);
  dp32g030_iic_t *i = &c->periph.iic[n];

  if (size != 4)
    return false;
  switch (off) {
  case 0x00: i->ccfg = val & 0x7Fu; break;
  case 0x04: break; /* read-only */
  case 0x08: i->ctrans = (i->ctrans & CTRANS_RX_ACK) | (val & CTRANS_TX_ACK);
    break;
  case 0x0C: break; /* read-only */
  case 0x10:
    i->txdata = (uint8_t)val;
    i->iflags &= ~IF_TXE;
    break;
  case 0x14: i->ie = val & IE_MASK; break;
  case 0x18: i->iflags &= ~(val & IF_W1C_MASK); break;
  case 0x20: iic_mctrl_write(c, n, val); break;
  case 0x30: i->sctrl = val & 0xFu; break;
  case 0x34: i->saddr = val & 0xFFFFFFu; break;
  default: return dp32g030_regfile_write(c, r, off, size, val);
  }
  iic_update_irq(c, n);
  return true;
}
