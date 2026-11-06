#include "periph.h"

/* PY32F071 ADC, STM32F1-style. CR2: ADON bit 0, CONT bit 1, CAL bit 2
   (self-clearing: calibration is instant), SWSTART bit 22 (self-clearing).
   Only the first conversion of the sequence is modeled (channel in SQR3[4:0]);
   it completes instantly with the level the host set for that channel. In
   continuous mode the result follows the host level live and EOC stays set. */

#define SR_EOC (1u << 1)
#define CR2_ADON (1u << 0)
#define CR2_CONT (1u << 1)
#define CR2_CAL (1u << 2)
#define CR2_SWSTART (1u << 22)

void py32_adc_reset(dp32g030_cpu_t *c) {
  uint16_t keep[PY32_ADC_CHANNELS];

  for (unsigned i = 0; i < PY32_ADC_CHANNELS; i++)
    keep[i] = c->py32.adc.input[i];
  c->py32.adc = (dp32g030_py32_adc_t){0};
  for (unsigned i = 0; i < PY32_ADC_CHANNELS; i++)
    c->py32.adc.input[i] = keep[i];
}

void py32_adc_set_input(dp32g030_cpu_t *c, unsigned ch, uint32_t value) {
  if (ch < PY32_ADC_CHANNELS)
    c->py32.adc.input[ch] = (uint16_t)(value & 0xFFFu);
}

static uint32_t *adc_reg(dp32g030_py32_adc_t *a, uint32_t off) {
  switch (off) {
  case 0x00: return &a->sr;
  case 0x04: return &a->cr1;
  case 0x08: return &a->cr2;
  case 0x0C: return &a->smpr[0];
  case 0x10: return &a->smpr[1];
  case 0x14: return &a->smpr[2];
  case 0x30: return &a->sqr[0];
  case 0x34: return &a->sqr[1];
  case 0x38: return &a->sqr[2];
  default: return NULL;
  }
}

static uint32_t adc_level(const dp32g030_py32_adc_t *a) {
  return a->input[a->sqr[2] & 0x1Fu];
}

bool py32_adc_read(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                   uint32_t *out) {
  dp32g030_py32_adc_t *a = &c->py32.adc;
  uint32_t v;

  if ((off & ~3u) == 0x50) {
    if ((a->cr2 & CR2_CONT) && a->started) {
      a->dr = adc_level(a); /* streaming: EOC stays set */
    } else {
      a->sr &= ~SR_EOC;
    }
    v = a->dr;
  } else {
    uint32_t *r = adc_reg(a, off & ~3u);

    if (!r)
      return false;
    v = *r;
  }
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);
  *out = (v >> shift) & mask;
  return true;
}

bool py32_adc_write(dp32g030_cpu_t *c, uint32_t off, unsigned size,
                    uint32_t val) {
  dp32g030_py32_adc_t *a = &c->py32.adc;
  uint32_t *r = adc_reg(a, off & ~3u);
  unsigned shift = 8 * (off & 3u);
  uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u);

  if (!r)
    return false;
  uint32_t v = (*r & ~(mask << shift)) | ((val & mask) << shift);

  if (r == &a->sr) {
    a->sr &= v; /* flags are cleared by writing 0 */
    return true;
  }
  if (r == &a->cr2) {
    v &= ~CR2_CAL; /* calibration finishes at once */
    if ((v & CR2_SWSTART) && (v & CR2_ADON)) {
      a->dr = adc_level(a);
      a->sr |= SR_EOC;
      a->started = true;
    }
    v &= ~CR2_SWSTART;
  }
  *r = v;
  return true;
}
