#include "pacer.h"

#include <algorithm>

namespace {
constexpr double kEmaWeight = 0.2;
}

Pacer::Pacer(double cpu_hz, double max_frame_s)
    : cpu_hz_(cpu_hz), max_frame_s_(max_frame_s) {}

size_t Pacer::steps_for(double elapsed_s, uint64_t cycles_delta,
                        size_t steps_delta) {
  if (steps_delta > 0) {
    double sample = double(cycles_delta) / double(steps_delta);
    if (sample > 0.0)
      cycles_per_step_ += kEmaWeight * (sample - cycles_per_step_);
  }

  double max_steps = max_frame_s_ * cpu_hz_ / cycles_per_step_;
  double want = std::max(elapsed_s, 0.0) * cpu_hz_ / cycles_per_step_;
  capped_ = want > max_steps;
  return std::max<size_t>(1, size_t(std::min(want, max_steps)));
}
