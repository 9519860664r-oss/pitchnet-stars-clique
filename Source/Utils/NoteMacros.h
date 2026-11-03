#pragma once

#include <vector>
#include <cmath>

/**
 * The arithmetic of the timing, alignment and leveling macros, kept free of
 * UI and project plumbing so it can be tested. All positions are in analysis
 * frames.
 */
namespace NoteMacros
{
struct Span
{
    int start = 0;
    int end = 0;
};

/**
 * Quantize Time: each note's start moves `intensity` (0..1) of
 * the way to its target - the nearest grid line (spacing `gridFrames`,
 * lines at origin + k * gridFrames) or, when `references` is not empty, the
 * nearest reference start within `maxReachFrames`. The end stays, so the
 * note stretches; a previous note that ended where this one started (a
 * connected note) follows with its end. Starts never pass the note's end or
 * the previous note's start.
 */
std::vector<Span> quantizeStarts(const std::vector<Span>& notes, double gridFrames,
                                 double originFrame, double intensity,
                                 const std::vector<int>& references = {},
                                 int maxReachFrames = 0,
                                 const std::vector<bool>& movable = {});

/**
 * The value a macro popup opens with, from the values the notes remember
 * (-1: never corrected): the shared value when every note has the same one,
 * otherwise 0 (nothing applied, or different values).
 */
inline float rememberedMacroValue(const std::vector<float>& values)
{
    if (values.empty() || values.front() < 0.0f)
        return 0.0f;
    for (float v : values)
        if (v < 0.0f || std::abs(v - values.front()) > 0.01f)
            return 0.0f;
    return values.front();
}

struct RefNote
{
    int start = 0;
    int end = 0;
    float midi = 0.0f;
};

struct AlignOptions
{
    double timingIntensity = 1.0; // 0..1
    bool alignEnds = true;        // ends too, not only starts
    int maxReachFrames = 40;      // ignore reference notes further than this
    int pitchMode = 0;            // 0 none, 1 unison, 2 interval
    double intervalSemitones = 0; // for pitchMode 2 (e.g. -3, +4, +7, -12)
    double pitchIntensity = 1.0;  // 0..1
};

struct AlignedNote
{
    Span span;
    float midi = 0.0f;
    int reference = -1; // index into the reference notes, -1 when unmatched
};

/**
 * Align notes to a reference track (timing, plus a
 * groove reference track): each note is matched to the reference note that
 * overlaps it most (or, if none overlaps, the nearest start within reach),
 * in order, so two notes never take the same reference note. Matched notes
 * move their start (and end) toward the reference's; pitch can follow in
 * unison or at an interval.
 */
std::vector<AlignedNote> alignToReference(const std::vector<Span>& notes,
                                          const std::vector<float>& midi,
                                          const std::vector<RefNote>& references,
                                          const AlignOptions& options);

/**
 * Leveling: moves note levels (dB) toward the mean level of the
 * selection. `raiseQuiet` (0..1) lifts notes below the mean, `lowerLoud`
 * (0..1) lowers notes above it; neither crosses the mean. Notes shorter than
 * `minFramesForQuiet` and quieter than the mean are treated as noise and
 * left alone. Returns the gain change in dB for each note.
 */
std::vector<float> levelingGains(const std::vector<float>& levelsDb,
                                 const std::vector<int>& lengthsFrames,
                                 double raiseQuiet, double lowerLoud,
                                 int minFramesForQuiet = 6);
} // namespace NoteMacros
