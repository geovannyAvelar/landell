#include "DP32G030.h"
#include "file.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_STEPS 10000000ull

enum {
  EXIT_USAGE = 1,
  EXIT_FAULT = 2,
  EXIT_UNDEF = 3,
};

static int uart_echo = -1;

static void uart_tx(void *user, unsigned uart, uint8_t byte) {
  (void)user;
  if ((int)uart == uart_echo)
    putchar(byte);
}

static void lcd_render(const dp32g030_cpu_t *c) {
  static const char *const glyph[4] = {" ", "\xe2\x96\x80", "\xe2\x96\x84",
                                       "\xe2\x96\x88"};

  printf("lcd: %s\n", dp32g030_lcd_is_on(c) ? "on" : "off");
  for (unsigned y = 0; y < 64; y += 2) {
    for (unsigned x = 0; x < 128; x++) {
      unsigned top = dp32g030_lcd_get_pixel(c, x, y);
      unsigned bottom = dp32g030_lcd_get_pixel(c, x, y + 1);

      fputs(glyph[top | (bottom << 1)], stdout);
    }
    putchar('\n');
  }
}

static void usage(FILE *out, const char *prog) {
  fprintf(out,
          "usage: %s [-s steps] [-u uart] [-a ch=val] [--soc chip] [-l] firmware.bin\n"
          "\n"
          "  -s, --steps N  maximum instructions to execute (default %llu)\n"
          "  -u, --uart N   print bytes sent by UART N (0-2) to stdout\n"
          "  -a, --adc CH=V set SARADC channel CH (0-15) to 12-bit value V; "
          "repeatable\n"
          "      --soc CHIP set the chip: auto (default), dp32g030 or py32f071\n"
          "  -l, --lcd      print the LCD contents after the run\n"
          "  -h, --help     show this help\n"
          "\n"
          "exit status: 0 ok, 1 usage or I/O error, 2 bus fault, 3 undefined "
          "instruction\n",
          prog, DEFAULT_STEPS);
}

static bool parse_steps(const char *s, size_t *out) {
  char *end;

  if (*s == '\0' || *s == '-')
    return false;

  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno != 0 || *end != '\0' || v == 0 || v > SIZE_MAX)
    return false;

  *out = (size_t)v;
  return true;
}

static void dump(const dp32g030_cpu_t *c) {
  for (unsigned i = 0; i < 13; i++)
    printf("r%-2u = %08" PRIx32 "%s", i, c->r[i],
           (i % 4 == 3 || i == 12) ? "\n" : "  ");
  printf("sp  = %08" PRIx32 "  lr  = %08" PRIx32 "  pc  = %08" PRIx32 "\n",
         c->r[13], c->r[14], c->r[15]);
  printf("xpsr = %08" PRIx32 "  [%c%c%c%c]\n", c->xpsr,
         (c->xpsr & (1u << 31)) ? 'N' : '-', (c->xpsr & (1u << 30)) ? 'Z' : '-',
         (c->xpsr & (1u << 29)) ? 'C' : '-',
         (c->xpsr & (1u << 28)) ? 'V' : '-');
}

