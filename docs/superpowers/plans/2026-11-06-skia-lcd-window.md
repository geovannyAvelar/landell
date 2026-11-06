# Skia LCD Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `landell-gui` opens a window that shows the emulated UV-K5 LCD, drawn with Skia, with the firmware running in real time.

**Architecture:** A C++17 binary links the existing C `libdp32g030`. `LcdView` draws the LCD into an `SkCanvas` using only `dp32g030_lcd_get_pixel`/`dp32g030_lcd_is_on`. `Pacer` turns wall time into an emulator step budget. A single-thread SDL2 loop runs the emulator, draws into a Skia raster surface and uploads the pixmap to an SDL texture.

**Tech Stack:** C11 core (unchanged), C++17, Skia (raster, built from source, no GL, no fonts), SDL2, Meson.

**Spec:** `docs/superpowers/specs/2026-11-06-skia-lcd-window-design.md`

## Global Constraints

- Core (`src/`) and the `landell` CLI stay C11 and do not depend on Skia, SDL or C++.
- GUI is optional: Meson option `gui` (feature, default `disabled`), `skia_dir` (string). Default `meson setup build && meson test -C build` must stay green with no Skia installed.
- LCD logical size 128x64; default window scale 6, `--scale N` accepted for N in 1..16.
- Emulated clock 48 MHz; per-frame cycle budget capped.
- Battery ADC channel 9 preset to 2000; `--adc CH=VAL` (CH 0-15, VAL 0-0xFFF) overrides.
- Skia built with GL off and fonts off; no text drawn in v1.
- Skia pinned to one milestone branch recorded in `scripts/build-skia.sh`.
- Out of scope: keyboard/PTT, EEPROM, audio, BK4819 UI.

## Review Focus

- Display off (`dp32g030_lcd_is_on` false) draws a flat off color, never stale pixels. Pinned in Task 2.
- `--scale 0`, `17`, or non-numeric exits 1 with a message, no window. Pinned in Task 4.
- Firmware file missing or invalid exits 1 with a message, no window. Pinned in Task 4.
- CPU halted or faulted: window stays open on the last frame, title shows state. Pinned in Task 4.
- Stall (laptop sleep, window drag): budget capped, emulator does not run a huge burst. Pinned in Task 3.

---

### Task 1: Skia build script, Meson wiring, GUI stub

**Files:**
- Create: `scripts/build-skia.sh`, `meson_options.txt`, `gui/meson.build`, `gui/main.cpp`
- Modify: `meson.build`

**Interfaces:**
- Produces: Meson target `landell_gui` (executable `landell-gui`), variable `gui_deps` (SDL2 + Skia dependency list) in `gui/meson.build`, used by later tasks.

- [ ] **Step 1: Write `scripts/build-skia.sh`**: bash, `set -euo pipefail`, `SKIA_DIR` defaults to `$HOME/skia`, pinned `SKIA_BRANCH` (current stable `chrome/mNNN`, recorded as a variable at the top). Steps: clone depot_tools if missing, clone skia at the branch, `python3 tools/git-sync-deps`, `bin/gn gen out/Static --args='is_official_build=true is_component_build=false skia_use_gl=false skia_use_system_expat=false skia_use_system_freetype2=false skia_use_system_harfbuzz=false skia_use_system_icu=false skia_use_system_libjpeg_turbo=false skia_use_system_libpng=false skia_use_system_libwebp=false skia_use_system_zlib=false skia_enable_fontmgr_empty=true skia_use_freetype=false skia_use_fontconfig=false skia_use_harfbuzz=false skia_enable_pdf=false skia_enable_skshaper=false'`, then `ninja -C out/Static skia`. Prints final `libskia.a` path. (If a flag name is rejected by the pinned milestone, `gn args --list` shows the current name.)
- [ ] **Step 2: Run it** (long): `bash scripts/build-skia.sh`. Expected: `$SKIA_DIR/out/Static/libskia.a` exists.
- [ ] **Step 3: Write `meson_options.txt`**: `option('gui', type : 'feature', value : 'disabled')`, `option('skia_dir', type : 'string', value : '')`.
- [ ] **Step 4: Modify `meson.build`**: add `'cpp'` via `add_languages('cpp', required : get_option('gui'))` guarded by `get_option('gui').enabled()`; `default_options` gains `cpp_std=c++17`. Add `subdir('gui')` guarded by `get_option('gui').enabled()`. Must not change the C-only configure path.
- [ ] **Step 5: Write `gui/meson.build`**: `sdl2 = dependency('sdl2')`; `skia_dir = get_option('skia_dir')` (error if empty); `skia_inc = include_directories(skia_dir)`; Skia lib via `cpp.find_library('skia', dirs : skia_dir / 'out/Static')` plus `-lpthread -ldl`; `gui_deps = [dp32g030_dep, sdl2, skia_dep]`; `executable('landell-gui', 'main.cpp', dependencies : gui_deps, override_options : ['werror=false'])`.
- [ ] **Step 6: Write `gui/main.cpp` stub**: create `SkSurfaces::Raster(SkImageInfo::MakeN32Premul(8, 8))`, clear it, print `skia ok`.
- [ ] **Step 7: Verify**: `meson setup build-gui -Dgui=enabled -Dskia_dir=$HOME/skia && meson compile -C build-gui && build-gui/gui/landell-gui`. Expected output `skia ok`. Then `meson setup build-nogui --wipe`-free check: `meson setup /tmp/nogui && meson compile -C /tmp/nogui` passes with no Skia args.
- [ ] **Step 8: Commit** `feat(gui): add optional Skia build wiring and stub`.

