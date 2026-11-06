#include "pacer.h"

#include <cstdio>

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                     \
      failures++;                                                              \
    }                                                                          \
  } while (0)

// Pacer with cycles-per-step forced to 1.0 by one calibration slice.
static Pacer calibrated() {
  Pacer p;
  p.steps_for(0.0, 0, 0); // no-op: steps_delta 0 is ignored
  for (int i = 0; i < 200; i++)
    p.steps_for(0.0, 1000, 1000);
  return p;
}

static bool near(size_t got, size_t want, double tol) {
  double d = double(got) - double(want);
  return d < 0 ? -d <= tol * want : d <= tol * want;
}

int main() {
  {
    Pacer p = calibrated();
    CHECK(near(p.steps_for(1.0 / 60, 0, 0), 800000, 0.01)); // normal_frame
    CHECK(!p.capped());
  }
  {
    Pacer p = calibrated();
    CHECK(near(p.steps_for(5.0, 0, 0), 4800000, 0.01)); // stall_is_capped
    CHECK(p.capped());
  }
  {
    Pacer p;
    CHECK(p.steps_for(0.0, 0, 0) == 1); // zero_elapsed_returns_one
  }
  {
    Pacer p;
    for (int i = 0; i < 200; i++)
      p.steps_for(0.0, 2000, 1000);
    CHECK(near(p.steps_for(1.0 / 60, 0, 0), 400000, 0.01)); // cps_adapts
  }
  {
    Pacer p;
    size_t before = p.steps_for(1.0 / 60, 0, 0);
    p.steps_for(0.0, 12345, 0); // zero_steps_delta_ignored
    CHECK(p.steps_for(1.0 / 60, 0, 0) == before);
  }
  return failures ? 1 : 0;
}
