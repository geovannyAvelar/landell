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

static void usage(FILE *out, const char *prog) {
  fprintf(out,
          "usage: %s [-s steps] [-u uart] firmware.bin\n"
          "\n"
          "  -s, --steps N  maximum instructions to execute (default %llu)\n"
          "  -u, --uart N   print bytes sent by UART N (0-2) to stdout\n"
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

  dp32g030_cpu_t *cpu = dp32g030_cpu_new(fw, size);
  free(fw);
  if (!cpu) {
    fprintf(stderr, "%s: '%s' is not a valid image (%zu bytes, need 8..%d)\n",
            prog, path, size, DP32G030_FLASH_SIZE);
    return EXIT_USAGE;
  }

  cpu->periph.hooks.uart_tx = uart_tx;
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

  dp32g030_cpu_free(cpu);
  return code;
}
