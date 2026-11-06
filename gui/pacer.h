#pragma once

#include <cstddef>
#include <cstdint>

// Converts wall-clock time into an emulator step budget.
class Pacer {
public:
  explicit Pacer(double cpu_hz = 48e6, double max_frame_s = 0.1);

  // Steps to run for `elapsed_s` of wall time. The last slice's
  // (cycles_delta, steps_delta) refines the cycles-per-step estimate.
  // Result is in [1, max_frame_s * cpu_hz / cycles_per_step].
  size_t steps_for(double elapsed_s, uint64_t cycles_delta, size_t steps_delta);

  // True when the last call hit the per-frame cap.
  bool capped() const { return capped_; }

private:
  double cpu_hz_;
  double max_frame_s_;
  double cycles_per_step_ = 1.5;
  bool capped_ = false;
};
