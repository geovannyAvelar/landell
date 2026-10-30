#include <stdint.h>

#define N_FIB 16
#define N_SORT 8

static volatile uint32_t fib[N_FIB];
static volatile uint32_t data[N_SORT] = {9, 3, 7, 1, 8, 2, 6, 4};
static volatile uint32_t squares = 10;

__attribute__((noinline)) static void compute_fib(void) {
  fib[0] = 0;
  fib[1] = 1;
  for (uint32_t i = 2; i < N_FIB; i++)
    fib[i] = fib[i - 1] + fib[i - 2];
}

__attribute__((noinline)) static uint32_t sum_squares(uint32_t n) {
  uint32_t sum = 0;

  for (uint32_t i = 0; i < n; i++)
    sum += i * i;
  return sum;
}

__attribute__((noinline)) static void bubble_sort(volatile uint32_t *a,
                                                  uint32_t n) {
  for (uint32_t i = 0; i + 1 < n; i++) {
    for (uint32_t j = 0; j + 1 < n - i; j++) {
      if (a[j] > a[j + 1]) {
        uint32_t t = a[j];
        a[j] = a[j + 1];
        a[j + 1] = t;
      }
    }
  }
}

__attribute__((noinline)) static int is_sorted(volatile uint32_t *a,
                                               uint32_t n) {
  for (uint32_t i = 0; i + 1 < n; i++)
    if (a[i] > a[i + 1])
      return 0;
  return 1;
}

uint32_t main(void) {
  compute_fib();
  bubble_sort(data, N_SORT);

  if (!is_sorted(data, N_SORT))
    return 0xDEAD0000u;

  return (sum_squares(squares) << 16) | fib[N_FIB - 1];
}
