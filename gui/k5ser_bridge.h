#pragma once

#include <cstddef>

extern "C" {
#include "DP32G030.h"
}

// Bridges a Linux PTY to the emulated programming UART (DP32 UART1 or
// PY32 USART1). Chirp opens the slave path printed at start-up.
struct K5serBridge {
  int master = -1;
  int slave_keep = -1; // held open so the master does not HUP early
  char path[64]{};
  unsigned hook_uart = 1; // hooks.uart_tx index (1 = UART1 / USART1)
  bool py32 = false;
  dp32g030_cpu_t **cpup = nullptr;

  bool open(bool is_py32);
  void close();
  void on_tx(unsigned uart, uint8_t byte);
  void poll_rx();
};
