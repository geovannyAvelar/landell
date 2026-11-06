#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP32G030_FLASH_BASE 0x00000000u
#define DP32G030_FLASH_SIZE (64 * 1024)
#define DP32G030_SRAM_BASE 0x20000000u
#define DP32G030_SRAM_SIZE (16 * 1024)
/* Backing store for flash; the largest supported SoC (PY32F071) is 128 KB. */
#define DP32G030_FLASH_MAX (128 * 1024)

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
  uint32_t driven; /* 1 = dp32g030_gpio_set_input has driven this pin at
                      least once; until then, an input pin floats to
                      PORTCON's pull-up/pull-down configuration, if any */
} dp32g030_gpio_t;

typedef struct {
  uint32_t ctrl, baud, ie, fifo, fc, rxto;
  uint32_t sticky; /* latched IF bits (TXDONE, errors, RXTO, overflow) */
  uint8_t rx[DP32G030_UART_FIFO_DEPTH];
  unsigned rx_head, rx_count;
  uint64_t rxto_deadline; /* 0 = no timeout armed */
} dp32g030_uart_t;

#define DP32G030_SPI_FIFO_DEPTH 8
#define DP32G030_ADC_CHANNELS 16

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

/* TIMER_BASE0-2: two free-running 16-bit up-counters (HIGH/LOW) sharing one
   clock divider. Each reloads to 0 and raises an interrupt on reaching its
   own *_LOAD value (auto-reload; the datasheet does not say explicitly, see
   README). */
typedef struct {
  uint32_t en, div, ie, iflags;
  uint32_t high_load, high_cnt;
  uint32_t low_load, low_cnt;
  uint32_t presc; /* core clocks left until the next prescaled tick */
} dp32g030_timer_base_t;

/* PWM_BASE0-1: one 16-bit up-counter wrapping at PERIOD, three channels each
   comparing the counter against a flip point. */
typedef struct {
  uint32_t en, div, con, period, ie, iflags, cnt;
  uint32_t comp[3];
  uint32_t presc;
} dp32g030_pwm_base_t;

#define DP32G030_TIMERPLUS_MODE_TIMING 0u
#define DP32G030_TIMERPLUS_MODE_COUNT 1u
#define DP32G030_TIMERPLUS_MODE_CAPTURE 2u
#define DP32G030_TIMERPLUS_MODE_HALL 3u /* LOW half only */

/* TIMER_PLUS0-1: like TIMER_BASE, but HIGH/LOW can each run from an external
   pin (count/capture mode) instead of the prescaled clock, and LOW can run
   as a 3-line HALL sensor decoder. */
typedef struct {
  uint32_t en, div, ctr, ie, iflags;
  uint32_t high_load, high_cnt, high_cval;
  uint32_t low_load, low_cnt, low_cval;
  uint32_t hall_val;
  uint32_t ext_level[2]; /* raw host-driven level of timer_in0/timer_in1 */
  uint32_t presc;
} dp32g030_timer_plus_t;

/* PWM_PLUS0-1: PWM_BASE plus complementary (CHn/CHnN) outputs, dead-time,
   center-aligned/up-down counting and brake inputs. PERIOD/COMPn/DTn/
   TRIG_COMP writes take effect immediately: the real shadow-register
   latching on SWLOAD or the cycle autoreload counter is not modeled (see
   README). */
typedef struct {
  uint32_t cfg, gen, clksrc, brake_cfg, mask_lev;
  uint32_t period, comp[3], dt[3], trig_comp, trig_cfg;
  uint32_t ie, iflags, mask_en;
  uint32_t cnt;
  bool counting_down; /* current direction in center-aligned mode */
  uint32_t brake_level; /* raw host-driven level of brake0..2 */
  uint32_t ch_level;    /* bit per channel: 0 CH0 1 CH0N 2 CH1 3 CH1N 4 CH2 5 CH2N */
  uint32_t presc;
} dp32g030_pwm_plus_t;

/* RTC: a BCD calendar clock. The prescaler's "1 second" is modeled as
   PRE_ROUND+1 core clocks (the real chip runs it off a separate slow
   oscillator); the PRE_DECIMAL/PRE_PERIOD fractional adjustment is not
   modeled (see README). */
