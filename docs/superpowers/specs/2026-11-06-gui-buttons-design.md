# GUI buttons (clickable panel + PC keyboard)

## Goal

Let a person press the UV-K5 keys in `landell-gui`: a Skia-drawn key panel below
the LCD (mouse) and PC keyboard shortcuts. Presses reach the firmware through
the real keypad matrix on GPIO.

## Scope

In: key matrix and PTT/side-key emulation, panel drawing, mouse and keyboard
input, window layout change, tests.
Out: audio, EEPROM, BK4819 UI, key repeat tuning, touch/gesture input, rebinding.

The C core and `landell` CLI do not change. Everything lives in `gui/`.

## Hardware model (from the egzumer firmware, driver/keyboard.c, gpio.h)

- Rows are outputs on GPIOA PA10, PA11, PA12, PA13 (called ROW0..ROW3 below,
  firmware names KEYBOARD_4..7). Columns are inputs on PA3, PA4, PA5, PA6
  (COL0..COL3, KEYBOARD_0..3), pulled up by the firmware.
- The firmware drives one row low at a time and reads the columns; a low column
  means the key at (row, column) is pressed.

| Row   | COL0 PA3 | COL1 PA4 | COL2 PA5 | COL3 PA6 |
|-------|----------|----------|----------|----------|
| PA10  | MENU     | 1        | 4        | 7        |
| PA11  | UP       | 2        | 5        | 8        |
| PA12  | DOWN     | 3        | 6        | 9        |
| PA13  | EXIT     | STAR (*) | 0        | F (#)    |

- Side keys: SIDE1 pulls PA3 low and SIDE2 pulls PA4 low whenever pressed,
  independent of any row (the firmware reads them with all rows high).
- PTT: GPIOC PC5 low while pressed.

## Architecture

```
gui/keypad.h/.cpp   Keypad: pressed set -> GPIO column/PTT input levels (no SDL, no Skia)
gui/panel.h/.cpp    Panel: key layout in logical units, hit-test, Skia drawing
gui/glyphs.h/.cpp   5x7 bitmap glyphs for button labels
gui/main.cpp        SDL events -> Keypad; window = LCD + panel
```

### Keypad

- `enum class Key { K0..K9, Menu, Up, Down, Exit, Star, F, Ptt, Side1, Side2 }`.
- `Keypad(dp32g030_cpu_t* cpu)`; `void set(Key, bool pressed)`; `void refresh()`.
- `refresh()` computes the input levels and calls `dp32g030_gpio_set_input`:
  - GPIOA mask PA3..PA6: bit is 0 if a pressed matrix key is on a column whose
    row is currently an output driven low, or if SIDE1 (PA3) / SIDE2 (PA4) is
    pressed; otherwise 1.
  - GPIOC pin 5 (PC5): 0 while PTT is pressed, else 1.
- "Row driven low" = pin is an output (`dir` bit set) and its out level is 0.
- The GUI installs `hooks.gpio_change`; for port 0 it calls `keypad.refresh()`.
  `set()` also calls `refresh()`. Because the firmware changes row outputs and
  samples the columns in the same step sequence, `refresh()` runs from the hook
  before the next instruction reads GPIOA.
- Level semantics must match the core: `set_input` marks the pin as driven; once
  driven, the host value wins over the PORTx_PU pull-up. Keypad therefore
  always drives all four columns and PC5 from the first `refresh()`
  (called once at startup).

### Panel

- Layout in logical units (not pixels), scaled by the window scale: body area
  below the LCD with: top row MENU, UP, DOWN, EXIT; then 1-2-3, 4-5-6, 7-8-9,
  `*`-0-`#`; PTT and SIDE1/SIDE2 as side buttons on the left of the body.
- `Panel::hit(x, y, scale) -> std::optional<Key>`.
- `Panel::draw(SkCanvas*, int scale, const Keypad&)` draws rounded buttons;
  pressed buttons are drawn darker. Labels use `glyphs`: only the glyphs
  needed (digits, `*`, `#`, letters of MENU/EXIT/PTT/S1/S2); UP/DOWN are drawn
  as triangles. No Skia fonts (Skia is built without them).
- Panel height is a fixed number of logical units so window height =
  `64*scale + panel_height*scale`.

### Window / input

- Window size: `128*scale` wide, `(64 + panel_units)*scale` tall.
- Mouse: left press on a button sets the key and remembers it as the mouse key;
  release (anywhere) or leaving the button releases it. One mouse key at a time.
- Keyboard (SDL keycodes), held = pressed:
  - `0`-`9` and keypad `0`-`9`: digits.
  - `Return`: MENU. `Up`/`Down`: UP/DOWN. `Backspace`: EXIT.
  - `*` (and keypad `*`): STAR. `#` and `F`: F.
  - `Space`: PTT. `F1`: SIDE1. `F2`: SIDE2.
  - `Esc` and window close quit (unchanged). No new quit shortcut.
- Keyboard and mouse can hold different keys at once; the Keypad set is the OR.
- Auto-repeat events are ignored; focus loss releases all keys.
- Pressed state must be redrawn: the window is redrawn when the panel changes
  (dirty flag), not only when the LCD changes.

### Screenshot mode

`--screenshot` output includes the panel (taller PNG: `128*scale` x
`(64+panel_units)*scale`). Optional `--press KEY[,KEY...]` presses keys before
the run so tests can see the pressed look; keys named as in `Key` lower-case
(`menu`, `1`, `star`, `f`, `ptt`, `side1`...).

## Testing

- `test_keypad` (C++, real `dp32g030_cpu_t`, no window): with ROW0 set as an
  output driven low, pressing MENU makes PA3 read 0 and PA4..6 read 1; with
  ROW0 high it reads 1; each of the 16 matrix positions maps to the right
  (row, column); SIDE1 pulls PA3 low with every row high; PTT pulls PC5 low;
  releasing restores 1; two keys in one row; row not an output (`dir` 0) never
  pulls a column low.
- `test_panel`: every `Key` has a hit rectangle, rectangles do not overlap, hit
  outside returns nullopt, hit at button centre returns that key at scales 1
  and 6.
- Screenshot test: PNG size equals the taller window; with `--press menu` the
  MENU button pixel differs from the unpressed screenshot.
- No automated end-to-end test with the egzumer firmware (it is not in the
  repo and a run takes ~15 s); covered by the manual check below.
- Manual: run `landell-gui firmware/egzumer_v0.22.bin`; pressing MENU on the
  panel and `Return` opens the menu on the LCD.

## Risks

- Timing: the firmware debounces key reads over many ms; the emulator runs
  slower than real time, so key presses must be held across several frames.
  Holding while the button/key is down satisfies this.
- Shared pins: PA10/PA11 also carry I2C and PA12/PA13 the voice chip; a
  gpio_change refresh while the firmware bit-bangs I2C on PA10/PA11 must not
  corrupt it. Keypad only drives PA3..PA6 and PC5, never PA10..PA13.
- The CLI `landell` has no way to press keys; out of scope.

## Success criteria

- On the real firmware, pressing the MENU button (mouse) or `Return` makes the
  LCD change to the menu; releasing returns normal behavior; digit keys enter a
  frequency.
- PTT held keeps GPIOC PC5 low (observable with a unit test).
- Default build and existing tests unchanged; with `gui` disabled nothing is built.