int main(int argc, char **argv) {
  const char *prog = argv[0];
  const char *path = NULL;
  size_t steps = (size_t)DEFAULT_STEPS;
  bool show_lcd = false;
  const char *soc_name = "auto";
  unsigned adc_ch[16], adc_val[16], adc_n = 0;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      usage(stdout, prog);
      return 0;
    }
    if (!strcmp(arg, "-s") || !strcmp(arg, "--steps")) {
      if (++i >= argc || !parse_steps(argv[i], &steps)) {
        fprintf(stderr, "%s: invalid value for --steps\n", prog);
        return EXIT_USAGE;
      }
      continue;
    }
    if (!strcmp(arg, "-a") || !strcmp(arg, "--adc")) {
      char *end;
      unsigned long ch, val;

      if (++i >= argc || adc_n >= 16) {
        fprintf(stderr, "%s: invalid value for --adc\n", prog);
        return EXIT_USAGE;
      }
      ch = strtoul(argv[i], &end, 0);
      if (end == argv[i] || *end != '=' || ch > 15) {
        fprintf(stderr, "%s: invalid value for --adc\n", prog);
        return EXIT_USAGE;
      }
      val = strtoul(end + 1, &end, 0);
      if (*end != '\0' || val > 0xFFF) {
        fprintf(stderr, "%s: invalid value for --adc\n", prog);
        return EXIT_USAGE;
      }
      adc_ch[adc_n] = (unsigned)ch;
      adc_val[adc_n++] = (unsigned)val;
      continue;
    }
    if (!strcmp(arg, "--soc")) {
      if (++i >= argc) {
        fprintf(stderr, "%s: --soc needs a chip name\n", prog);
        return EXIT_USAGE;
      }
      soc_name = argv[i];
      continue;
    }
    if (!strcmp(arg, "-l") || !strcmp(arg, "--lcd")) {
      show_lcd = true;
      continue;
    }
    if (!strcmp(arg, "-u") || !strcmp(arg, "--uart")) {
      if (++i >= argc || strlen(argv[i]) != 1 || argv[i][0] < '0' ||
          argv[i][0] > '2') {
        fprintf(stderr, "%s: invalid value for --uart\n", prog);
        return EXIT_USAGE;
      }
      uart_echo = argv[i][0] - '0';
      continue;
    }
    if (arg[0] == '-' && arg[1] != '\0') {
      fprintf(stderr, "%s: unknown option '%s'\n", prog, arg);
      usage(stderr, prog);
      return EXIT_USAGE;
    }
    if (path) {
      fprintf(stderr, "%s: more than one firmware file given\n", prog);
      usage(stderr, prog);
      return EXIT_USAGE;
    }
    path = arg;
  }

  if (!path) {
    usage(stderr, prog);
    return EXIT_USAGE;
  }

  size_t size = 0;
  uint8_t *fw = read_file(path, &size);
  if (!fw) {
    fprintf(stderr, "%s: cannot read '%s'\n", prog, path);
    return EXIT_USAGE;
  }

  const dp32g030_soc_t *soc;
  if (!strcmp(soc_name, "auto")) {
    soc = dp32g030_soc_detect(fw, size);
  } else {
    soc = dp32g030_soc_by_name(soc_name);
    if (!soc) {
      fprintf(stderr, "%s: unknown --soc '%s' (auto, dp32g030, py32f071)\n",
              prog, soc_name);
      free(fw);
      return EXIT_USAGE;
    }
  }

  dp32g030_cpu_t *cpu = dp32g030_cpu_new_soc(soc, fw, size);
  free(fw);
  if (!cpu) {
    fprintf(stderr, "%s: '%s' is not a valid image for %s (%zu bytes)\n", prog,
            path, dp32g030_soc_name(soc), size);
    return EXIT_USAGE;
  }

  cpu->periph.hooks.uart_tx = uart_tx;
  for (unsigned k = 0; k < adc_n; k++)
    dp32g030_adc_set_input(cpu, adc_ch[k], adc_val[k]);
  uint8_t status = dp32g030_cpu_run(&cpu, steps);
  if (uart_echo >= 0)
    fflush(stdout);

  int code = 0;
  switch (status) {
  case EMU_OK:
    printf("%s\n", cpu->halted ? "halted" : "step limit reached");
    break;
  case EMU_FAULT:
    printf("fault at pc=%08" PRIx32 "\n", cpu->r[15]);
    code = EXIT_FAULT;
    break;
  case EMU_UNDEF:
    printf("undefined instruction at pc=%08" PRIx32 "\n", cpu->r[15]);
    code = EXIT_UNDEF;
    break;
  default:
    printf("unknown status %u\n", status);
    code = EXIT_FAULT;
    break;
  }
  dump(cpu);
  if (show_lcd)
    lcd_render(cpu);

  dp32g030_cpu_free(cpu);
  return code;
}
