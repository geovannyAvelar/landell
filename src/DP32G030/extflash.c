#include "periph.h"

#include "socdesc.h"

#include <stdlib.h>
#include <string.h>

/* PY25Q16-style SPI NOR flash: 2 MiB, 4 KiB sectors, 256-byte pages. A byte
   exchange is one SPI transfer; the active-low chip select is a GPIO of the
   SoC. Programming and erasing complete instantly (WIP is never set).
   Supported: READ 03, FAST READ 0B, status 05/35, JEDEC ID 9F, write enable
   06/04, page program 02, sector/block erase 20/52/D8, chip erase C7/60; the
   rest is ignored (reads return 0xFF). */

#define SECTOR_SIZE 0x1000u
#define PAGE_SIZE 0x100u
#define ADDR_MASK (DP32G030_EXTFLASH_SIZE - 1u)

#define CMD_WRITE_STATUS 0x01
#define CMD_PAGE_PROGRAM 0x02
#define CMD_READ 0x03
#define CMD_WRITE_DISABLE 0x04
#define CMD_READ_STATUS 0x05
#define CMD_WRITE_ENABLE 0x06
#define CMD_FAST_READ 0x0B
#define CMD_SECTOR_ERASE 0x20
#define CMD_READ_STATUS2 0x35
#define CMD_BLOCK_ERASE_32K 0x52
#define CMD_CHIP_ERASE_60 0x60
#define CMD_JEDEC_ID 0x9F
#define CMD_CHIP_ERASE_C7 0xC7
#define CMD_BLOCK_ERASE_64K 0xD8

#define STATUS_WEL 0x02

static const uint8_t kJedecId[3] = {0x85, 0x20, 0x15};

bool dp32g030_extflash_init(dp32g030_cpu_t *c) {
  if (!c->soc->extflash.present)
    return true;
  c->extflash.mem = malloc(DP32G030_EXTFLASH_SIZE);
  if (!c->extflash.mem)
    return false;
  memset(c->extflash.mem, 0xFF, DP32G030_EXTFLASH_SIZE);
  dp32g030_extflash_reset_bus(c);
  return true;
}

void dp32g030_extflash_reset_bus(dp32g030_cpu_t *c) {
  dp32g030_extflash_t *f = &c->extflash;

  f->cs_low = false;
  f->selected = false;
  f->wel = false;
  f->cmd = 0;
  f->nbytes = 0;
  f->addr = 0;
}

void dp32g030_extflash_set(dp32g030_cpu_t *c, uint32_t offset,
                           const uint8_t *data, uint32_t len) {
  if (!c->extflash.mem)
    return;
  for (uint32_t i = 0; i < len && offset + i < DP32G030_EXTFLASH_SIZE; i++)
    c->extflash.mem[offset + i] = data[i];
}

void dp32g030_extflash_get(const dp32g030_cpu_t *c, uint32_t offset,
                           uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i++)
    data[i] = c->extflash.mem && offset + i < DP32G030_EXTFLASH_SIZE
                  ? c->extflash.mem[offset + i]
                  : 0xFF;
}

static void erase(dp32g030_extflash_t *f, uint32_t addr, uint32_t size) {
  uint32_t base = (addr & ADDR_MASK) & ~(size - 1u);

  memset(f->mem + base, 0xFF, size);
  f->wel = false;
}

/* Chip select edges delimit transactions: a falling edge opens one, a rising
   edge ends it (a page program that took data has then consumed the write
   enable latch). */
void dp32g030_extflash_cs(dp32g030_cpu_t *c) {
  const dp32g030_soc_t *soc = c->soc;
  dp32g030_extflash_t *f = &c->extflash;

  if (!f->mem)
    return;
  bool cs_low = !(soc->gpio_pins(c, soc->extflash.cs_port) &
                  (1u << soc->extflash.cs_pin));

  if (cs_low && !f->cs_low) {
    f->selected = true;
    f->nbytes = 0;
  } else if (!cs_low && f->cs_low) {
    if (f->cmd == CMD_PAGE_PROGRAM && f->nbytes > 4)
      f->wel = false;
    f->selected = false;
  }
  f->cs_low = cs_low;
}

uint8_t dp32g030_extflash_xfer(dp32g030_cpu_t *c, uint8_t tx) {
  dp32g030_extflash_t *f = &c->extflash;

  if (!f->mem || !f->selected)
    return 0xFF;

  unsigned n = f->nbytes++;

  if (n == 0) {
    f->cmd = tx;
    f->addr = 0;
    switch (tx) {
    case CMD_WRITE_ENABLE: f->wel = true; break;
    case CMD_WRITE_DISABLE: f->wel = false; break;
    case CMD_CHIP_ERASE_C7:
    case CMD_CHIP_ERASE_60:
      if (f->wel) {
        memset(f->mem, 0xFF, DP32G030_EXTFLASH_SIZE);
        f->wel = false;
      }
      break;
    default: break;
    }
    return 0xFF;
  }

  switch (f->cmd) {
  case CMD_READ_STATUS: return f->wel ? STATUS_WEL : 0;
  case CMD_READ_STATUS2: return 0;
  case CMD_JEDEC_ID: return n <= 3 ? kJedecId[n - 1] : 0xFF;
  case CMD_READ:
  case CMD_FAST_READ: {
    unsigned header = f->cmd == CMD_FAST_READ ? 4 : 3; /* address (+ dummy) */

    if (n <= 3) {
      f->addr = (f->addr << 8) | tx;
      return 0xFF;
    }
    if (n <= header)
      return 0xFF;
    uint8_t v = f->mem[f->addr & ADDR_MASK];

    f->addr = (f->addr + 1) & ADDR_MASK;
    return v;
  }
  case CMD_PAGE_PROGRAM:
    if (n <= 3) {
      f->addr = (f->addr << 8) | tx;
      return 0xFF;
    }
    if (f->wel) {
      uint32_t a = f->addr & ADDR_MASK;

      f->mem[a] &= tx; /* programming can only clear bits */
      f->addr = (a & ~(PAGE_SIZE - 1u)) | ((a + 1u) & (PAGE_SIZE - 1u));
    }
    return 0xFF;
  case CMD_SECTOR_ERASE:
  case CMD_BLOCK_ERASE_32K:
  case CMD_BLOCK_ERASE_64K:
    if (n <= 3) {
      f->addr = (f->addr << 8) | tx;
      if (n == 3 && f->wel)
        erase(f, f->addr,
              f->cmd == CMD_SECTOR_ERASE ? SECTOR_SIZE
              : f->cmd == CMD_BLOCK_ERASE_32K ? 0x8000u : 0x10000u);
    }
    return 0xFF;
  default: return 0xFF;
  }
}
