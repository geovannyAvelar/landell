# Landell

An emulator for the Quansheng UV-K5 handheld radio, written in C. It emulates the DP32G030 microcontroller (ARM Cortex-M0) so raw firmware images can run on a PC.

## Status

Work in progress. The SoC core is complete; the radio side is not started.

Implemented:

- ARMv6-M Thumb decoder and executor. Code runs from flash or SRAM.
- 64 KB flash, 16 KB SRAM, memory bus with alignment and bounds checks.
- Exceptions: stacking and unstacking, `EXC_RETURN`, MSP/PSP, thread and handler mode, HardFault and lockup, `SVC`, `WFI`/`WFE`/`SEV`.
- NVIC (32 IRQs, 2-bit priorities, level-sensitive lines), SCB (`ICSR`, `AIRCR`, `SCR`, `SHPR`, `CPUID`) and SysTick.
- A cycle counter (Cortex-M0 timings) that drives SysTick and the peripherals. `WFI` skips ahead to the next event.
- GPIOA/B/C with host input and output hooks.
- UART0-2 with an 8-byte RX FIFO, level and timeout interrupts, and a TX hook.
- SPI0/1 master with RX FIFO, interrupts and a host transfer hook (used for the LCD and similar).
- SARADC: a conversion copies host-set inputs into `CHn` and raises the FIFO flags.
- CRC: CCITT, CRC-8, CRC-16 and CRC-32 (checked against the standard test vectors; input/output reversal bits are a best guess).
- TIMER_BASE0-2: two free-running 16-bit up-counters (HIGH/LOW) each with a load/compare value and interrupt.
- TIMER_PLUS0-1: as TIMER_BASE, plus a count mode (counts edges on a host-driven pin) and an input-capture mode (latches the free-running count on a pin edge), and a HALL mode that decodes 3 host-driven sensor lines into edge interrupts.
- PWM_BASE0-1: a 16-bit up-counter wrapping at a configurable period, with 3 channels compared against flip points and overflow/compare interrupts.
- PWM_PLUS0-1: as PWM_BASE, plus complementary (CHn/CHnN) outputs, up/down/center-aligned counting, single-shot or looping, host-driven brake inputs, and output masking.
- RTC: a BCD calendar clock (time, date and weekday), with per-unit (seconds/minutes/hours/day) and half-second interrupts, and a one-shot or recurring alarm.
- IWDT: a 20-bit down-counter watchdog. Expiring once raises an interrupt and reloads; expiring again unfed resets the core.
- WWDT: a windowed 7-bit down-counter watchdog. Feeding outside its window, or the count reaching the reset threshold, resets the core; the window and pre-reset thresholds each raise their own interrupt.
- SYSCON, PMU, PORTCON, flash control, I2C and AES as plain read/write register files (no behaviour yet).

Not yet implemented: behaviour for the register-file blocks above, clock gating, and the BK4819 RF chip. The reference headers had no timer, PWM, I2C, RTC or watchdog register maps; the register layout for TIMER_BASE/PLUS, PWM_BASE/PLUS, RTC, IWDT and WWDT above comes from the `dp32g030` Rust SVD (itself derived from the UV-K5 firmware's register definitions), not from this project's own headers.

TIMER_BASE/PLUS and PWM_BASE/PLUS model limits:

- A TIMER_BASE/PLUS counter is assumed to reload to 0 on reaching its load value; the datasheet says only that a "status signal is generated".
- PWM_PLUS's PERIOD/CHn_COMP/CHn_DT/TRIG_COMP writes take effect immediately. The real hardware's shadow-register latching (on SWLOAD or the cycle autoreload counter) is not modeled, so `AUTORELOAD_IE`/`AUTORELOAD_IF` never fire.
- PWM_PLUS dead-time (`CHn_DT`) and brake digital filtering (`BRAKE_FILTER`) are stored but have no timing effect: channel outputs switch instantly and brake overrides apply as soon as the line is asserted.
- PWM_PLUS's internal trigger point (`TRIG_COMP`/`TRIG_CFG`) only raises its own interrupt flags; nothing else in the emulator consumes it (e.g. to trigger an SARADC conversion).
- TIMER_PLUS's DMA-read-of-capture-value bits are stored but have no effect (the DMA peripheral itself has no behaviour yet).

RTC/IWDT/WWDT model limits:

- RTC's "1 second" is `RTC_PRE`'s `PRE_ROUND + 1` core clocks; the real chip runs it off a separate slow oscillator, and the `PRE_DECIMAL`/`PRE_PERIOD` fractional adjustment is not modeled.
- RTC's leap-year rule is a plain `year % 4` (the 2-digit BCD year has no century to apply the usual exceptions to).
- RTC_VALID.CUR_VALID simply follows RTC_EN; the real chip's exact validity conditions after enabling or loading are unclear from the register description alone.
- IWDT's 20-bit `IWDT_LOAD` count is modeled as core clocks, not the separate watchdog oscillator the real chip likely uses.

Model limits:

- Exceptions that cannot run (no valid vector) lock the core up: the CLI prints `halted`.
- The Cortex-M0 has no VTOR here, so the vector table stays at address 0.
- UART timing is approximate: the RX timeout uses `10 * (BAUD + 1)` core clocks per character.
- `SysTick CALIB` reads as 0.

## Build

Requires meson, ninja and a C11 compiler.

```sh
meson setup build
meson compile -C build
```

## Usage

```sh
build/landell [-s steps] [-u uart] firmware.bin
```

The image must be a raw binary starting with the Cortex-M vector table (initial SP, then reset vector). It runs until `bkpt`, a fault, or the step limit (default 10,000,000). The final register state is printed.

Exit status: `0` ok, `1` usage or I/O error, `2` bus fault, `3` undefined instruction.

## Tests

Requires python3 and pytest.

```sh
meson test -C build --print-errorlogs
```

The tests load the static library through `ctypes` and cover the decoder, executor, bus and CLI.

## Example firmware

`firmware/` has a small bare-metal program (Fibonacci, bubble sort, sum of squares). It needs `arm-none-eabi-gcc`.

```sh
make -C firmware
build/landell firmware/build/firmware.bin   # expect r0 = 011d0262
```

## Layout

```
src/main.c, src/file.c   CLI and file loading
src/DP32G030/            SoC: decode, exec, bus, exceptions, SCS, timing, peripherals
tests/                   pytest suite, run through meson
firmware/                example firmware
```
