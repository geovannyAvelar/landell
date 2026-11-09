#pragma once

#include <SDL.h>

#include <atomic>
#include <cstdint>

// PC mic → BK4819 REG_64 amplitude for VOX. Capture runs on the SDL audio
// thread; level() is safe from the emulator thread.
class Mic {
public:
  Mic() = default;
  Mic(const Mic &) = delete;
  Mic &operator=(const Mic &) = delete;
  ~Mic() { close(); }

  // Opens the default capture device. Returns false on failure (caller may
  // continue without VOX-from-mic).
  bool open();
  void close();

  // Latest mic amplitude, scaled to the BK4819 REG_64 15-bit range (0..0x7FFF).
  uint16_t level() const {
    return level_.load(std::memory_order_relaxed);
  }

private:
  static void callback(void *userdata, Uint8 *stream, int len);

  SDL_AudioDeviceID dev_ = 0;
  std::atomic<uint16_t> level_{0};
  static constexpr int kSampleRate = 48000;
  // Mean |sample| * gain, clamped. Quiet speech still clears typical VOX
  // calibration thresholds (~hundreds to ~2k on virgin flash).
  static constexpr unsigned kGain = 4;
};
