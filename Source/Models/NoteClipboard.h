#pragma once

#include "Project.h"

#include <memory>
#include <vector>

/**
 * Copying notes. A note is a piece of the recording, so a
 * copy carries the audio and analysis of the frames the notes cover - the
 * original and rendered audio, mel, pitch curves and masks - along with the
 * notes themselves and their edits.
 *
 * Pasting overwrites the destination frames with that material: notes
 * already there are trimmed or removed, the copied notes are placed, and the
 * range is marked for resynthesis. A Slice records what was there before so
 * the paste can be undone exactly.
 *
 * The clipboard is shared by every PitchNet instance in the process, so
 * notes can be copied between tracks.
 */
namespace NoteClipboard
{
struct Clip
{
    juce::String sourceKey;   // track the notes came from
    int lengthFrames = 0;
    double firstNoteSeconds = 0.0; // where the copy started, for the grid offset
    std::vector<Note> notes;  // frame 0 = start of the copy
    std::vector<float> rawF0, cleanedF0, denseF0, f0, baseF0, basePitch, deltaPitch;
    std::vector<bool> voicedMask, vadMask;
    std::vector<std::vector<float>> mel;
    juce::AudioBuffer<float> original, rendered; // lengthFrames * HOP_SIZE samples
};

/** Copy the frames from the first to the last selected note (rests excluded). */
std::shared_ptr<Clip> copySelection(const Project& project, const juce::String& sourceKey);

/** What a paste overwrote, to put it back. */
struct Slice
{
    int startFrame = 0;
    int lengthFrames = 0;
    std::vector<Note> notes; // the whole note list on the other side of the swap
    std::vector<float> rawF0, cleanedF0, denseF0, f0, baseF0, basePitch, deltaPitch;
    std::vector<bool> voicedMask, vadMask;
    std::vector<std::vector<float>> mel;
    juce::AudioBuffer<float> original, rendered;
};

/** Paste at destStartFrame (clamped to the project). Returns the overwritten
    material for undo; lengthFrames 0 when nothing could be pasted. */
Slice paste(Project& project, const Clip& clip, int destStartFrame);

/** Undo / redo of a paste: swap the overwritten material with the slice. */
void swapSlice(Project& project, Slice& slice);

/** The process-wide clipboard: one clip per source track. */
std::vector<std::shared_ptr<Clip>>& contents();
}
