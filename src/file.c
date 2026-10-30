#include "file.h"

#include <stdio.h>

uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;

  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }

  uint8_t *buf = malloc((size_t)len ? (size_t)len : 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }

  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (n != (size_t)len) {
    free(buf);
    return NULL;
  }

  *out_size = n;
  return buf;
}
