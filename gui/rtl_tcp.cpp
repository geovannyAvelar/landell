#include "rtl_tcp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

uint32_t be32(uint32_t v) { return htonl(v); }

ssize_t write_all(int fd, const void *data, size_t n) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (w == 0)
      return -1;
    off += size_t(w);
  }
  return ssize_t(n);
}

} // namespace

bool RtlTcp::start(uint16_t port, Mic *mic) {
  if (run_.load())
    return true;
  mic_ = mic;

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0)
    return false;
  int yes = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0 ||
      listen(listen_fd_, 1) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  run_.store(true);
  thr_ = std::thread([this] { thread_main(); });
  std::fprintf(stderr, "rtl_tcp: listening on port %u (GQRX: RTL-SDR TCP)\n",
               unsigned(port));
  return true;
}

void RtlTcp::stop() {
  if (!run_.exchange(false))
    return;
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (thr_.joinable())
    thr_.join();
  mic_ = nullptr;
}

void RtlTcp::set_tx(bool active, uint32_t hz) {
  tx_active_.store(active, std::memory_order_relaxed);
  tx_hz_.store(hz, std::memory_order_relaxed);
}

void RtlTcp::set_muted(bool muted) {
  tx_muted_.store(muted, std::memory_order_relaxed);
}

void RtlTcp::thread_main() {
  while (run_.load(std::memory_order_relaxed)) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd_, &rfds);
    timeval tv{1, 0};
    int n = select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv);
    if (n <= 0 || !FD_ISSET(listen_fd_, &rfds))
      continue;
    int client = accept(listen_fd_, nullptr, nullptr);
    if (client < 0)
      continue;
    std::fprintf(stderr, "rtl_tcp: client connected\n");
    serve_client(client);
    ::close(client);
    std::fprintf(stderr, "rtl_tcp: client gone\n");
  }
}

bool RtlTcp::serve_client(int client) {
  // Dongle info header (osmocom rtl_tcp).
  struct {
    char magic[4];
    uint32_t tuner_type;
    uint32_t tuner_gain_count;
  } info = {{'R', 'T', 'L', '0'}, be32(1) /* R820T */, be32(0)};
  if (write_all(client, &info, sizeof info) < 0)
    return false;

  Nbfm fm;
  std::vector<uint8_t> buf(16384); // 8192 IQ pairs
  auto next = std::chrono::steady_clock::now();

  while (run_.load(std::memory_order_relaxed)) {
    // Drain rtl_tcp commands (5-byte: cmd + BE32 arg).
    for (;;) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(client, &rfds);
      timeval tv{0, 0};
      if (select(client + 1, &rfds, nullptr, nullptr, &tv) <= 0)
        break;
      uint8_t cmd[5];
      ssize_t r = ::recv(client, cmd, 5, MSG_WAITALL);
      if (r == 0)
        return true; // hangup
      if (r < 0) {
        if (errno == EINTR)
          continue;
        return false;
      }
      if (r != 5)
        return false;
      uint32_t arg = (uint32_t(cmd[1]) << 24) | (uint32_t(cmd[2]) << 16) |
                     (uint32_t(cmd[3]) << 8) | uint32_t(cmd[4]);
      switch (cmd[0]) {
      case 0x01: // SET_FREQ
        center_hz_.store(arg, std::memory_order_relaxed);
        break;
      case 0x02: { // SET_SAMPLE_RATE
        uint32_t rate = arg ? arg : 1024000;
        if (rate > kMaxSampRate)
          rate = kMaxSampRate;
        if (rate < 225001)
          rate = 225001;
        samp_rate_.store(rate, std::memory_order_relaxed);
        break;
      }
      default:
        break; // gains etc. ignored
      }
    }

    uint32_t rate = samp_rate_.load(std::memory_order_relaxed);
    bool active = tx_active_.load(std::memory_order_relaxed);
    bool muted = tx_muted_.load(std::memory_order_relaxed);
    double tx = double(tx_hz_.load(std::memory_order_relaxed));
    double center = double(center_hz_.load(std::memory_order_relaxed));
    double iq_rate = double(rate);

    size_t pairs = buf.size() / 2;
    int16_t scratch = 0;
    auto take = [&](int16_t *out) {
      if (mic_) {
        mic_->read(out, 1);
      } else {
        *out = 0;
      }
      scratch = *out;
      (void)scratch;
    };

    for (size_t i = 0; i < pairs; i++)
      fm.sample(&buf[i * 2], active, muted, tx, center, iq_rate, take);

    if (write_all(client, buf.data(), buf.size()) < 0)
      return false;

    // Pace to wall clock.
    double chunk_s = double(pairs) / iq_rate;
    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(chunk_s));
    auto now = std::chrono::steady_clock::now();
    if (next > now)
      std::this_thread::sleep_until(next);
    else
      next = now; // fell behind; resync
  }
  return true;
}
