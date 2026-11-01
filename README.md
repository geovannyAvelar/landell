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
- SYSCON, PMU, PORTCON, flash control, timers, PWM, I2C, DMA, RTC, watchdogs and AES as plain read/write register files (no behaviour yet).

Not yet implemented: behaviour for the register-file blocks above, clock gating, and the BK4819 RF chip. The reference headers had no timer, PWM, I2C, RTC or watchdog register maps.

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