### Task 2: LcdView (pure drawing)

**Files:**
- Create: `gui/lcd_view.h`, `gui/lcd_view.cpp`, `gui/test_lcd_view.cpp`
- Modify: `gui/meson.build` (add `lcd_view.cpp` to a `lcd_view_lib` static library; add `test('lcd_view', executable('test_lcd_view', ...))` only when gui enabled)

**Interfaces:**
- Produces:
  - `struct LcdPalette { SkColor off, background, pixel_on, pixel_off; }` and `const LcdPalette& lcd_palette()`.
  - `void lcd_view_draw(const dp32g030_cpu_t* cpu, SkCanvas* canvas, int scale)` draws a 128*scale x 64*scale image.
  - `SkColor` of pixel (x,y) is sampled at `(x*scale + scale/2, y*scale + scale/2)` for the test.
- Consumes: `dp32g030_lcd_get_pixel`, `dp32g030_lcd_is_on` from `DP32G030.h` (wrapped in `extern "C"` in the C++ includes if the header has no guards).

- [ ] **Step 1: Write failing tests** in `gui/test_lcd_view.cpp` (plain `main`, `assert`-style helper that returns nonzero on failure). Build a cpu with `dp32g030_cpu_new` from an 8-byte image (SP, reset vector). Drive the LCD through its public command path (call `dp32g030_lcd_spi_byte` via an `extern "C"` declaration, or set `cpu->periph.lcd` fields directly: `display_on`, `ram[0][0] = 0x01`). Draw with scale 6 into an `SkSurfaces::Raster` and read pixels with `readPixels`. Tests: `display_on_pixel_on_color` (pixel (0,0) center == `pixel_on`), `display_on_pixel_off_color` (pixel (1,0) center == `pixel_off`), `gap_between_pixels_is_background` (column `x*6+5` of pixel 0 == `background`), `display_off_is_flat` (display_on=false with ram set: every sampled pixel == `off`), `scale_one_has_no_gap` (scale 1: image is 128x64, pixel (0,0) == `pixel_on`).
- [ ] **Step 2: Run to verify fail**: `meson test -C build-gui lcd_view`. Expected: link error, `lcd_view_draw` undefined.
- [ ] **Step 3: Implement** `lcd_view_draw`: fill `off` if `!dp32g030_lcd_is_on`, else fill `background` then one `SkRect` per pixel, `pixel_on` or `pixel_off`, rect size `scale - gap` where `gap = scale >= 3 ? 1 : 0`. Palette constants fixed in `lcd_view.cpp`.
- [ ] **Step 4: Run to verify pass**: `meson test -C build-gui lcd_view`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(gui): add LcdView Skia drawing`.

### Task 3: Pacer (wall time to step budget)

**Files:**
- Create: `gui/pacer.h`, `gui/pacer.cpp`, `gui/test_pacer.cpp`
- Modify: `gui/meson.build`

**Interfaces:**
- Produces: `class Pacer { public: explicit Pacer(double cpu_hz = 48e6, double max_frame_s = 0.1); size_t steps_for(double elapsed_s, uint64_t cycles_delta, size_t steps_delta); }`. `steps_for` returns the step count to run for `elapsed_s` of wall time. It keeps a running cycles-per-step average updated from the last `(cycles_delta, steps_delta)` (initial guess 1.5). Returns at least 1, at most `max_frame_s * cpu_hz / cycles_per_step`.

- [ ] **Step 1: Write failing tests** in `gui/test_pacer.cpp`: `normal_frame` (elapsed 1/60 s, cps 1.0 -> 800000 steps), `stall_is_capped` (elapsed 5 s -> result equals the 0.1 s cap, 4800000 at cps 1.0), `zero_elapsed_returns_one`, `cps_adapts` (after feeding `(cycles_delta=2000, steps_delta=1000)` repeatedly, budget for 1/60 s approaches 400000), `zero_steps_delta_ignored` (feeding `steps_delta=0` leaves cps unchanged, no division by zero).
- [ ] **Step 2: Run to verify fail**: `meson test -C build-gui pacer`. Expected: link error.
- [ ] **Step 3: Implement** `Pacer` with an exponential moving average (weight 0.2) for cycles-per-step.
- [ ] **Step 4: Run to verify pass**: `meson test -C build-gui pacer`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(gui): add real-time Pacer`.

