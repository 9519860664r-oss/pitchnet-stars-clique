#include "NoteMacros.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NoteMacros
{
std::vector<Span> quantizeStarts(const std::vector<Span>& notes, double gridFrames,
                                 double originFrame, double intensity,
                                 const std::vector<int>& references,
                                 int maxReachFrames,
                                 const std::vector<bool>& movable)
{
    std::vector<Span> result = notes;
    const double amount = std::clamp(intensity, 0.0, 1.0);
    for (size_t i = 0; i < notes.size(); ++i)
    {
        // Notes outside the selection (and rests) keep their start; only
        // their end follows a connected note that moves.
        if (!movable.empty() && (i >= movable.size() || !movable[i]))
            continue;
        const auto& note = notes[i];
        double target = note.start;
        if (!references.empty())
        {
            int best = -1, bestDistance = std::numeric_limits<int>::max();
            for (int ref : references)
            {
                const int d = std::abs(ref - note.start);
                if (d < bestDistance) { bestDistance = d; best = ref; }
            }
            if (best >= 0 && (maxReachFrames <= 0 || bestDistance <= maxReachFrames))
                target = best;
        }
        else if (gridFrames > 0.0)
        {
            target = originFrame +
                     std::round((note.start - originFrame) / gridFrames) * gridFrames;
        }

        int start = static_cast<int>(std::lround(note.start + (target - note.start) * amount));
        start = std::min(start, note.end - 1);
        if (i > 0)
            start = std::max(start, result[i - 1].start + 1);
        start = std::max(0, start);
        result[i].start = start;

        // A connected previous note follows with its end.
        if (i > 0 && notes[i - 1].end == note.start)
            result[i - 1].end = start;
    }
    return result;
}

std::vector<AlignedNote> alignToReference(const std::vector<Span>& notes,
                                          const std::vector<float>& midi,
                                          const std::vector<RefNote>& references,
                                          const AlignOptions& options)
{
    std::vector<AlignedNote> result(notes.size());
    std::vector<bool> taken(references.size(), false);
    const double t = std::clamp(options.timingIntensity, 0.0, 1.0);
    const double p = std::clamp(options.pitchIntensity, 0.0, 1.0);

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& note = notes[i];
        auto& out = result[i];
        out.span = note;
        out.midi = i < midi.size() ? midi[i] : 0.0f;

        // Most overlap first; otherwise the nearest start within reach.
        int best = -1, bestOverlap = 0, bestDistance = std::numeric_limits<int>::max();
        for (size_t r = 0; r < references.size(); ++r)
        {
            if (taken[r])
                continue;
            const auto& ref = references[r];
            const int overlap = std::min(note.end, ref.end) - std::max(note.start, ref.start);
            const int distance = std::abs(ref.start - note.start);
            if (overlap > bestOverlap ||
                (bestOverlap == 0 && overlap <= 0 && distance < bestDistance &&
                 distance <= options.maxReachFrames))
            {
                best = static_cast<int>(r);
                bestOverlap = std::max(0, overlap);
                bestDistance = distance;
            }
        }
        if (best < 0)
            continue;
        taken[static_cast<size_t>(best)] = true;
        out.reference = best;
        const auto& ref = references[static_cast<size_t>(best)];

        int start = static_cast<int>(std::lround(note.start + (ref.start - note.start) * t));
        int end = options.alignEnds
                      ? static_cast<int>(std::lround(note.end + (ref.end - note.end) * t))
                      : note.end;
        if (end <= start)
            end = start + 1;
        out.span = {std::max(0, start), end};

        if (options.pitchMode != 0)
        {
            const double interval = options.pitchMode == 2 ? options.intervalSemitones : 0.0;
            const double target = ref.midi + interval;
            out.midi = static_cast<float>(out.midi + (target - out.midi) * p);
        }
    }

    // Keep notes in order and apart: a note may not start before the
    // previous one ends after alignment moved them.
    for (size_t i = 1; i < result.size(); ++i)
    {
        auto& prev = result[i - 1].span;
        auto& cur = result[i].span;
        if (cur.start < prev.end)
        {
            if (cur.start <= prev.start)
                cur.start = prev.start + 1;
            prev.end = cur.start;
        }
        if (cur.end <= cur.start)
            cur.end = cur.start + 1;
    }
    return result;
}

std::vector<float> levelingGains(const std::vector<float>& levelsDb,
                                 const std::vector<int>& lengthsFrames,
                                 double raiseQuiet, double lowerLoud,
                                 int minFramesForQuiet)
{
    std::vector<float> gains(levelsDb.size(), 0.0f);
    if (levelsDb.size() < 2)
        return gains;
    // Mean level weighted by length, in dB.
    double sum = 0.0, weight = 0.0;
    for (size_t i = 0; i < levelsDb.size(); ++i)
    {
        const double w = i < lengthsFrames.size() ? std::max(1, lengthsFrames[i]) : 1;
        sum += levelsDb[i] * w;
        weight += w;
    }
    const double mean = sum / std::max(1.0, weight);
    const double up = std::clamp(raiseQuiet, 0.0, 1.0);
    const double down = std::clamp(lowerLoud, 0.0, 1.0);
    for (size_t i = 0; i < levelsDb.size(); ++i)
    {
        const double level = levelsDb[i];
        const int length = i < lengthsFrames.size() ? lengthsFrames[i] : minFramesForQuiet;
        if (level < mean)
        {
            if (length < minFramesForQuiet)
                continue; // short and quiet: probably noise
            gains[i] = static_cast<float>((mean - level) * up);
        }
        else
        {
            gains[i] = static_cast<float>((mean - level) * down);
        }
    }
    return gains;
}
} // namespace NoteMacros
