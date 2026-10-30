# Landell

An emulator for the Quansheng UV-K5 handheld radio, written in C. It emulates the DP32G030 microcontroller (ARM Cortex-M0) so raw firmware images can run on a PC.

## Status

Early work in progress.

- Implemented: ARMv6-M Thumb decoder and executor, 64 KB flash, 16 KB SRAM, memory bus with alignment and bounds checks.
- Not yet implemented: peripherals (SYSCON, PORTCON, GPIO, UART, SPI, timers), NVIC and exceptions, SysTick. Any access outside flash and SRAM currently faults.

## Build

Requires meson, ninja and a C11 compiler.

```sh
meson setup build
meson compile -C build
```

## Usage

```sh
build/landell [-s steps] firmware.bin
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
src/DP32G030/            CPU core: decode, exec, bus, exceptions (stub)
tests/                   pytest suite, run through meson
firmware/                example firmware
```