typedef struct {
  uint32_t cfg, ie, iflags, pre, tr, dr, ar;
  uint8_t sec, min, hour, weekday, date, month, year; /* current time */
  uint32_t cnt;   /* RTC_CNT: seconds elapsed since the last load */
  uint32_t presc; /* core clocks into the current second */
} dp32g030_rtc_t;

/* IWDT: a 20-bit down-counter. The first time it reaches 0 it raises an
   interrupt and reloads; if not fed before it reaches 0 again, it resets
   the core. Modeled as 1 LOAD tick per core clock (see README). */
typedef struct {
  uint32_t load, ctrl, iflags, cnt;
  bool expired_once;
} dp32g030_iwdt_t;

/* WWDT: a 7-bit down-counter with a reset threshold (RST_LOAD) and a
   window interrupt threshold (INT_LOAD) above it. Feeding outside the
   window, or the count reaching RST_LOAD, resets the core. */
typedef struct {
  uint32_t load, ctrl, iflags, value, presc;
} dp32g030_wwdt_t;

/* AES128: ECB/CBC/CTR encryption, decryption and key derivation. One
   128-bit block is processed as soon as the 4th DINR word is written;
   there is no pipelining (see README). */
typedef struct {
  uint32_t cr, sr;
  uint32_t key[4], iv[4];
  uint32_t din[4], dout[4];
  unsigned din_count, dout_count;
} dp32g030_aes_t;

/* IIC0-1: master mode only (slave registers are stored but inert; see
   README). Transfers complete instantly, like SPI. */
typedef struct {
  uint32_t ccfg, ctrans, ie, iflags, sctrl, saddr;
  uint8_t rxdata, txdata;
  bool busy;
} dp32g030_iic_t;

/* ST7565 (or compatible): an external LCD controller, driven over the
   real SPI0 peripheral (not bit-banged) plus GPIOB pin 9 (A0: low for
   a command byte, high for a pixel-data byte). 132 columns x 8 pages
   of 8 vertically-packed pixels each (a real ST7565's full GDDRAM);
   the UV-K5's visible 128x64 window starts at column 4. See README
   for the command subset modeled and what's approximate. */
typedef struct {
  uint8_t ram[8][132];
  uint8_t page, col;    /* current GDDRAM address (page 0-7, col 0-131) */
  uint8_t start_line;   /* vertical scroll offset, 0-63 */
  uint8_t contrast;     /* stored only; no visual effect modeled */
  bool pending_contrast; /* mid-way through the 2-byte SET_EV command */
  bool display_on;
  bool seg_reverse, com_reverse; /* column/row mirroring */
  bool inverse, all_on;
} dp32g030_lcd_t;

/* BK4819: an external RF transceiver chip, bit-banged over 3 GPIOC pins
   (not a DP32G030 peripheral). SCN=pin0 (chip select), SCL=pin1 (clock),
   SDA=pin2 (data); 8-bit address (bit 7 = read) then 16 bits of data,
   MSB first. First slice: a generic register file plus the RX
   measurement path (frequency, RSSI/glitch/noise, RSSI-threshold
   squelch). See README for what's not modeled yet. */
typedef struct {
  uint16_t regs[128];
  bool selected;      /* SCN low: mid-transaction */
  bool prev_scn, prev_scl, prev_sda_in;
  bool addr_phase;    /* decoding the 8-bit address/cmd byte */
  bool reading;       /* decoding/streaming a 16-bit read, vs. a write */
  unsigned bitpos;
  uint8_t addr;
  uint16_t shift;
  bool squelch_open;
  uint32_t ctcss_tone_in;  /* host-provided received CTCSS tone, 0.1 Hz/LSB; 0 = none */
  uint32_t cdcss_code_in;  /* host-provided received CDCSS code (23/24-bit) */
  bool cdcss_present_in;   /* whether a CDCSS signal is being received at all */
  bool ctcss_found, cdcss_found;
  uint16_t cdcss_low, cdcss_high; /* the two REG_08 halves, 12 bits each */
  bool vox_active;
  uint16_t fsk_rx[8];    /* RX FIFO (REG_5F); depth is a guess (see README) */
  unsigned fsk_rx_head, fsk_rx_count;
} dp32g030_bk4819_t;

