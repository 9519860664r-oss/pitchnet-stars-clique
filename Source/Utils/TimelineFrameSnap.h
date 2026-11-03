#pragma once

#include "Constants.h"

#include <algorithm>
#include <cmath>

namespace TimelineFrameSnap {

struct Result {
  double offsetSeconds = 0.0; // on the HOP_SIZE frame grid, <= the request
  int leadSamples = 0;        // silence to prepend, at the capture's rate
};

// A capture is placed on the timeline twice: its waveform by exact samples,
// its per-frame data (mel, F0, notes) by whole HOP_SIZE frames. Unless the
// offset lands on a frame, the two disagree by up to half a hop and the
// resynthesised audio plays off the original. Snap the offset down to the
// frame grid and return the remainder as leading silence for the capture, so
// the audio stays where it was and both placements agree.
inline Result snapToFrameGrid(double offsetSeconds, double captureSampleRate) {
  const double framesPerSecond =
      static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
  const double clamped = std::max(0.0, offsetSeconds);
  Result result;
  result.offsetSeconds =
      std::floor(clamped * framesPerSecond + 1e-6) / framesPerSecond;
  result.leadSamples = std::max(
      0, static_cast<int>(std::llround((clamped - result.offsetSeconds) *
                                       captureSampleRate)));
  return result;
}

} // namespace TimelineFrameSnap
