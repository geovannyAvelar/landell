#include "beeper.h"
#include "k5ser_bridge.h"
#include "keypad.h"
#include "lcd_view.h"
#include "mic.h"
#include "pacer.h"
#include "panel.h"
#include "rtl_tcp.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <SDL.h>

#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

extern "C" {
#include "file.h"
}

namespace {

constexpr int kLcdWidth = 128;
constexpr int kLcdHeight = 64;
constexpr int kDefaultScale = 4;
constexpr size_t kDefaultScreenshotSteps = 10000000;

enum {
  EXIT_USAGE = 1,
  EXIT_FAULT = 2,
  EXIT_UNDEF = 3,
};

struct AdcInput {
  unsigned ch;
  unsigned val;
};

struct Options {
  const char *soc = "auto";
  const char *path = nullptr;
  const char *screenshot = nullptr;
  bool k5ser = false;
  int scale = kDefaultScale;
  size_t steps = kDefaultScreenshotSteps;
  AdcInput adc[16];
  unsigned adc_n = 0;
  Key press[int(Key::Count)];
  unsigned press_n = 0;
  bool flock = false; // PTT+SIDE1 at power-on → F-Lock / advanced config
  bool mute = false;  // skip SDL audio (UI beeps)
  bool no_mic = false; // skip SDL capture (VOX mic level)
  bool iq_tcp = false; // stream NBFM TX IQ over rtl_tcp
  unsigned iq_port = 1234;
};

// Shared with the core through hooks.user.
struct Ui {
  bool dirty = true; // LCD or panel needs redrawing
  int backlight = -1; // level (0-255) of the last drawn frame
  Keypad *keypad = nullptr;
  K5serBridge *k5ser = nullptr;
  Beeper *beeper = nullptr;
  RtlTcp *iq = nullptr;
  unsigned tone_hz = 0; // last BK4819 tone; 0 = chip silent
  bool amp_on = false;  // GPIOC pin 4 / AUDIO_PATH
};

constexpr unsigned kAudioPathPort = 2; // GPIOC
constexpr uint32_t kAudioPathPin = 1u << 4;

void apply_beep(Ui *ui) {
  if (!ui->beeper)
    return;
  ui->beeper->set_hz(ui->amp_on ? ui->tone_hz : 0);
}

void uart_txed(void *user, unsigned uart, uint8_t byte) {
  Ui *ui = static_cast<Ui *>(user);
  if (ui->k5ser)
    ui->k5ser->on_tx(uart, byte);
}

// Backlight level 0-255; boards without a modeled backlight stay lit.
int backlight_level(dp32g030_cpu_t *cpu) {
  return dp32g030_has_backlight(cpu) ? dp32g030_backlight_level(cpu) : 255;
}

void lcd_updated(void *user) { static_cast<Ui *>(user)->dirty = true; }

void tone_changed(void *user, unsigned hz) {
  Ui *ui = static_cast<Ui *>(user);
  ui->tone_hz = hz;
  apply_beep(ui);
}

void tx_changed(void *user, bool active, uint32_t hz) {
  Ui *ui = static_cast<Ui *>(user);
  if (ui->iq)
    ui->iq->set_tx(active, hz);
}

void gpio_changed(void *user, unsigned port, uint32_t pins, uint32_t) {
  Ui *ui = static_cast<Ui *>(user);
  if (ui->keypad)
    ui->keypad->on_gpio_change(port);
  if (port == kAudioPathPort) {
    bool amp = (pins & kAudioPathPin) != 0;
    if (amp != ui->amp_on) {
      ui->amp_on = amp;
      apply_beep(ui);
    }
  }
}

void usage(FILE *out, const char *prog) {
  std::fprintf(
      out,
      "usage: %s [--scale N] [--soc CHIP] [--k5ser] [-a|--adc CH=VAL] [--press KEYS] "
      "[--flock] [--mute] [--no-mic] [--iq-tcp [PORT]] "
      "[--screenshot out.png [--steps N]] firmware.bin\n"
      "\n"
      "  --scale N          window pixels per LCD pixel, 1-16 (default %d)\n"
      "  --soc CHIP         auto (default), dp32g030 or py32f071\n"
      "  --k5ser            open a PTY bridged to UART1/USART1 for Chirp\n"
      "  -a, --adc CH=VAL   set SARADC channel CH (0-15) to 12-bit VAL; "
      "repeatable\n"
      "  --press KEY[,KEY]  hold keys from the start (0-9 menu up down exit "
      "star f ptt side1 side2)\n"
      "  --flock            boot into F-Lock / advanced config (hold PTT+side1)\n"
      "  --mute             disable UI beeps (SDL audio playback)\n"
      "  --no-mic           disable PC mic capture for VOX/TX AF (REG_64)\n"
      "  --iq-tcp [PORT]    stream NBFM TX IQ on rtl_tcp (default port 1234)\n"
      "  --screenshot FILE  run headless, write one frame as PNG and exit\n"
      "  --steps N          instructions to run before the screenshot\n"
      "  -h, --help         show this help\n",
      prog, kDefaultScale);
}

void add_press(Options *o, Key k) {
  for (unsigned i = 0; i < o->press_n; i++)
    if (o->press[i] == k)
      return;
  if (o->press_n < unsigned(Key::Count))
    o->press[o->press_n++] = k;
}

bool parse_uint(const char *s, unsigned long max, unsigned long *out,
                char **end) {
  if (*s == '\0' || *s == '-')
    return false;
  errno = 0;
  unsigned long v = std::strtoul(s, end, 0);
  if (errno != 0 || *end == s || v > max)
    return false;
  *out = v;
  return true;
}

// Returns 0 to continue, -1 for help (exit 0), EXIT_USAGE on error.
int parse_args(int argc, char **argv, Options *o) {
  const char *prog = argv[0];

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    unsigned long v;
    char *end;

    if (!std::strcmp(arg, "-h") || !std::strcmp(arg, "--help")) {
      usage(stdout, prog);
      return -1;
    }
    if (!std::strcmp(arg, "--scale")) {
      if (++i >= argc || !parse_uint(argv[i], ULONG_MAX, &v, &end) ||
          *end != '\0' || v < 1 || v > 16) {
        std::fprintf(stderr, "%s: invalid value for --scale (1-16)\n", prog);
        return EXIT_USAGE;
      }
      o->scale = int(v);
      continue;
    }
    if (!std::strcmp(arg, "--steps")) {
      if (++i >= argc || !parse_uint(argv[i], ULONG_MAX, &v, &end) ||
          *end != '\0' || v == 0) {
        std::fprintf(stderr, "%s: invalid value for --steps\n", prog);
        return EXIT_USAGE;
      }
      o->steps = size_t(v);
      continue;
    }
    if (!std::strcmp(arg, "--soc")) {
      if (++i >= argc) {
        std::fprintf(stderr, "%s: --soc needs a chip name\n", prog);
        return EXIT_USAGE;
      }
      o->soc = argv[i];
      continue;
    }
    if (!std::strcmp(arg, "--k5ser")) {
      o->k5ser = true;
      continue;
    }
    if (!std::strcmp(arg, "--flock")) {
      o->flock = true;
      continue;
    }
    if (!std::strcmp(arg, "--mute")) {
      o->mute = true;
      continue;
    }
    if (!std::strcmp(arg, "--no-mic")) {
      o->no_mic = true;
      continue;
    }
    if (!std::strcmp(arg, "--iq-tcp")) {
      o->iq_tcp = true;
      o->iq_port = 1234;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        if (!parse_uint(argv[i + 1], 65535, &v, &end) || *end != '\0' ||
            v == 0) {
          std::fprintf(stderr, "%s: invalid port for --iq-tcp\n", prog);
          return EXIT_USAGE;
        }
        o->iq_port = unsigned(v);
        i++;
      }
      continue;
    }
    if (!std::strcmp(arg, "--press")) {
      if (++i >= argc) {
        std::fprintf(stderr, "%s: --press needs a key list\n", prog);
        return EXIT_USAGE;
      }
      std::string list = argv[i];
      size_t pos = 0;
      while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        std::string name = list.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        Key k;
        if (!key_from_name(name.c_str(), &k) || o->press_n >= unsigned(Key::Count)) {
          std::fprintf(stderr, "%s: invalid key '%s' for --press\n", prog,
                       name.c_str());
          return EXIT_USAGE;
        }
        o->press[o->press_n++] = k;
        if (comma == std::string::npos)
          break;
        pos = comma + 1;
      }
      continue;
    }
    if (!std::strcmp(arg, "--screenshot")) {
      if (++i >= argc) {
        std::fprintf(stderr, "%s: --screenshot needs a file\n", prog);
        return EXIT_USAGE;
      }
      o->screenshot = argv[i];
      continue;
    }
    if (!std::strcmp(arg, "--adc") || !std::strcmp(arg, "-a")) {
      unsigned long ch, val;

      if (++i >= argc || o->adc_n >= 16 ||
          !parse_uint(argv[i], 15, &ch, &end) || *end != '=' ||
          !parse_uint(end + 1, 0xFFF, &val, &end) || *end != '\0') {
        std::fprintf(stderr, "%s: invalid value for --adc (CH=VAL)\n", prog);
        return EXIT_USAGE;
      }
      o->adc[o->adc_n++] = {unsigned(ch), unsigned(val)};
      continue;
    }
    if (arg[0] == '-' && arg[1] != '\0') {
      std::fprintf(stderr, "%s: unknown option '%s'\n", prog, arg);
      usage(stderr, prog);
      return EXIT_USAGE;
    }
    if (o->path) {
      std::fprintf(stderr, "%s: more than one firmware file given\n", prog);
      return EXIT_USAGE;
    }
    o->path = arg;
  }

  if (!o->path) {
    usage(stderr, prog);
    return EXIT_USAGE;
  }
  // F4HWN / egzumer: PTT held + SIDE1 twice → BOOT_MODE_F_LOCK (advanced menu).
  if (o->flock) {
    add_press(o, Key::Ptt);
    add_press(o, Key::Side1);
  }
  return 0;
}

