#pragma once

extern "C" {
#include "DP32G030.h"
}

enum class Key {
  K0, K1, K2, K3, K4, K5, K6, K7, K8, K9,
  Menu, Up, Down, Exit, Star, F, Ptt, Side1, Side2,
  Count
};

// Lowercase name used on the command line: "0".."9", "menu", "up", "down",
// "exit", "star", "f", "ptt", "side1", "side2".
const char *key_name(Key k);
bool key_from_name(const char *name, Key *out);

// Where the key matrix sits on the chip's GPIO. The firmware drives one of
// four "row" outputs low at a time and reads four "column" inputs; a low
// column means the key at (row, column) is pressed. Row r is pin
// `out_first + r * out_step` of `out_port`, column c is pin
// `in_first + c * in_step` of `in_port`. The side keys pull columns 0 and 1
// low whatever the rows do; PTT is its own active-low pin.
struct KeypadWiring {
  unsigned out_port;
  int out_first, out_step;
  unsigned in_port;
  int in_first, in_step;
  unsigned ptt_port;
  unsigned ptt_pin;
};

// UV-K5 V1/V2 (DP32G030): rows PA10..PA13, columns PA3..PA6, PTT on PC5.
extern const KeypadWiring kWiringDp32g030;
// UV-K5 V3 / UV-K1 (PY32F071): rows PB6..PB3, columns PB15..PB12, PTT on PB10.
extern const KeypadWiring kWiringPy32f071;

// The wiring for a chip name ("dp32g030", "py32f071"); the DP32G030 one for
// anything else.
const KeypadWiring &keypad_wiring_for(const char *soc_name);

// Turns a set of pressed keys into the GPIO input levels the firmware samples.
class Keypad {
public:
  explicit Keypad(dp32g030_cpu_t *cpu,
                  const KeypadWiring &wiring = kWiringDp32g030)
      : cpu_(cpu), wiring_(wiring) {}

  void set(Key k, bool pressed);
  bool pressed(Key k) const { return down_[int(k)]; }
  void refresh();
  // For hooks.gpio_change: recompute when the row port changed.
  void on_gpio_change(unsigned port);
  // Call after dp32g030_cpu_run may have replaced the cpu object.
  void set_cpu(dp32g030_cpu_t *cpu) { cpu_ = cpu; }

private:
  dp32g030_cpu_t *cpu_;
  KeypadWiring wiring_;
  bool down_[int(Key::Count)] = {};
  bool refreshing_ = false;
};