/* Host callbacks. Any may be NULL. */
typedef struct {
  void *user;
  void (*uart_tx)(void *user, unsigned uart, uint8_t byte);
  /* Called when the effective pin levels of a GPIO port change. */
  void (*gpio_change)(void *user, unsigned port, uint32_t pins, uint32_t dir);
  /* Full-duplex SPI master transfer of one byte; returns the byte read. */
  uint8_t (*spi_xfer)(void *user, unsigned spi, uint8_t tx);
  /* I2C master: start/repeated-start condition. */
  void (*i2c_start)(void *user, unsigned iic);
  /* I2C master: send one byte; returns true if the slave ACKed. */
  bool (*i2c_write)(void *user, unsigned iic, uint8_t byte);
  /* I2C master: receive one byte, sending `ack` (true) or NACK. */
  uint8_t (*i2c_read)(void *user, unsigned iic, bool ack);
  /* I2C master: stop condition. */
  void (*i2c_stop)(void *user, unsigned iic);
  /* BK4819: firmware tuned a new frequency (REG_38/REG_39), raw units. */
  void (*bk4819_freq_change)(void *user, uint32_t freq);
  /* BK4819: its own GPIO0-6 output state changed (REG_33, low byte).
     GPIOn is bit (6-n) of `state`: 0=RX enable, 1=PA enable, 3=UHF LNA,
     4=VHF LNA, 5=red LED, 6=green LED (see BK4819_GPIO_PIN_t in the
     real driver; GPIO2 is unused there). */
  void (*bk4819_gpio_change)(void *user, uint8_t state);
  /* LCD: a command or data byte just changed display state (GDDRAM, the
     address pointer, or an on/off/mirroring/scroll setting). Fired after
     the change; read it back with dp32g030_lcd_get_pixel(). */
  void (*lcd_update)(void *user);
} dp32g030_hooks_t;

typedef struct {
  uint32_t regfile[DP32G030_NREGIONS][DP32G030_REGFILE_WORDS];
  dp32g030_gpio_t gpio[3];
  dp32g030_uart_t uart[3];
  dp32g030_spi_t spi[2];
  dp32g030_adc_t adc;
  dp32g030_crc_t crc;
  dp32g030_timer_base_t timer_base[3];
  dp32g030_timer_plus_t timer_plus[2];
  dp32g030_pwm_base_t pwm_base[2];
  dp32g030_pwm_plus_t pwm_plus[2];
  dp32g030_rtc_t rtc;
  dp32g030_iwdt_t iwdt;
  dp32g030_wwdt_t wwdt;
  dp32g030_aes_t aes;
  dp32g030_iic_t iic[2];
  dp32g030_bk4819_t bk4819;
  dp32g030_lcd_t lcd;
  dp32g030_hooks_t hooks;
} dp32g030_periph_t;

#define PY32_NREGIONS 40
#define PY32_REGION_SIZE 0x400u
#define PY32_REGFILE_WORDS (PY32_REGION_SIZE / 4)

#define PY32_NGPIO 4 /* ports A, B, C, F */

typedef struct {
  uint32_t moder, otyper, ospeedr, pupdr, odr, lckr, afr[2];
  uint32_t in;     /* levels driven from outside on non-output pins */
  uint32_t driven; /* 1 = dp32g030_gpio_set_input has driven this pin */
} dp32g030_py32_gpio_t;

typedef struct {
  uint32_t cr1, cr2;
  uint8_t rx;
  bool rxne;
} dp32g030_py32_spi_t;

#define PY32_ADC_CHANNELS 20

typedef struct {
  uint32_t sr, cr1, cr2, smpr[3], sqr[3], dr;
  bool started;                      /* a conversion has been started */
  uint16_t input[PY32_ADC_CHANNELS]; /* levels set by the host */
} dp32g030_py32_adc_t;

#define PY32_DMA_CHANNELS 7

