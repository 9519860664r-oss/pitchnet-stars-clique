#include "../Source/Utils/PitchMacroScope.h"
#include "TestAssert.h"
#include <iostream>

int main()
{
  Project first, second;
  first.addNote(Note(0, 10, 60));
  first.addNote(Note(10, 20, 62));
  first.addNote(Note(20, 30, 64));
  first.getNotes()[0].setSelected(true);
  second.addNote(Note(40, 50, 67));
  second.addNote(Note(50, 60, 69));
  second.getNotes()[1].setSelected(true);

  // Every region window participates, regardless of selections. Shared and
  // overlapping playback windows must not duplicate notes in the edit.
  const auto a = collectPitchMacroNotes(first, true, {{0, 10}, {20, 30}, {0, 10}});
  const auto b = collectPitchMacroNotes(second, true, {{40, 60}});
  CHECK(a.size() == 2 && a[0] == &first.getNotes()[0] && a[1] == &first.getNotes()[2]);
  CHECK(b.size() == 2);
  // Half-open boundaries exclude a note outside the track's playback span.
  CHECK(collectPitchMacroNotes(first, true, {{10, 20}}).size() == 1);
  CHECK(collectPitchMacroNotes(first, true, {}).empty());
  // Clip mode retains the established selected-note behavior.
  CHECK(collectPitchMacroNotes(first, false, {}).size() == 1);
  second.getNotes()[1].setSelected(false);
  CHECK(collectPitchMacroNotes(second, false, {}).size() == 2);
  // Macro edits: a selection in any region limits Track mode to the
  // selection everywhere; regions without selected notes are left alone.
  second.getNotes()[1].setSelected(true);
  const bool anySelected =
      pitchMacroScopeHasSelection(first, true, {{0, 30}}) ||
      pitchMacroScopeHasSelection(second, true, {{40, 60}});
  CHECK(anySelected);
  const auto editA = collectPitchMacroEditNotes(first, true, {{0, 30}}, anySelected);
  const auto editB = collectPitchMacroEditNotes(second, true, {{40, 60}}, anySelected);
  CHECK(editA.size() == 1 && editA[0] == &first.getNotes()[0]);
  CHECK(editB.size() == 1 && editB[0] == &second.getNotes()[1]);
  Project third;
  third.addNote(Note(0, 10, 60));
  CHECK(collectPitchMacroEditNotes(third, true, {{0, 10}}, anySelected).empty());
  // No selection anywhere: every note in the windows.
  first.getNotes()[0].setSelected(false);
  second.getNotes()[1].setSelected(false);
  CHECK(!pitchMacroScopeHasSelection(first, true, {{0, 30}}));
  CHECK(collectPitchMacroEditNotes(first, true, {{0, 30}}, false).size() == 3);
  std::cout << "Pitch macro scope tests passed\n";
}