// Owns the Skia raster surface the LCD is drawn into.
struct Canvas {
  SkBitmap bitmap;
  sk_sp<SkSurface> surface;

  explicit Canvas(int scale) {
    SkImageInfo info =
        SkImageInfo::Make(kLcdWidth * scale, (kLcdHeight + kPanelUnits) * scale,
                          kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    bitmap.allocPixels(info);
    surface = SkSurfaces::WrapPixels(info, bitmap.getPixels(), bitmap.rowBytes());
  }
};

int status_exit_code(uint8_t status) {
  switch (status) {
  case EMU_OK:
    return 0;
  case EMU_FAULT:
    return EXIT_FAULT;
  default:
    return EXIT_UNDEF;
  }
}

// Cycles to keep PTT+SIDE1 down so BOOT_GetMode can sample F-Lock, then release
// so the firmware can leave the "RELEASE ALL KEYS" prompt.
constexpr uint64_t kFlockHoldCycles = 100000000ull;

void release_flock_keys(Keypad *pad) {
  pad->set(Key::Ptt, false);
  pad->set(Key::Side1, false);
}

int run_screenshot(dp32g030_cpu_t **cpup, const Options &o, Keypad *keypad) {
  uint8_t status = EMU_OK;
  if (o.flock && o.steps > kFlockHoldCycles) {
    status = dp32g030_cpu_run(cpup, size_t(kFlockHoldCycles));
    release_flock_keys(keypad);
    if (status == EMU_OK && !(*cpup)->halted)
      status = dp32g030_cpu_run(cpup, o.steps - size_t(kFlockHoldCycles));
  } else {
    status = dp32g030_cpu_run(cpup, o.steps);
  }
  dp32g030_cpu_t *cpu = *cpup;
  Canvas canvas(o.scale);
  lcd_view_draw(cpu, canvas.surface->getCanvas(), o.scale,
                float(backlight_level(cpu)) / 255.0f);
  panel_draw(canvas.surface->getCanvas(), o.scale, *keypad);

  SkPixmap pixmap;
  if (!canvas.bitmap.peekPixels(&pixmap)) {
    std::fprintf(stderr, "cannot read back the frame\n");
    return EXIT_USAGE;
  }
  SkFILEWStream out(o.screenshot);
  if (!out.isValid() || !SkPngEncoder::Encode(&out, pixmap, {})) {
    std::fprintf(stderr, "cannot write '%s'\n", o.screenshot);
    return EXIT_USAGE;
  }
  out.flush();
  return status_exit_code(status);
}

// PC keyboard to UV-K5 key. Shift+8 / Shift+3 give * and # on US layouts, where
// SDL reports the unshifted keycode.
bool key_for_sym(SDL_Keycode sym, Uint16 mod, Key *out) {
  if (sym >= SDLK_0 && sym <= SDLK_9) {
    bool shift = (mod & KMOD_SHIFT) != 0;
    if (shift && sym == SDLK_8)
      *out = Key::Star;
    else if (shift && sym == SDLK_3)
      *out = Key::F;
    else
      *out = Key(int(Key::K0) + int(sym - SDLK_0));
    return true;
  }
  if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9) {
    *out = Key(int(Key::K1) + int(sym - SDLK_KP_1));
    return true;
  }
  switch (sym) {
  case SDLK_KP_0: *out = Key::K0; return true;
  case SDLK_RETURN:
  case SDLK_KP_ENTER: *out = Key::Menu; return true;
  case SDLK_UP: *out = Key::Up; return true;
  case SDLK_DOWN: *out = Key::Down; return true;
  case SDLK_BACKSPACE: *out = Key::Exit; return true;
  case SDLK_ASTERISK:
  case SDLK_KP_MULTIPLY: *out = Key::Star; return true;
  case SDLK_HASH:
  case SDLK_f: *out = Key::F; return true;
  case SDLK_SPACE: *out = Key::Ptt; return true;
  case SDLK_F1: *out = Key::Side1; return true;
  case SDLK_F2: *out = Key::Side2; return true;
  default: return false;
  }
}

