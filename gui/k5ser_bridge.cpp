#include "k5ser_bridge.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <unistd.h>

#include <termios.h>

namespace {

void raw_tty(int fd) {
  termios attrs{};
  if (tcgetattr(fd, &attrs) != 0)
    return;
  attrs.c_lflag &= ~(ECHO | ICANON | ISIG);
  attrs.c_iflag &= ~(IXON | IXOFF | INLCR | ICRNL);
  attrs.c_oflag &= ~(OPOST);
  tcsetattr(fd, TCSANOW, &attrs);
}

} // namespace

bool K5serBridge::open(bool is_py32) {
  py32 = is_py32;
  hook_uart = 1;
  int slave = -1;
  if (openpty(&master, &slave, path, nullptr, nullptr) != 0) {
    std::perror("k5ser: openpty");
    master = -1;
    return false;
  }
  raw_tty(slave);
  raw_tty(master);
  int flags = fcntl(master, F_GETFL, 0);
  if (flags >= 0)
    fcntl(master, F_SETFL, flags | O_NONBLOCK);
  slave_keep = slave;
  std::fprintf(stderr, "k5ser: Chirp port %s (38400, %s)\n", path,
               py32 ? "py32 USART1" : "dp32 UART1");
  std::printf("%s\n", path);
  std::fflush(stdout);
  return true;
}

void K5serBridge::close() {
  if (master >= 0) {
    ::close(master);
    master = -1;
  }
  if (slave_keep >= 0) {
    ::close(slave_keep);
    slave_keep = -1;
  }
}

void K5serBridge::on_tx(unsigned uart, uint8_t byte) {
  if (master < 0 || uart != hook_uart)
    return;
  if (write(master, &byte, 1) < 0 && errno != EAGAIN)
    std::perror("k5ser: write");
}

void K5serBridge::poll_rx() {
  if (master < 0 || !cpup || !*cpup)
    return;
  uint8_t buf[256];
  for (;;) {
    ssize_t n = read(master, buf, sizeof buf);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      break;
    }
    if (n == 0)
      break;
    dp32g030_cpu_t *cpu = *cpup;
    for (ssize_t i = 0; i < n; i++) {
      if (py32)
        py32_usart_rx(cpu, 0, buf[i]); // USART1
      else
        dp32g030_uart_rx(cpu, hook_uart, buf[i]); // UART1
    }
  }
}
