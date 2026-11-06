#include "keypad.h"

#include <cstring>

namespace {

// keyboard[] of the UV-K5 firmwares: [row][column]. The same logical matrix on
// every chip; only the pins differ.
constexpr Key kMatrix[4][4] = {
    {Key::Menu, Key::K1, Key::K4, Key::K7},
    {Key::Up, Key::K2, Key::K5, Key::K8},
    {Key::Down, Key::K3, Key::K6, Key::K9},
    {Key::Exit, Key::Star, Key::K0, Key::F},
};

constexpr const char *kNames[int(Key::Count)] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "menu", "up", "down", "exit", "star", "f", "ptt", "side1", "side2",
};

} // namespace

const KeypadWiring kWiringDp32g030 = {0, 10, 1, 0, 3, 1, 2, 5};
const KeypadWiring kWiringPy32f071 = {1, 6, -1, 1, 15, -1, 1, 10};

const KeypadWiring &keypad_wiring_for(const char *soc_name) {
  return std::strcmp(soc_name, "py32f071") == 0 ? kWiringPy32f071
                                                : kWiringDp32g030;
}

const char *key_name(Key k) { return kNames[int(k)]; }

bool key_from_name(const char *name, Key *out) {
  for (int i = 0; i < int(Key::Count); i++) {
    if (!std::strcmp(name, kNames[i])) {
      *out = Key(i);
      return true;
    }
  }
  return false;
}

void Keypad::set(Key k, bool pressed) {
  down_[int(k)] = pressed;
  refresh();
}

void Keypad::refresh() {
  if (refreshing_)
    return;
  refreshing_ = true;

  const KeypadWiring &w = wiring_;
  uint32_t level = dp32g030_gpio_pins(cpu_, w.out_port);
  uint32_t dir = dp32g030_gpio_dir(cpu_, w.out_port);
  unsigned cols = 0xF; // bit c = column c reads high

  for (int r = 0; r < 4; r++) {
    uint32_t pin = 1u << (w.out_first + r * w.out_step);
    bool driven_low = (dir & pin) && !(level & pin);
    if (!driven_low)
      continue;
    for (int c = 0; c < 4; c++)
      if (down_[int(kMatrix[r][c])])
        cols &= ~(1u << c);
  }
  if (down_[int(Key::Side1)])
    cols &= ~1u;
  if (down_[int(Key::Side2)])
    cols &= ~2u;

  uint32_t mask = 0, value = 0;
  for (int c = 0; c < 4; c++) {
    uint32_t pin = 1u << (w.in_first + c * w.in_step);
    mask |= pin;
    if (cols & (1u << c))
      value |= pin;
  }
  dp32g030_gpio_set_input(cpu_, w.in_port, mask, value);
  uint32_t ptt = 1u << w.ptt_pin;
  dp32g030_gpio_set_input(cpu_, w.ptt_port, ptt,
                          down_[int(Key::Ptt)] ? 0 : ptt);
  refreshing_ = false;
}

void Keypad::on_gpio_change(unsigned port) {
  if (port == wiring_.out_port)
    refresh();
}
