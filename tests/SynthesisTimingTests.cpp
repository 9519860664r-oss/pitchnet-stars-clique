// Timing of the analysis frame grid against the waveform.
//
// Resynthesised audio played ~11 ms late in FL Studio: ~5 ms from the mel
// framing (frame i described audio at i * HOP_SIZE, while the vocoder writes
// frame i at i * HOP_SIZE + HOP_SIZE / 2) and up to ~5.8 ms from placing a
// capture's waveform by samples but its frames by rounded frames. These tests
// pin both down.

#include "../Source/Utils/Constants.h"
#include "../Source/Utils/MelSpectrogram.h"
#include "../Source/Utils/TimelineFrameSnap.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// Index of the mel frame holding the most energy for a single click.
int loudestFrameForClick(int clickSample) {
  const int numSamples = HOP_SIZE * 40;
  std::vector<float> audio(static_cast<size_t>(numSamples), 0.0f);
  audio[static_cast<size_t>(clickSample)] = 1.0f;

  MelSpectrogram mel(SAMPLE_RATE, N_FFT, HOP_SIZE, NUM_MELS, FMIN, FMAX);
  const auto frames = mel.compute(audio.data(), numSamples);
  int best = -1;
  double bestEnergy = -1e30;
  for (size_t i = 0; i < frames.size(); ++i) {
    double energy = 0.0;
    for (float v : frames[i])
      energy += std::exp(static_cast<double>(v));
    if (energy > bestEnergy) {
      bestEnergy = energy;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void melFrameIsCentredOnItsVocoderOutput() {
  // Frame i must describe the audio the vocoder writes for it,
  // [i * HOP, (i + 1) * HOP), centred at i * HOP + HOP / 2. A click 400
  // samples into frame 20's span belongs to frame 20; the old centred
  // framing (frame i centred at i * HOP) gave it to frame 21.
  const int frame = 20;
  CHECK(loudestFrameForClick(frame * HOP_SIZE + 400) == frame);
  CHECK(loudestFrameForClick(frame * HOP_SIZE + HOP_SIZE / 2) == frame);
  CHECK(loudestFrameForClick(frame * HOP_SIZE + 100) == frame);
}

void melFrameCountIsUnchanged() {
  // Every per-frame array is sized from the mel; the framing fix must not
  // change how many frames there are.
  MelSpectrogram mel(SAMPLE_RATE, N_FFT, HOP_SIZE, NUM_MELS, FMIN, FMAX);
  for (int n : {1, 511, 512, 513, 44100, 100000}) {
    std::vector<float> audio(static_cast<size_t>(n), 0.0f);
    const int expected = n / HOP_SIZE + 1;
    CHECK(static_cast<int>(mel.compute(audio.data(), n).size()) == expected);
  }
}

void captureOffsetSnapsToFrameGrid() {
  const double hopSeconds =
      static_cast<double>(HOP_SIZE) / static_cast<double>(SAMPLE_RATE);
  const double captureRate = 48000.0;
  for (double offset : {0.0, 0.001, 1.0, 2.123456, 17.5, 63.9999}) {
    const auto snap = TimelineFrameSnap::snapToFrameGrid(offset, captureRate);
    // On the grid...
    const double frames = snap.offsetSeconds / hopSeconds;
    CHECK(std::abs(frames - std::round(frames)) < 1e-6);
    // ...never later than asked, and less than one hop earlier...
    CHECK(snap.offsetSeconds <= offset + 1e-9);
    CHECK(offset - snap.offsetSeconds < hopSeconds + 1e-9);
    // ...with the lead silence putting the audio back where it was, to
    // within one capture sample.
    const double audioStart =
        snap.offsetSeconds + snap.leadSamples / captureRate;
    CHECK(std::abs(audioStart - offset) <= 0.5 / captureRate + 1e-9);
  }
  // Offsets already on the grid need no silence.
  const auto onGrid = TimelineFrameSnap::snapToFrameGrid(hopSeconds * 37, 48000.0);
  CHECK(onGrid.leadSamples == 0);
  CHECK(TimelineFrameSnap::snapToFrameGrid(-1.0, 48000.0).offsetSeconds == 0.0);
}

} // namespace

int main() {
  melFrameIsCentredOnItsVocoderOutput();
  melFrameCountIsUnchanged();
  captureOffsetSnapsToFrameGrid();
  std::printf("SynthesisTimingTests passed\n");
  return 0;
}