typedef struct {
  uint32_t isr;
  uint32_t ccr[PY32_DMA_CHANNELS], cndtr[PY32_DMA_CHANNELS];
  uint32_t cpar[PY32_DMA_CHANNELS], cmar[PY32_DMA_CHANNELS];
  uint32_t cur_p[PY32_DMA_CHANNELS], cur_m[PY32_DMA_CHANNELS]; /* running */
  bool busy; /* guards against re-entry from DMA-initiated bus accesses */
} dp32g030_py32_dma_t;

#define DP32G030_EEPROM_SIZE 8192 /* 24C64 */

/* The board's I2C EEPROM (24C64 at address 0xA0) and the state of its
   bit-banged bus decoder (see i2c_eeprom.c). Contents survive a reset. */
typedef struct {
  uint8_t mem[DP32G030_EEPROM_SIZE];
  bool prev_scl, prev_sda; /* bus levels seen at the last event */
  bool slave_low;          /* the EEPROM is pulling SDA low */
  bool master_ack;         /* while sending: the master acknowledged */
  uint8_t state;
  uint8_t bitcount;
  uint8_t shift;
  uint8_t out;
  uint16_t ptr;
  bool reading;
} dp32g030_eeprom_t;

/* PY32F071 peripheral state (see py32f071.c). */
typedef struct {
  uint32_t regfile[PY32_NREGIONS][PY32_REGFILE_WORDS];
  dp32g030_py32_gpio_t gpio[PY32_NGPIO];
  dp32g030_py32_spi_t spi[2]; /* SPI1, SPI2 */
  dp32g030_py32_adc_t adc;
  dp32g030_py32_dma_t dma;
} dp32g030_py32_t;

typedef struct {
  uint32_t r[16];
  uint32_t insn_pc;
  uint32_t xpsr;
  uint32_t primask;
  uint32_t control;
  bool halted;
  bool sleeping;
  uint8_t flash[DP32G030_FLASH_MAX];
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
  const struct dp32g030_soc *soc; /* chip this cpu models */
  dp32g030_scs_t scs;
  dp32g030_periph_t periph;
  /* Fields below are not mirrored by the Python test harness. */
  uint32_t vtor;      /* vector table offset (chips with a VTOR only) */
  uint32_t bus_fault_addr; /* address of the last failed bus access */
  bool bus_fault_write;    /* ... and whether it was a write */
  dp32g030_py32_t py32;
  dp32g030_eeprom_t eeprom;
} dp32g030_cpu_t;

typedef struct dp32g030_soc dp32g030_soc_t;

/* Host access to the board's EEPROM contents (clamped to its size). */
void dp32g030_eeprom_set(dp32g030_cpu_t *c, uint32_t offset, const uint8_t *data,
                         uint32_t len);
void dp32g030_eeprom_get(const dp32g030_cpu_t *c, uint32_t offset, uint8_t *data,
                         uint32_t len);

/* Looks a chip up by name ("dp32g030"); NULL if unknown. */
const dp32g030_soc_t *dp32g030_soc_by_name(const char *name);
/* Loads `fw` at the chip's image base. NULL if it is too small (< 8 bytes) or
   does not fit in the chip's flash. */
dp32g030_cpu_t *dp32g030_cpu_new_soc(const dp32g030_soc_t *soc,
                                     const uint8_t *fw, size_t size);
/* Same, with the chip chosen by dp32g030_soc_by_name's default (dp32g030). */
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

/* Drive TIMER_PLUS `unit`'s timer_in0/timer_in1 pin (`which` 0 or 1). Used
   by count and input-capture modes. */
void dp32g030_timer_plus_set_ext(dp32g030_cpu_t *c, unsigned unit,
                                 unsigned which, bool level);
/* Drive TIMER_PLUS `unit`'s 3 HALL sensor lines (bit n = HALLn). */
void dp32g030_timer_plus_set_hall(dp32g030_cpu_t *c, unsigned unit,
                                  uint32_t mask);
/* Drive PWM_PLUS `unit`'s 3 brake lines (bit n = braken), raw level. */
void dp32g030_pwm_plus_set_brake(dp32g030_cpu_t *c, unsigned unit,
                                 uint32_t mask);

