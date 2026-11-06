# GUI Buttons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Clickable key panel plus PC keyboard in `landell-gui`, driving the UV-K5 keypad matrix on GPIO.

**Architecture:** `Keypad` (pure, real cpu) turns a pressed-key set into GPIOA PA3..PA6 / GPIOC PC5 input levels; `Panel` lays out and draws buttons; `main.cpp` wires SDL events to `Keypad`.

**Tech Stack:** C++17, Skia raster, SDL2, Meson (existing `gui/` setup).

**Spec:** `docs/superpowers/specs/2026-11-06-gui-buttons-design.md`

## Global Constraints

- Core (`src/`) and `landell` CLI unchanged; all work in `gui/`.
- Keypad drives only PA3..PA6 (GPIOA, port 0) and PC5 (GPIOC, port 2); never PA10..PA13.
- Matrix: rows PA10..PA13 (outputs), cols PA3..PA6. Row PA10: MENU,1,4,7; PA11: UP,2,5,8; PA12: DOWN,3,6,9; PA13: EXIT,STAR,0,F. SIDE1 -> PA3 low, SIDE2 -> PA4 low (any row), PTT -> PC5 low.
- "Row driven low" = `gpio[0].dir` bit set and `gpio[0].out` bit clear.
- Esc and window close quit; Backspace = EXIT. Auto-repeat ignored; focus loss releases all.
- Skia has no fonts: labels use embedded 5x7 glyphs.

## Review Focus

- Hook recursion: `refresh()` from `gpio_change` must not loop (reentrancy guard). Pinned in Task 1.
- Row configured as input (`dir`=0) never pulls a column low. Pinned in Task 1.
- Key pressed during startup before firmware sets pull-ups still reads correctly (all four columns driven from the first `refresh()`). Pinned in Task 1.
- Release on mouse-up outside the window or focus loss never leaves a stuck key. Pinned in Task 3.

---

### Task 1: Keypad

**Files:** Create `gui/keypad.h`, `gui/keypad.cpp`, `gui/test_keypad.cpp`; modify `gui/meson.build`.

**Interfaces:**
- Produces: `enum class Key { K0,K1,K2,K3,K4,K5,K6,K7,K8,K9,Menu,Up,Down,Exit,Star,F,Ptt,Side1,Side2, Count }`; `class Keypad { explicit Keypad(dp32g030_cpu_t*); void set(Key,bool); bool pressed(Key) const; void refresh(); void on_gpio_change(unsigned port); }`; `const char* key_name(Key)` (lowercase: "0".."9","menu","up","down","exit","star","f","ptt","side1","side2") and `bool key_from_name(const char*, Key*)`.

