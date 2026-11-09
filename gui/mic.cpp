#include "mic.h"

#include <cstdlib>

bool Mic::open() {
  if (dev_)
    return true;

  SDL_AudioSpec want{}, have{};
  want.freq = kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  want.callback = callback;
  want.userdata = this;

  dev_ = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
  if (!dev_)
    return false;
  SDL_PauseAudioDevice(dev_, 0);
  return true;
}

void Mic::close() {
  if (!dev_)
    return;
  SDL_CloseAudioDevice(dev_);
  dev_ = 0;
  level_.store(0, std::memory_order_relaxed);
}

void Mic::callback(void *userdata, Uint8 *stream, int len) {
  Mic *m = static_cast<Mic *>(userdata);
  const int16_t *in = reinterpret_cast<const int16_t *>(stream);
  int n = len / int(sizeof(int16_t));
  if (n <= 0) {
    m->level_.store(0, std::memory_order_relaxed);
    return;
  }

  uint64_t sum = 0;
  for (int i = 0; i < n; i++)
    sum += uint32_t(std::abs(int(in[i])));
  uint32_t mean = uint32_t(sum / uint64_t(n));
  uint32_t scaled = mean * kGain;
  if (scaled > 0x7FFFu)
    scaled = 0x7FFFu;
  m->level_.store(uint16_t(scaled), std::memory_order_relaxed);
}
