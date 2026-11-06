#include "periph.h"

#include "socdesc.h"

/* 24C64-style EEPROM (device address 0xA0, 16-bit memory address, 32-byte
   write pages) on a bit-banged I2C bus. The master (the firmware) drives both
   lines as GPIO outputs; the EEPROM answers by pulling SDA low through the
   SoC's gpio_set_input (the board has an external pull-up, so SDA idles
   high). Decoding is event based: START/STOP are SDA edges while SCL is high,
   bits are sampled on SCL rising edges, and the EEPROM changes SDA on SCL
   falling edges. */

enum {
  ST_IDLE,    /* waiting for START (or ignoring a transaction not for us) */
  ST_ADDR,    /* device address byte */
  ST_PTR_HI,  /* memory address, high byte */
  ST_PTR_LO,  /* memory address, low byte */
  ST_DATA_IN, /* write data */
  ST_SEND,    /* read data */
};

#define PAGE_SIZE 32u
#define ADDR_MASK (DP32G030_EEPROM_SIZE - 1u)

void dp32g030_eeprom_init(dp32g030_cpu_t *c) {
  for (unsigned i = 0; i < DP32G030_EEPROM_SIZE; i++)
    c->eeprom.mem[i] = 0xFF;
  dp32g030_eeprom_reset_bus(c);
}

void dp32g030_eeprom_reset_bus(dp32g030_cpu_t *c) {
  dp32g030_eeprom_t *e = &c->eeprom;

  e->prev_scl = e->prev_sda = true;
  e->slave_low = false;
  e->master_ack = false;
  e->state = ST_IDLE;
  e->bitcount = 0;
  e->shift = 0;
  e->reading = false;
}

void dp32g030_eeprom_set(dp32g030_cpu_t *c, uint32_t offset, const uint8_t *data,
                         uint32_t len) {
  for (uint32_t i = 0; i < len && offset + i < DP32G030_EEPROM_SIZE; i++)
    c->eeprom.mem[offset + i] = data[i];
}

void dp32g030_eeprom_get(const dp32g030_cpu_t *c, uint32_t offset, uint8_t *data,
                         uint32_t len) {
  for (uint32_t i = 0; i < len; i++)
    data[i] = offset + i < DP32G030_EEPROM_SIZE ? c->eeprom.mem[offset + i] : 0xFF;
}

static void load_out(dp32g030_eeprom_t *e) {
  e->out = e->mem[e->ptr & ADDR_MASK];
  e->bitcount = 0;
  e->slave_low = !(e->out & 0x80u);
}

static void next_page_byte(dp32g030_eeprom_t *e) {
  e->ptr = (uint16_t)((e->ptr & ~(PAGE_SIZE - 1u)) | ((e->ptr + 1u) & (PAGE_SIZE - 1u)));
}

/* The 8th clock just ended on a byte the master sent: act on it and decide
   whether to acknowledge. */
static void byte_received(dp32g030_eeprom_t *e) {
  switch (e->state) {
  case ST_ADDR:
    if ((e->shift & 0xFEu) == 0xA0u) {
      e->slave_low = true; /* ACK */
      e->reading = e->shift & 1u;
    } else {
      e->state = ST_IDLE; /* not for us: stay silent until the next START */
    }
    break;
  case ST_PTR_HI:
    e->ptr = (uint16_t)(((e->shift << 8) | (e->ptr & 0xFFu)) & ADDR_MASK);
    e->slave_low = true;
    break;
  case ST_PTR_LO:
    e->ptr = (uint16_t)(((e->ptr & 0xFF00u) | e->shift) & ADDR_MASK);
    e->slave_low = true;
    break;
  case ST_DATA_IN:
    e->mem[e->ptr & ADDR_MASK] = e->shift;
    next_page_byte(e);
    e->slave_low = true;
    break;
  default:
    break;
  }
}

static void on_rising(dp32g030_eeprom_t *e, bool sda) {
  if (e->state == ST_IDLE)
    return;
  if (e->bitcount < 8) {
    if (e->state != ST_SEND)
      e->shift = (uint8_t)((e->shift << 1) | (sda ? 1u : 0u));
    e->bitcount++;
    return;
  }
  /* 9th clock */
  e->bitcount++;
  if (e->state == ST_SEND)
    e->master_ack = !sda;
}

static void on_falling(dp32g030_eeprom_t *e) {
  if (e->state == ST_IDLE)
    return;
  if (e->state == ST_SEND) {
    if (e->bitcount < 8) {
      e->slave_low = !((e->out >> (7 - e->bitcount)) & 1u);
    } else if (e->bitcount == 8) {
      e->slave_low = false; /* release SDA for the master's ACK */
    } else { /* bitcount == 9 */
      if (e->master_ack) {
        next_page_byte(e);
        load_out(e);
      } else {
        e->slave_low = false;
        e->state = ST_IDLE;
      }
    }
    return;
  }
  if (e->bitcount == 8) {
    byte_received(e);
  } else if (e->bitcount == 9) {
    e->slave_low = false;
    e->bitcount = 0;
    e->shift = 0;
    switch (e->state) {
    case ST_ADDR:
      if (e->reading) {
        e->state = ST_SEND;
        load_out(e);
      } else {
        e->state = ST_PTR_HI;
      }
      break;
    case ST_PTR_HI: e->state = ST_PTR_LO; break;
    case ST_PTR_LO: e->state = ST_DATA_IN; break;
    default: break;
    }
  }
}

void dp32g030_eeprom_bus(dp32g030_cpu_t *c) {
  const dp32g030_soc_t *soc = c->soc;
  dp32g030_eeprom_t *e = &c->eeprom;

  if (!soc->eeprom.present)
    return;
  uint32_t scl_bit = 1u << soc->eeprom.scl_pin;
  uint32_t sda_bit = 1u << soc->eeprom.sda_pin;
  uint32_t level = soc->gpio_pins(c, soc->eeprom.port);
  uint32_t dir = soc->gpio_dir(c, soc->eeprom.port);
  bool scl = (level & scl_bit) != 0;
  /* the master's SDA: its output level, or released (high) when it listens */
  bool sda_master = (dir & sda_bit) ? (level & sda_bit) != 0 : true;
  bool sda_bus = sda_master && !e->slave_low;

  if (e->prev_scl && scl && e->prev_sda != sda_master) {
    if (!sda_master) { /* START */
      e->state = ST_ADDR;
      e->bitcount = 0;
      e->shift = 0;
      e->slave_low = false;
    } else { /* STOP */
      e->state = ST_IDLE;
      e->slave_low = false;
    }
  } else if (!e->prev_scl && scl) {
    on_rising(e, sda_bus);
  } else if (e->prev_scl && !scl) {
    on_falling(e);
  }
  e->prev_scl = scl;
  e->prev_sda = sda_master;

  /* SDA idles high through the board's pull-up; the EEPROM pulls it low. */
  soc->gpio_set_input(c, soc->eeprom.port, sda_bit, e->slave_low ? 0 : sda_bit);
}
