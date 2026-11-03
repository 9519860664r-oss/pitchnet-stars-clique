#pragma once

#include "../Models/Project.h"
#include <algorithm>
#include <vector>

// Clip mode honors the note selection; Track mode covers every voiced note
// intersecting the track's playback windows, once even for overlapping regions.
inline std::vector<Note*> collectPitchMacroNotes(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows)
{
  auto& notes = project.getNotes();
  const bool selected = !trackMode && std::any_of(notes.begin(), notes.end(),
      [](const Note& note) { return !note.isRest() && note.isSelected(); });
  std::vector<Note*> result;
  for (auto& note : notes) {
    const bool inWindow = !trackMode || std::any_of(windows.begin(), windows.end(),
        [&note](juce::Range<int> range) {
          return note.getEndFrame() > range.getStart() && note.getStartFrame() < range.getEnd();
        });
    if (!note.isRest() && inWindow && (!selected || note.isSelected()))
      result.push_back(&note);
  }
  return result;
}

// The notes a pitch macro edits. A selection anywhere limits
// the macro to the selected notes - in Track mode too, across every region -
// and with nothing selected it covers them all. `anySelectionInScope` is
// whether any note collected from any target is selected; the caller works it
// out over all targets so one region's selection does not leave the others
// fully edited.
inline std::vector<Note*> collectPitchMacroEditNotes(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows,
    bool anySelectionInScope)
{
  auto notes = collectPitchMacroNotes(project, trackMode, windows);
  if (anySelectionInScope)
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [](const Note* note) { return !note->isSelected(); }),
                notes.end());
  return notes;
}

inline bool pitchMacroScopeHasSelection(
    Project& project, bool trackMode, const std::vector<juce::Range<int>>& windows)
{
  const auto notes = collectPitchMacroNotes(project, trackMode, windows);
  return std::any_of(notes.begin(), notes.end(),
                     [](const Note* note) { return note->isSelected(); });
}