// Which input holds each key: the keyboard (by SDL keycode) and the mouse.
struct Held {
  Keypad *pad;
  Ui *ui;
  std::map<SDL_Keycode, Key> keys;
  bool mouse_down = false;
  Key mouse_key = Key::K0;
  bool changed = false; // set when a key edge is applied this frame

  bool held(Key k) const {
    if (mouse_down && mouse_key == k)
      return true;
    for (const auto &e : keys)
      if (e.second == k)
        return true;
    return false;
  }
  void sync(Key k) {
    bool want = held(k);
    if (pad->pressed(k) != want) {
      pad->set(k, want);
      ui->dirty = true;
      changed = true;
    }
  }
  void key_down(SDL_Keycode sym, Key k) {
    keys[sym] = k;
    sync(k);
  }
  void key_up(SDL_Keycode sym) {
    auto it = keys.find(sym);
    if (it == keys.end())
      return;
    Key k = it->second;
    keys.erase(it);
    sync(k);
  }
  void mouse_press(Key k) {
    mouse_release();
    mouse_down = true;
    mouse_key = k;
    sync(k);
  }
  void mouse_release() {
    if (!mouse_down)
      return;
    mouse_down = false;
    sync(mouse_key);
  }
  void release_all() {
    std::map<SDL_Keycode, Key> old;
    old.swap(keys);
    for (const auto &e : old)
      sync(e.second);
    mouse_release();
  }
};

