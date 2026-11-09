#pragma once

#include <SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// PC mic → BK4819 REG_64 amplitude for VOX, plus a PCM ring for TX NBFM.
// Capture runs on the SDL audio thread; level()/read() are safe from others.
class Mic {
public:
  Mic() = default;
  Mic(const Mic &) = delete;
  Mic &operator=(const Mic &) = delete;
  ~Mic() { close(); }

  // Opens the default capture device. Returns false on failure (caller may
  // continue without VOX/TX-from-mic).
  bool open();
  void close();

  // Latest mic amplitude, scaled to the BK4819 REG_64 15-bit range (0..0x7FFF).
  uint16_t level() const {
    return level_.load(std::memory_order_relaxed);
  }

  // Copy up to `n` newest-available samples into `dst` (oldest first among
  // those returned). Pads with zeros on underrun. Returns samples produced
  // (always `n`).
  size_t read(int16_t *dst, size_t n);

private:
  static void callback(void *userdata, Uint8 *stream, int len);

  SDL_AudioDeviceID dev_ = 0;
  std::atomic<uint16_t> level_{0};
  std::mutex mu_;
  std::vector<int16_t> ring_;
  size_t head_ = 0;  // next write
  size_t count_ = 0; // samples stored
  static constexpr int kSampleRate = 48000;
  static constexpr size_t kRingSamples = 48000 / 5; // ~200 ms
  // Mean |sample| * gain, clamped. Quiet speech still clears typical VOX
  // calibration thresholds (~hundreds to ~2k on virgin flash).
  static constexpr unsigned kGain = 4;
};