- [ ] **Step 1: Failing tests** in `gui/test_keypad.cpp` (plain main, CHECK macro like `test_pacer.cpp`, real `dp32g030_cpu_new` on the 8-byte image): set `cpu->periph.gpio[0].dir = rows mask (PA10..PA13)`, `.out = all rows high`; helper `cols()` returns `(dp32g030_gpio_pins(cpu,0) >> 3) & 0xF`. Tests: `each_matrix_position` (for each of the 16 (row,col): out has only that row low, press key, cols == 0xF & ~(1<<col); release -> 0xF; other rows high -> 0xF); `row_high_no_pull`; `row_as_input_no_pull` (dir=0 for that row); `side1_pa3`, `side2_pa4` (rows all high, cols bit 0 / bit 1 low); `ptt_pc5` (`gpio_pins(cpu,2) & (1<<5)` is 0 pressed, set released); `two_keys_one_row` (MENU+4 in row PA10 -> cols bits 0 and 2 low); `initial_refresh_drives_high` (before any press, after `refresh()`, cols == 0xF and PC5 high, without pull-ups); `hook_recompute` (install `hooks.user=&keypad`, `hooks.gpio_change` calling `on_gpio_change`; press MENU with row high, then write `cpu->periph`... drive row low via `dp32g030_bus_write` to GPIOA DATA 0x40060000 -> cols bit 0 low); `no_reentrancy_loop` (same hook, 1000 writes terminate). Name tests via comments.
- [ ] **Step 2: Run, expect FAIL** (link error): `meson test -C build-gui keypad`.
- [ ] **Step 3: Implement** `refresh()`: compute cols mask from pressed matrix keys and driven-low rows, plus SIDE1/SIDE2; call `dp32g030_gpio_set_input(cpu, 0, 0x78, cols<<3)` (mask bits 3..6) and `dp32g030_gpio_set_input(cpu, 2, 1u<<5, ptt?0:1u<<5)`. `on_gpio_change(port)` calls `refresh()` when port==0 and not already refreshing.
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(gui): add Keypad matrix model`.

### Task 2: glyphs and Panel

**Files:** Create `gui/glyphs.h/.cpp`, `gui/panel.h/.cpp`, `gui/test_panel.cpp`; modify `gui/meson.build`.

**Interfaces:**
- Consumes: `Key`, `Keypad::pressed` (Task 1).
- Produces: `constexpr int kPanelUnits` (panel height in LCD-pixel units, 80); `struct KeyRect { Key key; float x, y, w, h; }` in logical units where the width is 128; `const std::vector<KeyRect>& panel_keys()`; `std::optional<Key> panel_hit(float px, float py, int scale)` (window pixels, y measured from the LCD top, i.e. includes the 64-unit LCD area); `void panel_draw(SkCanvas*, int scale, const Keypad&)` draws into the area below the LCD (translate by `64*scale`); `bool glyph_rows(char c, uint8_t rows[7])` (5-bit rows; chars: 0-9 * # A-Z used by MENU EXIT PTT S1 S2 F).

- [ ] **Step 1: Failing tests** `gui/test_panel.cpp`: `every_key_has_rect` (all `Key` values appear once in `panel_keys()`), `no_overlap`, `inside_panel` (all rects within 128 x kPanelUnits), `hit_center` (at scales 1 and 6, center of each rect returns that key), `hit_outside` (LCD area, gap between buttons, below panel, negative -> nullopt), `glyphs_for_labels` (every char in the label set returns true; `'~'` false), `draw_pressed_differs` (draw twice into raster surfaces, with and without MENU pressed; center pixel of MENU differs, center pixel of key 5 equal).
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** layout (top row MENU/UP/DOWN/EXIT, then 3x4 digits grid, PTT/SIDE1/SIDE2 on the left edge), 5x7 font table, drawing (rounded rects, pressed darker, labels via glyph pixels, UP/DOWN as triangles; no text API).
- [ ] **Step 4: Run, expect PASS.**
- [ ] **Step 5: Commit** `feat(gui): add key Panel and glyph labels`.

### Task 3: window integration, --press, docs

**Files:** Modify `gui/main.cpp`, `gui/test_screenshot.py`, `README.md`.

**Interfaces:** Consumes everything above. Produces CLI `--press KEY[,KEY...]`.

- [ ] **Step 1: Failing tests** in `gui/test_screenshot.py`: PNG height is `(64+80)*scale` and width `128*scale` for `--scale 2` (replace old 128-high check); `--press bogus` -> exit 1, stderr has `press`; two screenshots (`--press menu` vs none) differ in the PNG bytes.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** in `main.cpp`: window height `(64+kPanelUnits)*scale`; `Canvas` sized to the same; draw LCD then `panel_draw`; `Keypad` built after cpu, `hooks.gpio_change` -> `on_gpio_change`, initial `refresh()`; `--press` parsed with `key_from_name` and applied before the run; SDL events: mouse down on `panel_hit` presses + remembers; mouse up releases; `SDL_MOUSEMOTION` leaving the button releases; `SDL_WINDOWEVENT_FOCUS_LOST` releases all; keyboard map per spec with `ev.key.repeat` ignored (separate keyboard-held set so mouse and keyboard OR together); redraw when the Keypad pressed set changes (panel dirty flag).
- [ ] **Step 4: Run all** `meson test -C build-gui` (expect all OK); `meson test -C build` unchanged.
- [ ] **Step 5: README** `GUI` section: panel, keyboard map table, `--press`. Commit `feat(gui): clickable key panel and keyboard input`.
- [ ] **Step 6: Manual check** with `landell-gui firmware/egzumer_v0.22.bin` under Xvfb: send `--press menu` screenshot at 320M steps, expect the menu on the LCD (compare against unpressed).