### Task 4: Window main loop, options, screenshot mode

**Files:**
- Modify: `gui/main.cpp`, `gui/meson.build`
- Create: `gui/test_screenshot.py` (run by Meson as a test when gui is enabled and `python3` has no extra deps)

**Interfaces:**
- Consumes: `lcd_view_draw`, `Pacer::steps_for`, `read_file` from `src/file.h`, `dp32g030_cpu_new/run/free`, `dp32g030_adc_set_input`, `cpu->periph.hooks.lcd_update`.
- Produces: CLI `landell-gui [--scale N] [--adc CH=VAL] [--screenshot out.png [--steps N]] firmware.bin`. Exit codes match the CLI: 0 ok, 1 usage/IO error, 2 fault, 3 undefined instruction (screenshot mode only).

- [ ] **Step 1: Write failing test** `gui/test_screenshot.py`: runs `landell-gui` (path from `LANDELL_GUI` env) in three cases: (a) `--scale 0 fw.bin` -> exit 1, stderr contains `scale`; (b) `--scale 17 fw.bin` -> exit 1; (c) `missing.bin` -> exit 1 and stderr contains `cannot read`. Add a fourth: `--screenshot out.png --steps 1000 --scale 2 <valid 8-byte image>` -> exit 0, PNG file exists and is 256x128 (read IHDR bytes 16..24). Image bytes: SP `0x20004000`, reset `0x09`, followed by a `bkpt`-style halfword so the CPU halts quickly. Register the test in `gui/meson.build` with env `LANDELL_GUI`.
- [ ] **Step 2: Run to verify fail**: `meson test -C build-gui screenshot`. Expected: FAIL (stub prints `skia ok`).
- [ ] **Step 3: Implement `main.cpp`**:
  - Parse args; `--scale` outside 1..16 or non-numeric -> `scale` message, exit 1. Same `--adc` rules as `src/main.c`. Unknown option -> usage, exit 1.
  - Load firmware with `read_file`; failure -> `cannot read '<path>'`, exit 1. `dp32g030_cpu_new` null -> `not a valid image`, exit 1.
  - Apply ADC: channel 9 = 2000 first, then user `--adc` overrides.
  - `lcd_update` hook sets a `bool dirty` (user pointer = struct holder).
  - `--screenshot`: run `--steps` (default 10000000) with one `dp32g030_cpu_run`, draw one frame to a raster surface, encode with `SkPngEncoder`, write the file, exit with the status mapping above.
  - Window mode: `SDL_Init(VIDEO)`, window `128*scale x 64*scale`, renderer + streaming `SDL_PIXELFORMAT_BGRA8888` texture. Loop: poll events (`SDL_QUIT` or Escape exits); `Pacer::steps_for`; if not halted run `dp32g030_cpu_run(&cpu, steps)` (note it takes `dp32g030_cpu_t**`); on `cpu->halted` or nonzero status keep looping without running and set the title to `landell - halted` / `landell - fault at pc=%08x` / `landell - undefined insn`; if `dirty` draw and `SDL_UpdateTexture` from the surface pixmap; present; `SDL_Delay(1)` to avoid a busy spin. Print `warning: cannot keep up` once to stderr if the cap was hit three frames in a row.
- [ ] **Step 4: Run to verify pass**: `meson test -C build-gui`. Expected: all GUI tests PASS; `meson test -C /tmp/nogui` still passes.
- [ ] **Step 5: Manual verification** (display present): build the GUI with `-Dbuildtype=release`, then run `build-gui/gui/landell-gui firmware/egzumer_v0.22.bin`. Expected: window shows the egzumer VFO screen within ~10 s. Close with Escape. Record the observed result in the commit message.
- [ ] **Step 6: Commit** `feat(gui): add SDL2+Skia live LCD window`.

### Task 5: Docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add a "GUI" section** to `README.md`: how to run `scripts/build-skia.sh`, the Meson flags `-Dgui=enabled -Dskia_dir=...`, release build advice, usage line for `landell-gui`, and the note that only the display exists (no keys yet).
- [ ] **Step 2: Commit** `docs: document landell-gui`.

---

## Self-Review

- Spec coverage: window+Skia raster (T1,T4), `LcdView` (T2), `Pacer`+cap (T3), ADC preset and override (T4), Meson option off by default (T1), build script (T1), `--screenshot` (T4), docs (T5). Spec's golden-PNG comparison is replaced by pixel assertions in T2 and a size check in T4: a byte-exact golden would be brittle across Skia versions. This is a deliberate deviation; the dedicated LCD-pattern firmware is dropped.
- Types: `lcd_view_draw(const dp32g030_cpu_t*, SkCanvas*, int)` and `Pacer::steps_for(double, uint64_t, size_t)` are used the same way in T4.
- Proportion: plan is shorter than the code it describes, signatures and test names only.
