#pragma once

#include <SDL.h>

#include <atomic>
#include <cstdint>

// Host-side UI beep: SDL sine oscillator driven by BK4819 tone Hz.
class Beeper {
public:
  Beeper() = default;
  Beeper(const Beeper &) = delete;
  Beeper &operator=(const Beeper &) = delete;
  ~Beeper() { close(); }

  // Opens an SDL audio device. Returns false on failure (caller may continue
  // silently). No-op if already open.
  bool open();
  void close();

  // 0 = silence. Safe to call from the emulator thread while the callback
  // runs on the audio thread.
  void set_hz(unsigned hz);

private:
  static void callback(void *userdata, Uint8 *stream, int len);

  SDL_AudioDeviceID dev_ = 0;
  std::atomic<unsigned> hz_{0};
  double phase_ = 0.0; // audio thread only
  static constexpr int kSampleRate = 48000;
  static constexpr int16_t kAmplitude = 6554; // ~0.2 * 32767
};