/* BK4819: set the simulated received signal. `rssi` and the open/close
   thresholds in REG_78 share the same raw 9-bit, 0.5 dB/step scale;
   `glitch`/`noise` are stored but do not yet affect squelch (see
   README). Recomputes squelch and fires REG_02 FOUND/LOST on change. */
void dp32g030_bk4819_set_signal(dp32g030_cpu_t *c, uint16_t rssi,
                                uint8_t glitch, uint8_t noise);
/* The frequency last written to REG_38/REG_39 (raw units, firmware's
   own scale - typically 10 Hz/LSB). */
uint32_t dp32g030_bk4819_frequency(const dp32g030_cpu_t *c);
/* Whether the RSSI-threshold squelch is currently open. */
bool dp32g030_bk4819_squelch_open(const dp32g030_cpu_t *c);

/* BK4819: set the simulated received CTCSS tone (0.1 Hz/LSB; 0 = no
   tone present). Compared against REG_07's CTC1 frequency with a
   +/-1 Hz tolerance; mismatched/no tone raises CTCSS_LOST, a match
   raises CTCSS_FOUND (REG_02). */
void dp32g030_bk4819_set_ctcss_tone(dp32g030_cpu_t *c, uint32_t tenth_hz);
/* BK4819: set the simulated received CDCSS code and whether a CDCSS
   signal is present at all. Compared against the 23/24-bit code
   assembled from REG_08; CDCSS_FOUND/LOST raised on REG_02 as above. */
void dp32g030_bk4819_set_cdcss_code(dp32g030_cpu_t *c, uint32_t code,
                                    bool present);
/* BK4819: signal that `digit` ('0'-'9', 'A'-'D', '*' or '#') was just
   decoded by the 5-tone DTMF detector. Only takes effect if REG_24 has
   the decoder enabled and selected for DTMF (not SELCALL); returns
   whether it did. Sets REG_0B's code and DTMF_5TONE_FOUND (REG_02). */
bool dp32g030_bk4819_signal_dtmf(dp32g030_cpu_t *c, char digit);
/* BK4819: set the simulated mic/voice amplitude (REG_64, 15 bits).
   Compared against REG_46/REG_79's VOX enable/disable thresholds with
   hysteresis (no delay, unlike the real chip's REG_7A delay) when VOX
   is enabled (REG_31 bit 2); raises VOX_FOUND/LOST on REG_02. */
void dp32g030_bk4819_set_mic_level(dp32g030_cpu_t *c, uint16_t amplitude);

/* BK4819: push one 16-bit word of already-decoded FSK data (REG_5F's
   RX FIFO), as if the chip had received it over the air. No-op (and
   returns false) unless REG_59 has FSK RX enabled or the FIFO is
   already full. Raises FSK_FIFO_ALMOST_FULL (REG_02) once the FIFO
   holds 4 or more words, matching how much the real firmware drains
   on that interrupt. */
bool dp32g030_bk4819_fsk_rx_push(dp32g030_cpu_t *c, uint16_t word);
/* BK4819: signal that the current FSK packet is complete, raising
   FSK_RX_FINISHED (REG_02). Does not touch the FIFO itself. */
void dp32g030_bk4819_fsk_rx_finish(dp32g030_cpu_t *c);

/* LCD: whether the display is on and this is currently showing a lit
   pixel at physical column `x` (0-131) / row `y` (0-63), after
   applying SEG/COM mirroring, the vertical scroll (start line),
   inverse display and the all-pixels-on override. The UV-K5's visible
   128x64 window is x in [4, 131]. */
bool dp32g030_lcd_get_pixel(const dp32g030_cpu_t *c, unsigned x, unsigned y);
/* LCD: the raw GDDRAM byte at (page 0-7, col 0-131); bit n is row
   page*8+n. No mirroring, scrolling or on/off applied. */
uint8_t dp32g030_lcd_ram(const dp32g030_cpu_t *c, unsigned page, unsigned col);
/* LCD: whether the display is currently on (DISPLAY_ON_OFF). */
bool dp32g030_lcd_is_on(const dp32g030_cpu_t *c);
