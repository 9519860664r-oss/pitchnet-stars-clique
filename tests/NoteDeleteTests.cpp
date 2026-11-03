// Deleting a note silences it through the amplitude path, and undo brings the
// audio back exactly.

#include "../Source/Undo/AmplitudeAction.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

int main() {
  Project project;
  auto &audio = project.getAudioData();
  const int frames = 40;
  audio.sampleRate = SAMPLE_RATE;
  audio.waveform.setSize(1, frames * HOP_SIZE);
  for (int i = 0; i < frames * HOP_SIZE; ++i)
    audio.waveform.setSample(0, i, 0.5f * std::sin(0.05f * static_cast<float>(i)));
  const juce::AudioBuffer<float> original(audio.waveform);

  project.addNote(Note(0, 10, 60.0f));
  project.addNote(Note(10, 30, 62.0f));
  project.addNote(Note(30, 40, 64.0f));
  Note *target = &project.getNotes()[1];

  AmplitudeAction action(project, {target}, {0.0f},
                         {NoteGainCurve::kDeletedNoteDb}, nullptr, "Delete Notes");
  action.redo();

  // Inside the note, away from the short edge ramps: silent.
  double inside = 0.0, outside = 0.0;
  for (int i = 12 * HOP_SIZE; i < 28 * HOP_SIZE; ++i)
    inside = std::max(inside, static_cast<double>(std::abs(audio.waveform.getSample(0, i))));
  for (int i = 0; i < 8 * HOP_SIZE; ++i)
    outside = std::max(outside, static_cast<double>(std::abs(
        audio.waveform.getSample(0, i) - original.getSample(0, i))));
  CHECK(inside < 0.5 * std::pow(10.0, -90.0 / 20.0));
  CHECK(outside < 1e-6);
  CHECK(target->getVolumeDb() == NoteGainCurve::kDeletedNoteDb);

  action.undo();
  double restored = 0.0;
  for (int i = 0; i < frames * HOP_SIZE; ++i)
    restored = std::max(restored, static_cast<double>(std::abs(
        audio.waveform.getSample(0, i) - original.getSample(0, i))));
  CHECK(restored < 1e-4);
  CHECK(target->getVolumeDb() == 0.0f);

  std::printf("NoteDeleteTests passed\n");
  return 0;
}
