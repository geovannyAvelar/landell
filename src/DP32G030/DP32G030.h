#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP32G030_FLASH_BASE 0x00000000u
#define DP32G030_FLASH_SIZE (64 * 1024)
#define DP32G030_SRAM_BASE 0x20000000u
#define DP32G030_SRAM_SIZE (16 * 1024)

typedef enum {
  OP_UNDEF = 0,
  OP_LSLS_IMM,
  OP_LSRS_IMM,
  OP_ASRS_IMM,
  OP_ADDS_REG,
  OP_SUBS_REG,
  OP_ADDS_IMM,
  OP_SUBS_IMM,
  OP_MOVS_IMM,
  OP_MOVS_REG,
  OP_MOV_REG,
  OP_CMP_IMM,
  OP_CMP_REG,
  OP_CMN,
  OP_ANDS,
  OP_EORS,
  OP_ORRS,
  OP_BICS,
  OP_MVNS,
  OP_TST,
  OP_ADCS,
  OP_SBCS,
  OP_RSBS,
  OP_MULS,
  OP_LSLS_REG,
  OP_LSRS_REG,
  OP_ASRS_REG,
  OP_RORS_REG,
  OP_ADD_REG,
  OP_ADD_SP_IMM,
  OP_ADD_SP_SP_IMM,
  OP_SUB_SP_SP_IMM,
  OP_ADR,
  OP_LDR_IMM,
  OP_STR_IMM,
  OP_LDRB_IMM,
  OP_STRB_IMM,
  OP_LDRH_IMM,
  OP_STRH_IMM,
  OP_LDR_SP,
  OP_STR_SP,
  OP_LDR_LIT,
  OP_LDR_REG,
  OP_STR_REG,
  OP_LDRB_REG,
  OP_STRB_REG,
  OP_LDRH_REG,
  OP_STRH_REG,
  OP_LDRSB_REG,
  OP_LDRSH_REG,
  OP_LDM,
  OP_STM,
  OP_PUSH,
  OP_POP,
  OP_B_COND,
  OP_B,
  OP_BL,
  OP_BX,
  OP_BLX_REG,
  OP_SXTB,
  OP_SXTH,
  OP_UXTB,
  OP_UXTH,
  OP_REV,
  OP_REV16,
  OP_REVSH,
  OP_NOP,
  OP_YIELD,
  OP_WFE,
  OP_WFI,
  OP_SEV,
  OP_SVC,
  OP_BKPT,
  OP_UDF,
  OP_CPSID,
  OP_CPSIE,
  OP_MRS,
  OP_MSR,
  OP_DMB,
  OP_DSB,
  OP_ISB,
  OP_COUNT,
} dp32g030_op_t;

typedef enum {
  EMU_OK = 0,
  EMU_FAULT,
  EMU_UNDEF,
} dp32g030_status_t;

typedef struct {
  dp32g030_op_t op;
  uint8_t rd, rn, rm;
  uint32_t imm;
  uint8_t cond;
  uint16_t reglist;
  uint8_t size;
} dp32g030_insn_t;

/* Cortex-M0 system control space: exceptions, NVIC, SCB, SysTick. */
typedef struct {
  uint64_t pending;      /* bit n = exception n pending */
  uint64_t active;       /* bit n = exception n active */
  uint32_t irq_lines;    /* level of each external IRQ line */
  uint32_t nvic_enabled; /* NVIC ISER/ICER state */
  uint8_t prio[48];      /* raw priority bytes, indexed by exception number */
  uint32_t scr;
  uint32_t st_ctrl;
  uint32_t st_load;
  uint32_t st_val;
} dp32g030_scs_t;

#define DP32G030_NREGIONS 30
#define DP32G030_REGFILE_WORDS (0x800 / 4)
#define DP32G030_UART_FIFO_DEPTH 8

typedef struct {
  uint32_t out; /* DATA register as written */
  uint32_t dir; /* 1 = output */
  uint32_t in;  /* levels driven from outside on input pins */
} dp32g030_gpio_t;

