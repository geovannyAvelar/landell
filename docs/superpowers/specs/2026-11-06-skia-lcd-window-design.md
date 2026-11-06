# Skia LCD window (v1: display only)

## Goal

Show the UV-K5 LCD (128x64, ST7565 model in `src/DP32G030/lcd.c`) in a live
window, drawn with Skia, while the firmware runs in real time. Display only.

## Scope

In:
- New binary `landell-gui` (C++17) in `gui/`.
- Skia raster surface, SDL2 for window and event loop.
- Real-time pacing against the emulated cycle counter.
- Battery ADC (channel 9) preset so firmware draws its UI.
- Optional Meson build (`-Dgui=enabled`, `-Dskia_dir=PATH`), off by default.
- Helper script to build Skia from source.
- `--screenshot out.png` headless mode (one frame, no window) for tests.

Out (separate specs): keyboard/PTT to GPIO key matrix, EEPROM, audio, BK4819 UI.

The C core and the `landell` CLI do not change, except for the shared ADC
preset if it is moved to a reusable helper.

## Architecture

```
gui/main.cpp      arg parsing, load firmware, SDL init, main loop
gui/lcd_view.*    LcdView: cpu -> SkCanvas (pure drawing, no SDL)
gui/pacing.*      Pacer: wall time -> cycle budget
scripts/build-skia.sh
```

- `LcdView::draw(const dp32g030_cpu_t*, SkCanvas*)` reads pixels only through
  `dp32g030_lcd_get_pixel` / `dp32g030_lcd_is_on`. It knows nothing about SDL.
  This is the unit tests target.
- `Pacer` converts elapsed wall time to a cycle budget at 48 MHz, with a cap
  per frame (avoids a spiral after a stall).
- `main.cpp` loop, single thread: poll SDL events (quit) -> run emulator for
  the frame budget -> if LCD dirty, draw into the Skia surface -> upload the
  pixmap to an SDL texture -> present. The `lcd_update` hook only sets `dirty`.

## Rendering

- Logical size 128x64, default scale 6 (`--scale N`, 1..16).
- Pixel on/off drawn as `SkRect` fills with a 1 px gap scaled with N.
- Off display: flat backlight-off color. On, no pixel: light background.
- Palette fixed in `lcd_view.cpp`; no config in v1.

## Emulator control

- `dp32g030_cpu_run(&cpu, steps)` is the only entry. The budget is converted
  from cycles to steps with a measured cycles-per-step average; the loop
  stops a slice early if `cpu->halted`.
- On halt or fault the window stays open, shows the last frame, and the
  title shows the state.
- ADC channel 9 preset to 2000; `--adc CH=VAL` overrides, same syntax as the
  CLI.

## Build

- `meson_options.txt`: `gui` (feature, default disabled), `skia_dir` (string).
- With `gui` enabled: `add_languages('cpp')`, `dependency('sdl2')`, Skia
  include dir and `libskia.a` from `skia_dir`, Skia's own system libs
  (freetype/fontconfig only if the Skia build uses them; v1 builds Skia with
  fonts off, no text drawn).
- The GUI target is in a separate `gui/meson.build` and does not use
  `werror` for Skia headers.
- `scripts/build-skia.sh`: clone depot_tools and skia to `$SKIA_DIR`
  (default `~/skia`), `git-sync-deps`, `gn gen` with
  `is_official_build=true skia_use_gl=false skia_use_system_*=false
  skia_enable_fontmgr_*=false` plus static lib target, then `ninja skia`.
  Skia is pinned to one milestone branch recorded in the script.

## Testing

- `LcdView` unit test: feed a cpu with a known LCD RAM pattern, draw into a
  raster `SkSurface`, assert specific pixels (on/off/gap) by color.
- `--screenshot` smoke test: run the egzumer-free test firmware in
  `firmware/` that writes a fixed LCD pattern, compare PNG to a golden file.
  Skipped when `gui` is disabled.
- The existing `meson test` stays green with `gui` disabled.

## Risks

- Skia build time and size (tens of minutes, several GB). Isolated in the
  script; the main build never depends on it.
- Skia API churn: pinned milestone.
- Emulator speed: the default build is `-O0`; real time at 48 MHz needs
  `-Dbuildtype=release` or `-O2` for the GUI. The GUI warns when it cannot keep
  up (budget capped) instead of failing.

## Success criteria

- `landell-gui -a 9=2000 firmware.bin` opens a window showing the egzumer
  VFO screen, updating as the firmware redraws.
- `--screenshot` produces a PNG identical to the golden one in the test.
- Default `meson setup build && meson test -C build` is unchanged.
