#include "keypad.h"

extern "C" {
#include "bus.h"
}

#include <cstdint>
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

namespace {

constexpr uint32_t kRowPins[4] = {10, 11, 12, 13};
constexpr uint32_t kRowMask = (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13);
constexpr uint32_t kGpioaData = 0x40060000;
constexpr uint32_t kGpioaDir = 0x40060004;

// Row r, column c as in the firmware's keyboard[] table.
const Key kMatrix[4][4] = {
    {Key::Menu, Key::K1, Key::K4, Key::K7},
    {Key::Up, Key::K2, Key::K5, Key::K8},
    {Key::Down, Key::K3, Key::K6, Key::K9},
    {Key::Exit, Key::Star, Key::K0, Key::F},
};

dp32g030_cpu_t *make_cpu() {
  const uint8_t image[8] = {0x00, 0x40, 0x00, 0x20, 0x09, 0x00, 0x00, 0x00};
  return dp32g030_cpu_new(image, sizeof image);
}

unsigned cols(dp32g030_cpu_t *cpu) {
  return (dp32g030_gpio_pins(cpu, 0) >> 3) & 0xF;
}

// Rows are outputs; only row `low` (or none if -1) is driven low.
void drive_rows(dp32g030_cpu_t *cpu, int low) {
  uint32_t out = kRowMask;
  if (low >= 0)
    out &= ~(1u << kRowPins[low]);
  cpu->periph.gpio[0].dir |= kRowMask;
  cpu->periph.gpio[0].out = (cpu->periph.gpio[0].out & ~kRowMask) | out;
}

} // namespace

