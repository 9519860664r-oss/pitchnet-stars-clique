// Copy / paste of notes with their audio and analysis, and its undo.

#include "../Source/Models/NoteClipboard.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

namespace {

Project makeProject() {
  Project project;
  auto &audio = project.getAudioData();
  const int frames = 100;
  audio.sampleRate = SAMPLE_RATE;
  audio.f0.resize(frames);
  audio.rawF0.resize(frames);
  audio.cleanedF0.resize(frames);
  audio.denseF0.resize(frames);
  audio.baseF0.resize(frames);
  audio.basePitch.resize(frames);
  audio.deltaPitch.resize(frames);
  audio.voicedMask.assign(frames, true);
  audio.vadMask.assign(frames, true);
  audio.melSpectrogram.assign(frames, std::vector<float>(NUM_MELS, 0.0f));
  for (int i = 0; i < frames; ++i) {
    audio.f0[static_cast<size_t>(i)] = 100.0f + static_cast<float>(i);
    audio.melSpectrogram[static_cast<size_t>(i)][0] = static_cast<float>(i);
  }
  audio.originalWaveform.setSize(1, frames * HOP_SIZE);
  audio.waveform.setSize(1, frames * HOP_SIZE);
  for (int s = 0; s < frames * HOP_SIZE; ++s) {
    audio.originalWaveform.setSample(0, s, static_cast<float>(s) * 1e-5f);
    audio.waveform.setSample(0, s, static_cast<float>(s) * 1e-5f);
  }
  project.addNote(Note(10, 20, 60.0f));
  project.addNote(Note(30, 40, 62.0f));
  project.addNote(Note(60, 70, 64.0f));
  return project;
}

} // namespace

int main() {
  auto project = makeProject();
  project.getNotes()[0].setSelected(true);

  const auto clip = NoteClipboard::copySelection(project, "track-a");
  CHECK(clip != nullptr);
  CHECK(clip->lengthFrames == 10);
  CHECK(clip->notes.size() == 1 && clip->notes[0].getStartFrame() == 0);
  CHECK(std::abs(clip->f0[0] - 110.0f) < 1e-4f);

  // Paste over the note at 60..70: it is replaced by the copy.
  auto slice = NoteClipboard::paste(project, *clip, 60);
  CHECK(slice.lengthFrames == 10);
  const auto &audio = project.getAudioData();
  CHECK(std::abs(audio.f0[60] - 110.0f) < 1e-4f);
  CHECK(std::abs(audio.f0[69] - 119.0f) < 1e-4f);
  CHECK(std::abs(audio.melSpectrogram[65][0] - 15.0f) < 1e-4f);
  // Audio away from the crossfaded edges is the source's.
  const int mid = 65 * HOP_SIZE;
  CHECK(std::abs(audio.originalWaveform.getSample(0, mid) -
                 static_cast<float>(15 * HOP_SIZE) * 1e-5f) < 1e-6f);
  int at60 = 0;
  float midi60 = 0.0f;
  for (const auto &note : project.getNotes())
    if (note.getStartFrame() == 60) { ++at60; midi60 = note.getMidiNote(); }
  CHECK(at60 == 1 && std::abs(midi60 - 60.0f) < 1e-4f);
  CHECK(project.getNotes().size() == 3);

  // A paste that overlaps a note's edge trims it.
  auto edge = NoteClipboard::paste(project, *clip, 35);
  bool trimmed = false;
  for (const auto &note : project.getNotes())
    if (note.getStartFrame() == 30) trimmed = note.getEndFrame() == 35;
  CHECK(trimmed);
  NoteClipboard::swapSlice(project, edge); // undo it again

  // Undo restores the original material exactly; redo re-applies it.
  NoteClipboard::swapSlice(project, slice);
  CHECK(std::abs(audio.f0[60] - 160.0f) < 1e-4f);
  CHECK(std::abs(audio.originalWaveform.getSample(0, mid) -
                 static_cast<float>(mid) * 1e-5f) < 1e-6f);
  bool original64 = false;
  for (const auto &note : project.getNotes())
    if (note.getStartFrame() == 60) original64 = std::abs(note.getMidiNote() - 64.0f) < 1e-4f;
  CHECK(original64);
  NoteClipboard::swapSlice(project, slice);
  CHECK(std::abs(audio.f0[60] - 110.0f) < 1e-4f);

  // Pasting past the end is clamped to the project.
  auto tail = NoteClipboard::paste(project, *clip, 95);
  CHECK(tail.lengthFrames == 5);
  CHECK(NoteClipboard::paste(project, *clip, 100).lengthFrames == 0);

  // Nothing selected: nothing to copy.
  for (auto &note : project.getNotes()) note.setSelected(false);
  CHECK(NoteClipboard::copySelection(project, "x") == nullptr);

  std::printf("NoteClipboardTests passed\n");
  return 0;
}
