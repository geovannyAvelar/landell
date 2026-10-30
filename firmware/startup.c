#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

uint32_t main(void);
void reset_handler(void);
void default_handler(void);

typedef void (*isr_t)(void);

__attribute__((section(".vectors"), used)) const isr_t vectors[48] = {
    [0] = (isr_t)&_estack,
    [1] = reset_handler,
    [2 ... 47] = default_handler,
};

void reset_handler(void) {
  const uint32_t *src = &_sidata;
  uint32_t *dst = &_sdata;

  while (dst < &_edata)
    *dst++ = *src++;
  for (dst = &_sbss; dst < &_ebss;)
    *dst++ = 0;

  uint32_t result = main();
  __asm volatile("mov r0, %0\n\tbkpt #0" : : "r"(result) : "r0");
  for (;;)
    ;
}

void default_handler(void) {
  for (;;)
    __asm volatile("bkpt #1");
}
