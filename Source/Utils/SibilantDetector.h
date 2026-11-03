#pragma once

#include "../Models/Project.h"

#include <algorithm>

/**
 * Sibilants: the unpitched parts of a sung note
 * ("s", "sh", "f", "t", "k", breaths). They lie at the start and/or end of a
 * note, never in the middle; a breath can be a whole note. On screen they
 * move with their note; in sound they keep their own pitch and length.
 *
 * Ranges are counted in source frames from the note's source start / end.
 */
namespace SibilantDetector
{
// Shorter unpitched blips (one frame is 11.6 ms) are left to the note.
constexpr int kMinFrames = 2;

inline bool rawVoicedAt(const AudioData &audio, int frame)
{
    if (!audio.voicedMask.empty())
        return frame >= 0 && frame < static_cast<int>(audio.voicedMask.size()) &&
               audio.voicedMask[static_cast<size_t>(frame)];
    if (!audio.rawF0.empty())
        return frame >= 0 && frame < static_cast<int>(audio.rawF0.size()) &&
               audio.rawF0[static_cast<size_t>(frame)] > 0.0f;
    return true; // nothing known: treat as pitched
}

/** Voiced, ignoring a lone voiced frame between unvoiced ones (a detector
    blip inside an "s" does not split it). */
inline bool voicedAt(const AudioData &audio, int frame)
{
    if (!rawVoicedAt(audio, frame))
        return false;
    return rawVoicedAt(audio, frame - 1) || rawVoicedAt(audio, frame + 1);
}

struct Range
{
    int head = 0; // sibilant frames at the start
    int tail = 0; // sibilant frames at the end
};

/** Sibilant frames at the ends of the source frames [start, end). A note
    with no pitched frame at all is a sibilant as a whole (head = length). */
inline Range detect(const AudioData &audio, int start, int end)
{
    Range range;
    const int length = end - start;
    if (length <= 0)
        return range;
    int head = 0;
    while (head < length && !voicedAt(audio, start + head))
        ++head;
    if (head >= length)
    {
        range.head = length;
        return range;
    }
    int tail = 0;
    while (tail < length - head && !voicedAt(audio, end - 1 - tail))
        ++tail;
    range.head = head >= kMinFrames ? head : 0;
    range.tail = tail >= kMinFrames ? tail : 0;
    return range;
}

inline void detect(const AudioData &audio, Note &note)
{
    const auto range = detect(audio, note.getSrcStartFrame(), note.getSrcEndFrame());
    note.setSibilants(range.head, range.tail, false);
}

/** Finds the sibilants of every note that has none recorded yet (a new
    analysis, a split, a project from before sibilants). Notes whose
    sibilants were set by hand are left alone. */
inline bool ensure(Project &project)
{
    bool changed = false;
    const auto &audio = project.getAudioData();
    if (audio.voicedMask.empty() && audio.rawF0.empty())
        return false;
    for (auto &note : project.getNotes())
        if (!note.isRest() && !note.hasSibilantInfo())
        {
            detect(audio, note);
            changed = true;
        }
    return changed;
}
} // namespace SibilantDetector