int main() {
  dp32g030_cpu_t *cpu = make_cpu();
  if (!cpu)
    return 1;
  Keypad pad(cpu);

  // initial_refresh_drives_high: no pull-up configured, nothing pressed
  pad.refresh();
  CHECK(cols(cpu) == 0xF);
  CHECK((dp32g030_gpio_pins(cpu, 2) & (1u << 5)) != 0);

  // each_matrix_position
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      Key k = kMatrix[r][c];
      drive_rows(cpu, r);
      pad.set(k, true);
      CHECK(cols(cpu) == (0xFu & ~(1u << c)));
      drive_rows(cpu, (r + 1) % 4); // row_high_no_pull
      pad.refresh();
      CHECK(cols(cpu) == 0xF);
      drive_rows(cpu, -1);
      pad.refresh();
      CHECK(cols(cpu) == 0xF);
      drive_rows(cpu, r);
      pad.set(k, false);
      CHECK(cols(cpu) == 0xF);
    }
  }

  { // row_as_input_no_pull: row PA10 is an input, MENU pressed
    drive_rows(cpu, 0);
    cpu->periph.gpio[0].dir &= ~(1u << 10);
    pad.set(Key::Menu, true);
    CHECK(cols(cpu) == 0xF);
    pad.set(Key::Menu, false);
  }

  { // side1_pa3 / side2_pa4: all rows high
    drive_rows(cpu, -1);
    pad.set(Key::Side1, true);
    CHECK(cols(cpu) == 0xE);
    pad.set(Key::Side2, true);
    CHECK(cols(cpu) == 0xC);
    pad.set(Key::Side1, false);
    CHECK(cols(cpu) == 0xD);
    pad.set(Key::Side2, false);
    CHECK(cols(cpu) == 0xF);
  }

  { // ptt_pc5
    pad.set(Key::Ptt, true);
    CHECK((dp32g030_gpio_pins(cpu, 2) & (1u << 5)) == 0);
    pad.set(Key::Ptt, false);
    CHECK((dp32g030_gpio_pins(cpu, 2) & (1u << 5)) != 0);
  }

  { // two_keys_one_row: MENU (col 0) and 4 (col 2) on row PA10
    drive_rows(cpu, 0);
    pad.set(Key::Menu, true);
    pad.set(Key::K4, true);
    CHECK(cols(cpu) == 0xA);
    pad.set(Key::Menu, false);
    pad.set(Key::K4, false);
  }

  { // key names round trip
    for (int i = 0; i < int(Key::Count); i++) {
      Key out;
      CHECK(key_from_name(key_name(Key(i)), &out) && out == Key(i));
    }
    Key out;
    CHECK(!key_from_name("bogus", &out));
  }

  // hook_recompute / no_reentrancy_loop: firmware writes rows through the bus
  cpu->periph.hooks.user = &pad;
  cpu->periph.hooks.gpio_change = [](void *u, unsigned port, uint32_t, uint32_t) {
    static_cast<Keypad *>(u)->on_gpio_change(port);
  };
  cpu->periph.gpio[0].dir = 0;
  cpu->periph.gpio[0].out = 0xFFFF;
  pad.refresh();
  pad.set(Key::Menu, true);
  dp32g030_bus_write(cpu, kGpioaDir, 4, kRowMask);
  dp32g030_bus_write(cpu, kGpioaData, 4, 0xFFFF);
  CHECK(cols(cpu) == 0xF);
  dp32g030_bus_write(cpu, kGpioaData, 4, 0xFFFF & ~(1u << 10));
  CHECK(cols(cpu) == 0xE);
  for (int i = 0; i < 1000; i++) {
    dp32g030_bus_write(cpu, kGpioaData, 4, (i & 1) ? 0xFFFF : 0xFFFF & ~(1u << 10));
    CHECK(cols(cpu) == ((i & 1) ? 0xFu : 0xEu));
  }

  { // PY32F071 board wiring: rows PB6..PB3 (outputs), columns PB15..PB12, PTT PB10
    const dp32g030_soc_t *soc = dp32g030_soc_by_name("py32f071");
    const uint8_t app[16] = {0x00, 0x40, 0x00, 0x20, 0x09, 0x28, 0x00, 0x08};
    dp32g030_cpu_t *py = dp32g030_cpu_new_soc(soc, app, sizeof app);
    CHECK(py != nullptr);
    if (py) {
      constexpr uint32_t kGpiob = 0x50000400, kModer = 0x00, kBsrr = 0x18;
      constexpr unsigned kPortB = 1;
      auto set_pin = [&](unsigned pin, bool high) {
        dp32g030_bus_write(py, kGpiob + kBsrr, 4, high ? 1u << pin : 1u << (16 + pin));
      };
      auto cols_py = [&]() { return (dp32g030_gpio_pins(py, kPortB) >> 12) & 0xFu; };
      // rows PB6, PB5, PB4, PB3 as outputs, all high
      uint32_t moder = 0;
      for (unsigned p = 3; p <= 6; p++)
        moder |= 1u << (2 * p);
      dp32g030_bus_write(py, kGpiob + kModer, 4, moder);
      auto drive_py = [&](int low) {
        for (int r = 0; r < 4; r++)
          set_pin(6 - unsigned(r), r != low);
      };
      Keypad pp(py, keypad_wiring_for("py32f071"));
      pp.refresh();
      drive_py(-1);
      CHECK(cols_py() == 0xF); // initial_refresh_drives_high

      for (int r = 0; r < 4; r++) { // each_matrix_position_py32
        for (int c = 0; c < 4; c++) {
          Key k = kMatrix[r][c];
          drive_py(r);
          pp.set(k, true);
          // column c is the c-th input, PB(15 - c): bit (3 - c) of the nibble
          CHECK(cols_py() == (0xFu & ~(1u << (3 - c))));
          drive_py((r + 1) % 4);
          pp.refresh();
          CHECK(cols_py() == 0xF); // another row selected: no pull
          drive_py(r);
          pp.set(k, false);
          CHECK(cols_py() == 0xF);
        }
      }
      drive_py(-1);
      pp.set(Key::Side1, true); // side1_pb15 / side2_pb14 with every row high
      CHECK(cols_py() == 0x7);
      pp.set(Key::Side2, true);
      CHECK(cols_py() == 0x3);
      pp.set(Key::Side1, false);
      pp.set(Key::Side2, false);
      CHECK(cols_py() == 0xF);

      pp.set(Key::Ptt, true); // ptt_pb10
      CHECK((dp32g030_gpio_pins(py, kPortB) & (1u << 10)) == 0);
      pp.set(Key::Ptt, false);
      CHECK((dp32g030_gpio_pins(py, kPortB) & (1u << 10)) != 0);

      // a row configured as an input never pulls a column low
      drive_py(0);
      dp32g030_bus_write(py, kGpiob + kModer, 4, moder & ~(1u << (2 * 6)));
      pp.set(Key::Menu, true);
      CHECK(cols_py() == 0xF);
      pp.set(Key::Menu, false);
      dp32g030_bus_write(py, kGpiob + kModer, 4, moder);

      // hook_recompute_py32: the firmware drives a row through the bus
      py->periph.hooks.user = &pp;
      py->periph.hooks.gpio_change = [](void *u, unsigned port, uint32_t, uint32_t) {
        static_cast<Keypad *>(u)->on_gpio_change(port);
      };
      drive_py(-1);
      pp.set(Key::K5, true); // group 2 (PB5), third column (PB13)
      CHECK(cols_py() == 0xF);
      set_pin(5, false); // select PB5
      CHECK(cols_py() == (0xFu & ~(1u << 1)));
      set_pin(5, true);
      CHECK(cols_py() == 0xF);
      pp.set(Key::K5, false);
      dp32g030_cpu_free(py);
    }
  }

  dp32g030_cpu_free(cpu);
  return failures ? 1 : 0;
}
