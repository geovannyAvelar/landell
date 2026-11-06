# Landell

An emulator for the Quansheng UV-K5 handheld radio, written in C. It emulates the DP32G030 microcontroller (ARM Cortex-M0) so raw firmware images can run on a PC.

## Status

Work in progress. The SoC core is complete; the radio side (the BK4819 RF chip) has just started.

Implemented:

- ARMv6-M Thumb decoder and executor. Code runs from flash or SRAM.
- 64 KB flash, 16 KB SRAM, memory bus with alignment and bounds checks.
- Exceptions: stacking and unstacking, `EXC_RETURN`, MSP/PSP, thread and handler mode, HardFault and lockup, `SVC`, `WFI`/`WFE`/`SEV`.
- NVIC (32 IRQs, 2-bit priorities, level-sensitive lines), SCB (`ICSR`, `AIRCR`, `SCR`, `SHPR`, `CPUID`) and SysTick.
- A cycle counter (Cortex-M0 timings) that drives SysTick and the peripherals. `WFI` skips ahead to the next event.
- GPIOA/B/C with host input and output hooks.
- UART0-2 with an 8-byte RX FIFO, level and timeout interrupts, and a TX hook.
- SPI0/1 master with RX FIFO, interrupts and a host transfer hook.
- LCD (ST7565-compatible): not a DP32G030 peripheral either, but unlike BK4819 it rides the real SPI0 peripheral, so `src/DP32G030/lcd.c` taps `dp32g030_spi_write` for SPI0 directly instead of decoding a bit-banged protocol; it also reads GPIOB pin 9 (A0) to tell a command byte from a pixel-data byte. Models the UV-K5 driver's actual command subset (page/column addressing with data auto-increment, display on/off, SEG/COM mirroring, start-line scroll, inverse display, all-pixels-on, software reset, and the 2-byte contrast command with no visual effect) over a full 132x8-page GDDRAM, queryable with `dp32g030_lcd_get_pixel()` (mirrored/scrolled/inverted, as shown) or `dp32g030_lcd_ram()` (raw). The UV-K5's visible 128x64 window is columns 4-131. The command decoding itself is unit-tested against the protocol described in the real driver, but running the real firmware for ~1.6 billion instructions only reaches `ST7565_Init()`'s initial `ST7565_FillScreen(0x00)` (display on, GDDRAM still all zero) — the boot-screen/battery UI that would exercise real pixel content lives further in than this emulator's non-realtime instruction count comfortably reaches in one sitting, so the SEG/COM mirroring and start-line math are unverified against an actual rendered frame.
- SARADC: 16 channels. A conversion copies host-set inputs into each selected channel's `CHn_DATA`, and raises that channel's `CHn_STAT`/`ADC_IF` EOC bit (bit n) as well as the FIFO flags.
- CRC: CCITT, CRC-8, CRC-16 and CRC-32 (checked against the standard test vectors; input/output reversal bits are a best guess).
- TIMER_BASE0-2: two free-running 16-bit up-counters (HIGH/LOW) each with a load/compare value and interrupt.
- TIMER_PLUS0-1: as TIMER_BASE, plus a count mode (counts edges on a host-driven pin) and an input-capture mode (latches the free-running count on a pin edge), and a HALL mode that decodes 3 host-driven sensor lines into edge interrupts.
- PWM_BASE0-1: a 16-bit up-counter wrapping at a configurable period, with 3 channels compared against flip points and overflow/compare interrupts.
- PWM_PLUS0-1: as PWM_BASE, plus complementary (CHn/CHnN) outputs, up/down/center-aligned counting, single-shot or looping, host-driven brake inputs, and output masking.
- RTC: a BCD calendar clock (time, date and weekday), with per-unit (seconds/minutes/hours/day) and half-second interrupts, and a one-shot or recurring alarm.
- IWDT: a 20-bit down-counter watchdog. Expiring once raises an interrupt and reloads; expiring again unfed resets the core.
- WWDT: a windowed 7-bit down-counter watchdog. Feeding outside its window, or the count reaching the reset threshold, resets the core; the window and pre-reset thresholds each raise their own interrupt.
- AES128: ECB, CBC and CTR, encryption, decryption and key derivation, from a from-scratch AES-128 core (checked against the NIST SP 800-38A test vectors).
- IIC0/1: master mode only, with a host transfer hook (start/write/read/stop), like SPI's. Transfers complete instantly.
- BK4819 (the RF transceiver chip): not a DP32G030 peripheral, so not in the memory map. It's bit-banged over 3 GPIOC pins (SCN/SCL/SDA); `src/DP32G030/bk4819.c` decodes that protocol directly off GPIOC writes. Implemented: a generic 128-register file; frequency tracking (REG_38/REG_39, with a host notification hook); an RSSI-threshold squelch (REG_78, REG_67, REG_02 FOUND/LOST); CTCSS/CDCSS detection (REG_07/REG_08/REG_51 vs. REG_02's CTCSS/CDCSS FOUND/LOST); DTMF digit reporting (REG_0B's code, REG_02's DTMF_5TONE_FOUND, gated on REG_24 selecting the decoder for DTMF); VOX (REG_64 amplitude vs. REG_46/REG_79 thresholds with hysteresis, gated on REG_31, REG_02's VOX FOUND/LOST); a frequency scan result (REG_32's scan enable plus the RSSI squelch report the tuned frequency as "found" on REG_0D/REG_0E); and an FSK RX FIFO (REG_5F, REG_02's FSK_RX_FINISHED/FIFO_ALMOST_FULL/FIFO_ALMOST_EMPTY, gated on REG_59's FSK RX enable, cleared via REG_59's RX FIFO clear bit), used by any digital protocol built on the chip's FSK hardware (e.g. MDC1200, Aircopy). All of the above are driven by host "set/push the received signal" calls that mirror how SARADC's input works. There's also a host notification hook for the chip's own GPIO0-6 outputs (REG_33: RX/PA enable, LNA selects, the two status LEDs). No AGC, tone generation, the separate CTCSS/CDCSS auto-scan result (REG_68-6A), CTC2 tail detection, SELCALL or TX encoding yet.
- PORTCON pull-up: `PORTx_PU` makes an input pin nothing has ever driven float high instead of to 0, matching a real pull-up resistor. (Pull-down needs no extra behaviour: that is already the floating default.)
- SYSCON, PMU, flash control, and the rest of PORTCON (function select, input enable, open-drain, wakeup) as plain read/write register files (no behaviour yet).

Not yet implemented: behaviour for the register-file blocks above, clock gating, I2C slave mode, and most of the BK4819 (see above). The reference headers had no timer, PWM, I2C, RTC or watchdog register maps; the register layout for TIMER_BASE/PLUS, PWM_BASE/PLUS, RTC, IWDT, WWDT, AES128 and IIC above comes from the `dp32g030` Rust SVD (itself derived from the UV-K5 firmware's register definitions), not from this project's own headers. The BK4819 register numbers and bit layouts come from the `uv-k5-firmware-custom` driver source (`driver/bk4819.c`/`driver/bk4819-regs.h`), the closest thing to a public register reference for this chip.

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

AES128/IIC model limits:

- AES128 processes a block synchronously on the 4th `AES_DINR` write; there is no pipelining, so `WRERR`/`RDERR` only catch a write with `EN` disabled or a read before `CCF`, not real overlapping-access timing.
- IIC slave mode (`IIC_SCTRL`, `IIC_SADDR`) and features that only matter for it (clock stretching, master-code detection, address matching) are stored but inert.
- IIC timing (`IIC_MSPC`'s `SCL_HI`/`SCL_LOW`/`DAT_HD`, the `MLTO` SCL-low timeout) is not modeled; transfers complete instantly like SPI's.

BK4819 model limits:

- Squelch only looks at the RSSI threshold (`REG_78`). The glitch and noise thresholds (`REG_4D`/`REG_4E`/`REG_4F`) and the undocumented `REG_77` squelch-source selector are stored but don't gate anything yet.
- `REG_02`'s FOUND/LOST bits (squelch, CTCSS, CDCSS) clear when the register is read; the real chip's exact clear condition isn't documented anywhere we found, so this is a guess.
- The module-enable bits in `REG_30` (RX DSP, PLL/VCO, AF DAC, ...) are stored but don't gate the RX path; squelch, RSSI and tone detection behave the same whether or not RX is actually "on".
- CTCSS detection compares `REG_07`'s CTC1 frequency against the host-set received tone with a fixed +/-1 Hz tolerance (to absorb the firmware's own rounding when it computes that register), not any real demodulator bandwidth. CTC2 (the 55 Hz "tail" detector) is not modeled.
- CDCSS detection is an exact bitwise compare (masked to 23 or 24 bits per `REG_51`) between `REG_08`'s two halves and the host-set received code; it doesn't care about code polarity, framing, or bit errors.
- The bit-bang decoder requires SCN and SCL to both be configured as GPIOC outputs before it will decode anything; it does not try to handle a half-configured or mid-reconfiguration bus.
- DTMF digit reporting takes a single character per `dp32g030_bk4819_signal_dtmf()` call; there's no actual 5-tone decoding, debounce, or the chip's own notion of tone duration.
- VOX has no delay (`REG_7A`'s disable-delay field is stored but ignored); the real chip only drops VOX after the signal stays below the disable threshold for that long.
- The frequency scan result only ever reports the single frequency currently tuned in REG_38/REG_39 (as "found" once RSSI squelch opens, "not finished" otherwise); the real firmware itself steps through candidate frequencies one at a time and asks the chip about each one, which is what this models. The separate CDCSS/CTCSS auto-scan result (`BK4819_GetCxCSSScanResult`, REG_68-6A) used for auto-detecting a channel's sub-tone is not modeled.
- The FSK RX FIFO's depth (8 words) and "almost full" level (4 words) are a guess from how much the real firmware drains per interrupt; the real chip's depth isn't documented anywhere we found. There's no sync-word, preamble or CRC checking (REG_5A/REG_5B/REG_5C/REG_5D are stored only) - the host is trusted to only push already-valid decoded words, and framing/CRC is left to firmware.
- Everything other than frequency, RSSI/glitch/noise, RSSI-threshold squelch, CTCSS/CDCSS detection, DTMF digit reporting, VOX, the frequency scan result and the FSK RX FIFO (AGC, tone generation, SELCALL, TX encoding) is unimplemented register storage only.

Model limits:

- Exceptions that cannot run (no valid vector) lock the core up: the CLI prints `halted`.
- The Cortex-M0 has no VTOR here, so the vector table stays at address 0.
- UART timing is approximate: the RX timeout uses `10 * (BAUD + 1)` core clocks per character.
- `SysTick CALIB` reads as 0.
- An input pin nothing has ever driven floats to its `PORTx_PU` pull-up if firmware enabled one, otherwise to 0 - there's no pull-down register check (see above) and no notion of a pin floating to anything else (e.g. a part with no internal pull and no external resistor). Once the host calls `dp32g030_gpio_set_input()` for a pin, that call's value always wins over the pull-up for that pin from then on, like a real external driver overriding a weak internal resistor. On the UV-K5, real firmware's own `BOARD_PORTCON_Init()` enables the pull-ups that the PTT pin and the keypad's column pins need, so real firmware boots without the host having to drive anything.

## Build

Requires meson, ninja and a C11 compiler.

```sh
meson setup build
meson compile -C build
```

## Supported chips

| Chip | Radios | Image |
|------|--------|-------|
| `dp32g030` | UV-K5 V1/V2 | raw binary, vector table first (flash at `0x0`) |
| `py32f071` | UV-K5 V3, UV-K1 | raw *app* image (vector table first, linked at `0x08002800`, e.g. the F4HWN Fusion `.bin`) |

`--soc auto` (the default) picks the chip from the image's reset vector: an address in `0x08000000..0x0801FFFF` selects `py32f071`, anything else `dp32g030`. `--soc dp32g030|py32f071` forces one. `py32f071` refuses an image whose reset vector is outside its app area (`0x08002800` up), such as a bootloader or a full flash dump; `dp32g030` accepts any image up to 64 KB, so forcing it on a PY32 image runs garbage. `landell` does not apply the board defaults below (use `--adc`); `landell-gui` does.

PY32F071 notes: the Cortex-M0+ VTOR is modeled (the bootloader hand-off is emulated: the app boots with its vector table at `0x08002800`, clocks running). Peripherals behave only as far as the F4HWN Fusion firmware needs: GPIO, SPI (the LCD on SPI1, the external PY25Q16 flash on SPI2 with DMA), ADC, DMA (including circular mode and timer-paced transfers), the basic timers TIM6/TIM7, and the BK4819 on PF9/PB8/PB9. The backlight is modeled the way the firmware drives it: TIM7 update events pace a circular DMA channel that writes a duty pattern to `PF8`'s BSRR; the core averages the pin over ~17 ms windows (`dp32g030_backlight_level`) and `landell-gui` dims the LCD glass accordingly, so the boot screen shows unlit and the main screen lights up after the firmware's fade-in. The `dp32g030` backlight (a PWM block) is not modeled: its glass is always drawn lit. The firmware keeps its settings and calibration in that 2 MiB flash (empty at start); `landell-gui` presets a factory-style battery calibration (flash `0x010140`) and the battery ADC (channel 8) so the firmware boots to its normal screen. The UV-K5 V3 key matrix (rows PB6-PB3, columns PB15-PB12, PTT on PB10) is wired to the GUI panel and the PC keyboard. Not modeled yet: the K1's own keys and layout (it shares the V3 matrix here), USB, audio, the I2C devices, and anything the Fusion firmware does not touch on its way to the main screen.

## Usage

```sh
build/landell [-s steps] [-u uart] [--soc chip] firmware.bin
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

## GUI

`landell-gui` shows the LCD in a window, drawn with Skia, while the firmware runs. Below the LCD it draws the radio's key panel. No EEPROM or audio yet.

Needs SDL2 and a Skia build. `scripts/build-skia.sh` clones depot_tools and Skia (branch pinned in the script) and builds a static, raster-only `libskia.a` in `~/skia` (set `SKIA_DIR` to change it). It takes a while and several GB.

```sh
scripts/build-skia.sh
meson setup build-gui -Dgui=enabled -Dskia_dir=$HOME/skia -Dbuildtype=debugoptimized
meson compile -C build-gui
build-gui/gui/landell-gui firmware.bin
```

```sh
landell-gui [--scale N] [--soc CHIP] [-a CH=VAL] [--press KEYS] [--screenshot out.png [--steps N]] firmware.bin
```

- `--soc CHIP`: `auto` (default), `dp32g030` or `py32f071`, as for `landell`.
- `--scale N`: window pixels per LCD pixel, 1-16 (default 4).
- `--adc CH=VAL`: set an ADC channel (repeatable), after the board defaults. The defaults put the battery at a sane level (SARADC channel 9 on `dp32g030`, ADC channel 8 on `py32f071`); without that the firmware shows no UI.
- `--press KEY[,KEY]`: hold keys from the start (`0`-`9`, `menu`, `up`, `down`, `exit`, `star`, `f`, `ptt`, `side1`, `side2`); mostly useful with `--screenshot`.
- `--screenshot out.png`: run headless for `--steps` instructions (default 10,000,000), write one frame as PNG and exit.
- Escape or closing the window quits.

Keys work with the mouse (click and hold a button) or the keyboard:

| PC key | Radio key |
|--------|-----------|
| `0`-`9`, keypad `0`-`9` | digits |
| `Return` | MENU |
| `Up` / `Down` | UP / DOWN |
| `Backspace` | EXIT |
| `*` or `Shift+8` | `*` |
| `#`, `Shift+3` or `F` | `F` (`#`) |
| `Space` | PTT (hold) |
| `F1` / `F2` | side keys 1 / 2 |

The keys go through the real keypad matrix on GPIO: on `dp32g030` rows PA10-PA13, columns PA3-PA6 and PTT on PC5; on `py32f071` rows PB6-PB3, columns PB15-PB12 and PTT on PB10. The firmware debounces over several ms, so hold a key for a moment (2-3 s on the F4HWN Fusion, since the emulator runs slower than the radio). The radio wakes its backlight on the first press after a timeout and ignores that press, as the real UV-K5 does.

Real-time pacing assumes 48 MHz. Use an optimized build; if the emulator cannot keep up it prints a warning and runs slower than real time. Without `-Dgui=enabled` the build is unchanged.