typedef struct {
  uint32_t ctrl, baud, ie, fifo, fc, rxto;
  uint32_t sticky; /* latched IF bits (TXDONE, errors, RXTO, overflow) */
  uint8_t rx[DP32G030_UART_FIFO_DEPTH];
  unsigned rx_head, rx_count;
  uint64_t rxto_deadline; /* 0 = no timeout armed */
} dp32g030_uart_t;

#define DP32G030_SPI_FIFO_DEPTH 8
#define DP32G030_ADC_CHANNELS 15

typedef struct {
  uint32_t cr, ie, sticky;
  uint8_t rx[DP32G030_SPI_FIFO_DEPTH];
  unsigned rx_head, rx_count;
} dp32g030_spi_t;

typedef struct {
  uint32_t cfg, ie, ifl, exttrig, calib_offset, calib_kd;
  uint32_t input[DP32G030_ADC_CHANNELS]; /* set by the host */
  uint32_t result[DP32G030_ADC_CHANNELS];
} dp32g030_adc_t;

typedef struct {
  uint32_t cr, iv, state;
} dp32g030_crc_t;

/* Host callbacks. Any may be NULL. */
typedef struct {
  void *user;
  void (*uart_tx)(void *user, unsigned uart, uint8_t byte);
  /* Called when the effective pin levels of a GPIO port change. */
  void (*gpio_change)(void *user, unsigned port, uint32_t pins, uint32_t dir);
  /* Full-duplex SPI master transfer of one byte; returns the byte read. */
  uint8_t (*spi_xfer)(void *user, unsigned spi, uint8_t tx);
} dp32g030_hooks_t;

typedef struct {
  uint32_t regfile[DP32G030_NREGIONS][DP32G030_REGFILE_WORDS];
  dp32g030_gpio_t gpio[3];
  dp32g030_uart_t uart[3];
  dp32g030_spi_t spi[2];
  dp32g030_adc_t adc;
  dp32g030_crc_t crc;
  dp32g030_hooks_t hooks;
} dp32g030_periph_t;

typedef struct {
  uint32_t r[16];
  uint32_t insn_pc;
  uint32_t xpsr;
  uint32_t primask;
  uint32_t control;
  bool halted;
  bool sleeping;
  uint8_t flash[DP32G030_FLASH_SIZE];
  uint8_t sram[DP32G030_SRAM_SIZE];
  /* State below is not mirrored by the Python test harness. */
  uint64_t cycles;
  uint32_t msp;       /* main SP while PSP is active */
  uint32_t psp;       /* process SP while MSP is active */
  bool using_psp;
  bool handler;       /* handler mode */
  bool locked_up;
  bool event;         /* event register for SEV/WFE */
  bool reset_req;     /* SYSRESETREQ seen */
  dp32g030_scs_t scs;
  dp32g030_periph_t periph;
} dp32g030_cpu_t;

dp32g030_cpu_t *dp32g030_cpu_new(const uint8_t *fw, size_t size);
uint8_t dp32g030_cpu_run(dp32g030_cpu_t **c, size_t size);
void dp32g030_cpu_free(dp32g030_cpu_t *c);
void dp32g030_cpu_reset(dp32g030_cpu_t *c);

/* Host-side peripheral access. */
void dp32g030_gpio_set_input(dp32g030_cpu_t *c, unsigned port, uint32_t mask,
                             uint32_t value);
uint32_t dp32g030_gpio_pins(const dp32g030_cpu_t *c, unsigned port);
void dp32g030_uart_rx(dp32g030_cpu_t *c, unsigned uart, uint8_t byte);
/* Analog level (12-bit) seen on SARADC channel `ch`. */
void dp32g030_adc_set_input(dp32g030_cpu_t *c, unsigned ch, uint32_t value);

/* Drive external interrupt line `irq` (0..31). Level sensitive. */
void dp32g030_irq_set(dp32g030_cpu_t *c, unsigned irq, bool level);
