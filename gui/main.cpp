#include "lcd_view.h"
#include "pacer.h"

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
#include <string>

extern "C" {
#include "file.h"
}

namespace {

constexpr int kLcdWidth = 128;
constexpr int kLcdHeight = 64;
constexpr int kDefaultScale = 6;
constexpr unsigned kBatteryAdcChannel = 9;
constexpr unsigned kBatteryAdcValue = 2000;
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
  const char *path = nullptr;
  const char *screenshot = nullptr;
  int scale = kDefaultScale;
  size_t steps = kDefaultScreenshotSteps;
  AdcInput adc[16];
  unsigned adc_n = 0;
};

struct LcdState {
  bool dirty = true;
};

void lcd_updated(void *user) { static_cast<LcdState *>(user)->dirty = true; }

void usage(FILE *out, const char *prog) {
  std::fprintf(
      out,
      "usage: %s [--scale N] [--adc CH=VAL] [--screenshot out.png "
      "[--steps N]] firmware.bin\n"
      "\n"
      "  --scale N          window pixels per LCD pixel, 1-16 (default %d)\n"
      "  --adc CH=VAL       set SARADC channel CH (0-15) to 12-bit VAL; "
      "repeatable\n"
      "  --screenshot FILE  run headless, write one frame as PNG and exit\n"
      "  --steps N          instructions to run before the screenshot\n"
      "  -h, --help         show this help\n",
      prog, kDefaultScale);
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
    if (!std::strcmp(arg, "--screenshot")) {
      if (++i >= argc) {
        std::fprintf(stderr, "%s: --screenshot needs a file\n", prog);
        return EXIT_USAGE;
      }
      o->screenshot = argv[i];
      continue;
    }
    if (!std::strcmp(arg, "--adc")) {
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
  return 0;
}

// Owns the Skia raster surface the LCD is drawn into.
struct Canvas {
  SkBitmap bitmap;
  sk_sp<SkSurface> surface;

  explicit Canvas(int scale) {
    SkImageInfo info =
        SkImageInfo::Make(kLcdWidth * scale, kLcdHeight * scale,
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

int run_screenshot(dp32g030_cpu_t **cpup, const Options &o) {
  uint8_t status = dp32g030_cpu_run(cpup, o.steps);
  dp32g030_cpu_t *cpu = *cpup;
  Canvas canvas(o.scale);
  lcd_view_draw(cpu, canvas.surface->getCanvas(), o.scale);

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

int run_window(dp32g030_cpu_t **cpup, const Options &o, LcdState *lcd) {
  dp32g030_cpu_t *cpu = *cpup;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return EXIT_USAGE;
  }
  SDL_Window *window = SDL_CreateWindow(
      "landell", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kLcdWidth * o.scale, kLcdHeight * o.scale, 0);
  SDL_Renderer *renderer =
      window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : nullptr;
  SDL_Texture *texture =
      renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_BGRA32,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   kLcdWidth * o.scale, kLcdHeight * o.scale)
               : nullptr;
  if (!texture) {
    std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
    if (renderer)
      SDL_DestroyRenderer(renderer);
    if (window)
      SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_USAGE;
  }

  Canvas canvas(o.scale);
  Pacer pacer;
  bool stopped = false;
  unsigned capped_frames = 0;
  bool warned = false;
  size_t last_steps = 0;
  uint64_t last_cycles = cpu->cycles;
  auto last = std::chrono::steady_clock::now();

  for (bool quit = false; !quit;) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT ||
          (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
        quit = true;
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - last).count();
    last = now;

    if (!stopped) {
      size_t steps = pacer.steps_for(elapsed, cpu->cycles - last_cycles, last_steps);
      uint64_t before = cpu->cycles;
      uint8_t status = dp32g030_cpu_run(cpup, steps);
      cpu = *cpup;
      last_steps = steps;
      last_cycles = before;

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
        lcd->dirty = true;
      }

      capped_frames = pacer.capped() ? capped_frames + 1 : 0;
      if (capped_frames >= 3 && !warned) {
        std::fprintf(stderr, "warning: cannot keep up with real time\n");
        warned = true;
      }
    }

    if (lcd->dirty) {
      lcd->dirty = false;
      lcd_view_draw(cpu, canvas.surface->getCanvas(), o.scale);
      SDL_UpdateTexture(texture, nullptr, canvas.bitmap.getPixels(),
                        int(canvas.bitmap.rowBytes()));
      SDL_RenderClear(renderer);
      SDL_RenderCopy(renderer, texture, nullptr, nullptr);
      SDL_RenderPresent(renderer);
    }
    SDL_Delay(1);
  }

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
  dp32g030_cpu_t *cpu = dp32g030_cpu_new(fw, size);
  std::free(fw);
  if (!cpu) {
    std::fprintf(stderr, "%s: '%s' is not a valid image (%zu bytes)\n", argv[0],
                 opts.path, size);
    return EXIT_USAGE;
  }

  LcdState lcd;
  cpu->periph.hooks.user = &lcd;
  cpu->periph.hooks.lcd_update = lcd_updated;

  dp32g030_adc_set_input(cpu, kBatteryAdcChannel, kBatteryAdcValue);
  for (unsigned i = 0; i < opts.adc_n; i++)
    dp32g030_adc_set_input(cpu, opts.adc[i].ch, opts.adc[i].val);

  int code = opts.screenshot ? run_screenshot(&cpu, opts)
                             : run_window(&cpu, opts, &lcd);
  dp32g030_cpu_free(cpu);
  return code;
}
