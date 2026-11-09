#pragma once

#include "mic.h"
#include "nbfm.h"

#include <atomic>
#include <cstdint>
#include <thread>

// Minimal rtl_tcp server: streams unsigned-8 IQ so GQRX can listen to the
// emulator's NBFM TX. Wall-clock realtime; TX gate follows BK4819 state.
class RtlTcp {
public:
  RtlTcp() = default;
  RtlTcp(const RtlTcp &) = delete;
  RtlTcp &operator=(const RtlTcp &) = delete;
  ~RtlTcp() { stop(); }

  // Bind/listen on TCP `port`. `mic` may be null (carrier-only / silence).
  bool start(uint16_t port, Mic *mic);
  void stop();

  void set_tx(bool active, uint32_t hz);
  void set_muted(bool muted);

private:
  void thread_main();
  bool serve_client(int client);

  Mic *mic_ = nullptr;
  std::atomic<bool> run_{false};
  std::atomic<bool> tx_active_{false};
  std::atomic<bool> tx_muted_{false};
  std::atomic<uint32_t> tx_hz_{0};
  std::atomic<uint32_t> center_hz_{100000000};
  std::atomic<uint32_t> samp_rate_{1024000};
  int listen_fd_ = -1;
  std::thread thr_;
  static constexpr uint32_t kMaxSampRate = 1024000;
};
