#include "mic.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

bool Mic::open() {
  if (dev_)
    return true;

  ring_.assign(kRingSamples, 0);
  head_ = 0;
  count_ = 0;

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
  std::lock_guard<std::mutex> lock(mu_);
  head_ = 0;
  count_ = 0;
}

size_t Mic::read(int16_t *dst, size_t n) {
  if (!dst || n == 0)
    return 0;
  std::lock_guard<std::mutex> lock(mu_);
  size_t take = std::min(n, count_);
  size_t start =
      (head_ + kRingSamples - count_) % kRingSamples; // oldest stored
  for (size_t i = 0; i < take; i++)
    dst[i] = ring_[(start + i) % kRingSamples];
  count_ -= take;
  for (size_t i = take; i < n; i++)
    dst[i] = 0;
  return n;
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

  std::lock_guard<std::mutex> lock(m->mu_);
  for (int i = 0; i < n; i++) {
    m->ring_[m->head_] = in[i];
    m->head_ = (m->head_ + 1) % kRingSamples;
    if (m->count_ < kRingSamples)
      m->count_++;
  }
}