int run_window(dp32g030_cpu_t **cpup, const Options &o, Ui *ui, Keypad *pad) {
  dp32g030_cpu_t *cpu = *cpup;
  Uint32 sdl_flags = SDL_INIT_VIDEO;
  if (!o.mute || !o.no_mic)
    sdl_flags |= SDL_INIT_AUDIO;
  if (SDL_Init(sdl_flags) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_USAGE;
  }
  Beeper beeper;
  if (!o.mute) {
    if (!beeper.open())
      std::fprintf(stderr, "warning: no audio playback (%s); beeps disabled\n",
                   SDL_GetError());
    else
      ui->beeper = &beeper;
  }
  Mic mic;
  bool mic_ok = false;
  if (!o.no_mic) {
    if (!mic.open())
      std::fprintf(stderr, "warning: no mic capture (%s); VOX/TX mic disabled\n",
                   SDL_GetError());
    else
      mic_ok = true;
  }
  RtlTcp iq;
  SDL_Window *window = SDL_CreateWindow(
      "landell", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kLcdWidth * o.scale, (kLcdHeight + kPanelUnits) * o.scale,
      SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer =
      window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
  if (!renderer && window)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_Texture *texture =
      renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   kLcdWidth * o.scale,
                                   (kLcdHeight + kPanelUnits) * o.scale)
               : nullptr;
  if (!texture) {
    std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
    ui->beeper = nullptr;
    beeper.close();
    mic.close();
    if (renderer)
      SDL_DestroyRenderer(renderer);
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_USAGE;
  }
  if (o.iq_tcp) {
    if (!iq.start(uint16_t(o.iq_port), mic_ok ? &mic : nullptr)) {
      std::fprintf(stderr, "warning: rtl_tcp listen failed on %u\n", o.iq_port);
    } else {
      ui->iq = &iq;
      iq.set_tx(dp32g030_bk4819_tx_active(cpu), dp32g030_bk4819_tx_hz(cpu));
      iq.set_muted(dp32g030_bk4819_tx_muted(cpu));
    }
  }

  // Logical size makes SDL map mouse coordinates back if the window manager
  // resizes the window to fit a small screen.
  SDL_RenderSetLogicalSize(renderer, kLcdWidth * o.scale,
                           (kLcdHeight + kPanelUnits) * o.scale);
  Canvas canvas(o.scale);
  Held held{pad, ui, {}, false, Key::K0, false};
  Pacer pacer;
  bool stopped = false;
  bool flock_released = !o.flock;
  unsigned capped_frames = 0;
  bool warned = false;
  size_t last_steps = 0;
  uint64_t last_cycles = cpu->cycles;
  auto last = std::chrono::steady_clock::now();
  auto last_present = last;
  // Firmware debounce is several ms; when the host is behind real time, push
  // a short catch-up burst after each key edge so the scan sees it sooner.
  constexpr double kKeyCatchUpS = 0.05;
  // Skia redraws compete with the emulator; while capped, present at ~30 Hz.
  constexpr double kMinPresentS = 1.0 / 30.0;

  for (bool quit = false; !quit;) {
    held.changed = false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_WINDOWEVENT &&
          (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
           ev.window.event == SDL_WINDOWEVENT_RESTORED ||
           ev.window.event == SDL_WINDOWEVENT_SHOWN))
        ui->dirty = true; // back buffer is undefined after a present
      if (ev.type == SDL_WINDOWEVENT &&
          ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        held.release_all();
      if (ev.type == SDL_QUIT ||
          (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
        quit = true;
      if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        Key k;
        if (key_for_sym(ev.key.keysym.sym, ev.key.keysym.mod, &k))
          held.key_down(ev.key.keysym.sym, k);
      }
      if (ev.type == SDL_KEYUP)
        held.key_up(ev.key.keysym.sym);
      if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
        auto hit = panel_hit(float(ev.button.x), float(ev.button.y), o.scale);
        if (hit)
          held.mouse_press(*hit);
      }
      if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)
        held.mouse_release();
      if (ev.type == SDL_MOUSEMOTION && held.mouse_down) {
        auto hit = panel_hit(float(ev.motion.x), float(ev.motion.y), o.scale);
        if (!hit || *hit != held.mouse_key)
          held.mouse_release();
      }
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last).count();
    last = now;

    if (ui->k5ser)
      ui->k5ser->poll_rx();

    // Feed PC mic amplitude into BK4819 REG_64 so firmware VOX can trip.
    if (mic_ok)
      dp32g030_bk4819_set_mic_level(cpu, mic.level());

    // REG_50 mute is not on the TX edge hook; refresh each frame for IQ.
    if (ui->iq)
      ui->iq->set_muted(dp32g030_bk4819_tx_muted(cpu));

    if (!stopped) {
      size_t steps =
          pacer.steps_for(elapsed, cpu->cycles - last_cycles, last_steps);
      uint64_t before = cpu->cycles;
      uint8_t status = dp32g030_cpu_run(cpup, steps);
      cpu = *cpup;
      pad->set_cpu(cpu);
      last_steps = steps;
      last_cycles = before;

      // Drop the power-on F-Lock chord once BOOT_GetMode has had time to sample.
      if (!flock_released && cpu->cycles >= kFlockHoldCycles) {
        flock_released = true;
        if (!held.held(Key::Ptt))
          pad->set(Key::Ptt, false);
        if (!held.held(Key::Side1))
          pad->set(Key::Side1, false);
        ui->dirty = true;
      }

      // Separate from the paced slice so the 0.1 s frame cap cannot eat it.
      if (held.changed && status == EMU_OK && !cpu->halted) {
        size_t boost = pacer.steps_for(kKeyCatchUpS, 0, 0);
        status = dp32g030_cpu_run(cpup, boost);
        cpu = *cpup;
        pad->set_cpu(cpu);
      }

      if (status != EMU_OK || cpu->halted) {
        char title[64];

        stopped = true;
        if (status == EMU_FAULT)
          std::snprintf(title, sizeof title, "landell - fault at pc=%08x",
                        unsigned(cpu->r[15]));
        else if (status != EMU_OK)
          std::snprintf(title, sizeof title, "landell - undefined insn");
        else
          std::snprintf(title, sizeof title, "landell - halted");
        SDL_SetWindowTitle(window, title);
        ui->dirty = true;
      }

      capped_frames = pacer.capped() ? capped_frames + 1 : 0;
      if (capped_frames >= 3 && !warned) {
        std::fprintf(stderr, "warning: cannot keep up with real time\n");
        warned = true;
      }
    }

    int level = backlight_level(cpu);
    if (ui->backlight < 0 || std::abs(level - ui->backlight) >= 3)
      ui->dirty = true; // the backlight moved enough to see
    double since_present =
        std::chrono::duration<double>(now - last_present).count();
    bool present_now =
        ui->dirty &&
        (held.changed || capped_frames == 0 || since_present >= kMinPresentS);
    if (present_now) {
      ui->dirty = false;
      ui->backlight = level;
      lcd_view_draw(cpu, canvas.surface->getCanvas(), o.scale,
                    float(level) / 255.0f);
      panel_draw(canvas.surface->getCanvas(), o.scale, *pad);
      SDL_UpdateTexture(texture, nullptr, canvas.bitmap.getPixels(),
                        int(canvas.bitmap.rowBytes()));
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
      last_present = now;
    }
    SDL_Delay(1);
  }

  ui->beeper = nullptr;
  ui->iq = nullptr;
  beeper.close();
  iq.stop();
  mic.close();
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Options opts;
  int rc = parse_args(argc, argv, &opts);
  if (rc != 0)
    return rc < 0 ? 0 : rc;

  size_t size = 0;
  uint8_t *fw = read_file(opts.path, &size);
  if (!fw) {
    std::fprintf(stderr, "%s: cannot read '%s'\n", argv[0], opts.path);
    return EXIT_USAGE;
  }
  const dp32g030_soc_t *soc;
  if (!std::strcmp(opts.soc, "auto")) {
    soc = dp32g030_soc_detect(fw, size);
  } else {
    soc = dp32g030_soc_by_name(opts.soc);
    if (!soc) {
      std::fprintf(stderr, "%s: unknown --soc '%s' (auto, dp32g030, py32f071)\n",
                   argv[0], opts.soc);
      std::free(fw);
      return EXIT_USAGE;
    }
  }
  dp32g030_cpu_t *cpu = dp32g030_cpu_new_soc(soc, fw, size);
  std::free(fw);
  if (!cpu) {
    std::fprintf(stderr,
                 "%s: '%s' is not a valid image for %s (%zu bytes; expected %s)\n",
                 argv[0], opts.path, dp32g030_soc_name(soc), size,
                 dp32g030_soc_image_hint(soc));
    return EXIT_USAGE;
  }

  Ui ui;
  Keypad keypad(cpu, keypad_wiring_for(dp32g030_soc_name(soc)));
  ui.keypad = &keypad;
  K5serBridge k5ser;
  if (opts.k5ser) {
    bool is_py32 = std::strcmp(dp32g030_soc_name(soc), "py32f071") == 0;
    if (!k5ser.open(is_py32)) {
      dp32g030_cpu_free(cpu);
      return EXIT_USAGE;
    }
    k5ser.cpup = &cpu;
    ui.k5ser = &k5ser;
  }
  cpu->periph.hooks.user = &ui;
  cpu->periph.hooks.lcd_update = lcd_updated;
  cpu->periph.hooks.gpio_change = gpio_changed;
  cpu->periph.hooks.bk4819_tone_change = tone_changed;
  cpu->periph.hooks.bk4819_tx_change = tx_changed;
  cpu->periph.hooks.uart_tx = uart_txed;
  keypad.refresh();
  for (unsigned i = 0; i < opts.press_n; i++)
    keypad.set(opts.press[i], true);

  dp32g030_soc_defaults(cpu);
  for (unsigned i = 0; i < opts.adc_n; i++)
    dp32g030_adc_set_input(cpu, opts.adc[i].ch, opts.adc[i].val);

  int code = opts.screenshot ? run_screenshot(&cpu, opts, &keypad)
                             : run_window(&cpu, opts, &ui, &keypad);
  k5ser.close();
  dp32g030_cpu_free(cpu);
  return code;
}
