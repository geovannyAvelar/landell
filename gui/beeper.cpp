#include "beeper.h"

#include <cmath>
#include <cstring>

bool Beeper::open() {
  if (dev_)
    return true;

  SDL_AudioSpec want{}, have{};
  want.freq = kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  want.callback = callback;
  want.userdata = this;

  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (!dev_)
    return false;
  SDL_PauseAudioDevice(dev_, 0);
  return true;
}

void Beeper::close() {
  if (!dev_)
    return;
  SDL_CloseAudioDevice(dev_);
  dev_ = 0;
  hz_.store(0, std::memory_order_relaxed);
  phase_ = 0.0;
}

void Beeper::set_hz(unsigned hz) {
  hz_.store(hz, std::memory_order_relaxed);
}

void Beeper::callback(void *userdata, Uint8 *stream, int len) {
  Beeper *b = static_cast<Beeper *>(userdata);
  int16_t *out = reinterpret_cast<int16_t *>(stream);
  int n = len / int(sizeof(int16_t));
  unsigned hz = b->hz_.load(std::memory_order_relaxed);

  if (hz == 0) {
    std::memset(stream, 0, size_t(len));
    b->phase_ = 0.0;
    return;
  }

  const double step = 2.0 * M_PI * double(hz) / double(kSampleRate);
  for (int i = 0; i < n; i++) {
    out[i] = int16_t(std::sin(b->phase_) * kAmplitude);
    b->phase_ += step;
    if (b->phase_ >= 2.0 * M_PI)
      b->phase_ -= 2.0 * M_PI;
  }
}
