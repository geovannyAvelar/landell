#pragma once

#include <cmath>
#include <cstdint>

// Narrowband FM: integrate mic PCM into RF phase, then mix to complex baseband
// relative to an SDR center frequency.
struct Nbfm {
  static constexpr double kDeviationHz = 2500.0;
  static constexpr double kMicRate = 48000.0;
  static constexpr double kTwoPi = 6.28318530717958647692;

  double phase = 0.0;       // radians at RF offset
  double audio_acc = 0.0;   // advances at mic_rate / iq_rate
  int16_t pcm = 0;          // last mic sample
  uint32_t noise = 1;       // LCG for idle floor

  // Emit one unsigned-8 IQ pair (I then Q), centered at 127.
  // `take_pcm` is called when a new mic sample is needed.
  template <typename TakePcm>
  void sample(uint8_t *iq, bool active, bool muted, double tx_hz,
              double center_hz, double iq_rate, TakePcm take_pcm) {
    audio_acc += kMicRate / iq_rate;
    while (audio_acc >= 1.0) {
      take_pcm(&pcm);
      audio_acc -= 1.0;
    }

    if (!active) {
      phase = 0.0;
      noise = noise * 1664525u + 1013904223u;
      iq[0] = uint8_t(127 + int((noise >> 24) & 7) - 3);
      iq[1] = uint8_t(127 + int((noise >> 16) & 7) - 3);
      return;
    }

    double offset = tx_hz - center_hz;
    double dev = 0.0;
    if (!muted)
      dev = kDeviationHz * (double(pcm) / 32768.0);
    phase += kTwoPi * (offset + dev) / iq_rate;
    if (phase > kTwoPi)
      phase -= kTwoPi * std::floor(phase / kTwoPi);
    else if (phase < 0.0)
      phase += kTwoPi * std::ceil(-phase / kTwoPi);

    constexpr double amp = 100.0; // strong enough for GQRX waterfall
    int i = int(127.5 + amp * std::cos(phase));
    int q = int(127.5 + amp * std::sin(phase));
    if (i < 0)
      i = 0;
    if (i > 255)
      i = 255;
    if (q < 0)
      q = 0;
    if (q > 255)
      q = 255;
    iq[0] = uint8_t(i);
    iq[1] = uint8_t(q);
  }
};
