#include "keypad.h"

#include <cstring>

namespace {

constexpr unsigned kPortA = 0;
constexpr unsigned kPortC = 2;
constexpr unsigned kColShift = 3;       // PA3..PA6 are the four columns
constexpr uint32_t kColMask = 0xFu << kColShift;
constexpr unsigned kRowFirstPin = 10;   // PA10..PA13 are the four rows
constexpr uint32_t kPttBit = 1u << 5;   // PC5

// keyboard[] of the UV-K5 firmware: [row][column].
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
  if (refreshing_ || !wired_)
    return;
  refreshing_ = true;

  const dp32g030_gpio_t &a = cpu_->periph.gpio[kPortA];
  unsigned cols = 0xF;

  for (int r = 0; r < 4; r++) {
    uint32_t pin = 1u << (kRowFirstPin + unsigned(r));
    bool driven_low = (a.dir & pin) && !(a.out & pin);
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

  dp32g030_gpio_set_input(cpu_, kPortA, kColMask, cols << kColShift);
  dp32g030_gpio_set_input(cpu_, kPortC, kPttBit,
                          down_[int(Key::Ptt)] ? 0 : kPttBit);
  refreshing_ = false;
}

void Keypad::on_gpio_change(unsigned port) {
  if (port == kPortA)
    refresh();
}
