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

// Turns a set of pressed keys into the GPIO input levels the UV-K5 firmware
// samples: columns PA3..PA6 (low when a pressed key's row is driven low, or
// for the side keys always) and PC5 for PTT.
class Keypad {
public:
  explicit Keypad(dp32g030_cpu_t *cpu) : cpu_(cpu) {}

  void set(Key k, bool pressed);
  bool pressed(Key k) const { return down_[int(k)]; }
  void refresh();
  // For hooks.gpio_change: recompute when GPIOA changed.
  void on_gpio_change(unsigned port);
  // Call after dp32g030_cpu_run may have replaced the cpu object.
  void set_cpu(dp32g030_cpu_t *cpu) { cpu_ = cpu; }

private:
  dp32g030_cpu_t *cpu_;
  bool down_[int(Key::Count)] = {};
  bool refreshing_ = false;
};
