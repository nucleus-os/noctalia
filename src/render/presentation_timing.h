#pragma once

#include <cstdint>
#include <functional>

// Realized compositor feedback for one wl_surface commit. Timestamps use the
// process steady-clock domain so renderer clients can compare them directly
// with input, paint, and scheduling timestamps.
struct SurfacePresentationFeedback {
  std::int64_t presentedSteadyNs = 0;
  std::int64_t callbackSteadyNs = 0;
  std::uint64_t sequence = 0;
  std::uint32_t refreshNs = 0;
  std::uint32_t flags = 0;
  bool presented = false;
  bool exactClock = false;
};

using SurfacePresentationCallback = std::function<void(const SurfacePresentationFeedback&)>;
