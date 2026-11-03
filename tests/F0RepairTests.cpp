// Octave-error repair in F0Smoother.
//
// On rasp the detector reported a ~100 Hz note as 54 Hz for a few frames and
// the vocoder sang that stretch an octave down. Short octave runs must fold
// back; a sung octave leap must not.

#include "../Source/Utils/F0Smoother.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool near(float a, float b) { return std::abs(a - b) < 0.5f; }

void shortSubharmonicRunFoldsUp() {
  std::vector<float> f0(40, 100.0f);
  for (int i = 18; i < 21; ++i)
    f0[static_cast<size_t>(i)] = 50.5f;
  const auto fixed = F0Smoother::repairOctaveErrors(f0);
  for (int i = 18; i < 21; ++i)
    CHECK(near(fixed[static_cast<size_t>(i)], 101.0f));
  CHECK(near(fixed[10], 100.0f));
}

void shortDoubledRunFoldsDown() {
  std::vector<float> f0(40, 100.0f);
  f0[25] = 199.0f;
  f0[26] = 201.0f;
  const auto fixed = F0Smoother::repairOctaveErrors(f0);
  CHECK(near(fixed[25], 99.5f));
  CHECK(near(fixed[26], 100.5f));
}

void realOctaveLeapStays() {
  // 30 frames at 100 Hz, then 30 at 200 Hz: a sung leap.
  std::vector<float> f0(60, 100.0f);
  for (int i = 30; i < 60; ++i)
    f0[static_cast<size_t>(i)] = 200.0f;
  const auto fixed = F0Smoother::repairOctaveErrors(f0);
  for (int i = 0; i < 60; ++i)
    CHECK(near(fixed[static_cast<size_t>(i)], f0[static_cast<size_t>(i)]));
}

void unvoicedAndOrdinaryMovementUntouched() {
  std::vector<float> f0(40, 0.0f);
  for (int i = 5; i < 35; ++i) // a slide of 5 semitones
    f0[static_cast<size_t>(i)] =
        100.0f * std::pow(2.0f, (i - 5) / 72.0f);
  const auto fixed = F0Smoother::repairOctaveErrors(f0);
  for (int i = 0; i < 40; ++i)
    CHECK(near(fixed[static_cast<size_t>(i)], f0[static_cast<size_t>(i)]));
}

void removeOutliersAppliesTheRepair() {
  std::vector<float> f0(40, 100.0f);
  f0[20] = 50.0f;
  f0[21] = 50.0f;
  const auto cleaned = F0Smoother::removeOutliers(f0, 1.5f);
  CHECK(near(cleaned[20], 100.0f));
  CHECK(near(cleaned[21], 100.0f));
}

void goodFrameAfterOutlierIsKept() {
  // One bad frame, then good ones: only the bad frame may change. (A value
  // within 3 semitones of an octave would be folded by the octave pass
  // instead, so use one well away from it.)
  std::vector<float> f0(40, 100.0f);
  f0[20] = 160.0f;
  const auto cleaned = F0Smoother::removeOutliers(f0, 1.5f);
  CHECK(near(cleaned[20], 100.0f));
  CHECK(near(cleaned[21], 100.0f));
  CHECK(near(cleaned[22], 100.0f));
}

} // namespace

int main() {
  goodFrameAfterOutlierIsKept();
  shortSubharmonicRunFoldsUp();
  shortDoubledRunFoldsDown();
  realOctaveLeapStays();
  unvoicedAndOrdinaryMovementUntouched();
  removeOutliersAppliesTheRepair();
  std::printf("F0RepairTests passed\n");
  return 0;
}
