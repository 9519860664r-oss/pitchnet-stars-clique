#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

/**
 * Doubles tuned to their lead, note for note ("unison"), and finding which
 * tracks are doubles of which. Pure arithmetic, kept free of projects and UI
 * so it can be tested. Times are in analysis frames on a shared timeline.
 */
namespace DoubleTuning
{
struct TNote
{
    int start = 0;
    int end = 0;
    float midi = 0.0f;
};

/** The lead note a double note doubles, and the octave it sings it in. */
struct Match
{
    int lead = -1;            // index into the lead's notes, -1: not a double here
    float octaveShift = 0.0f; // 0, +12 or -12 (an octave double)
};

// Doubles can miss the lead by more than a tone; up to this many semitones a
// note still counts as the same note.
constexpr float kMaxMissSemitones = 5.0f;
// A note must share at least this much of the shorter note's length with the
// lead note to be its double.
constexpr float kMinOverlap = 0.5f;

inline int overlapFrames(const TNote& a, const TNote& b)
{
    return std::max(0, std::min(a.end, b.end) - std::max(a.start, b.start));
}

// For unison a double note follows the lead note it sounds with for at
// least this share of its own length.
constexpr float kMinUnisonOverlap = 0.25f;

/** Unison: for each double note, the lead note sounding with it the longest
    (at least a quarter of the double note), or -1 when the lead is silent
    there. Pitch does not matter: unison means the double sings the lead's
    note, whatever it sang, in the lead's octave. */
inline std::vector<Match> matchNotes(const std::vector<TNote>& doubleNotes,
                                     const std::vector<TNote>& leadNotes)
{
    std::vector<Match> matches(doubleNotes.size());
    for (size_t d = 0; d < doubleNotes.size(); ++d)
    {
        const auto& note = doubleNotes[d];
        const int length = note.end - note.start;
        if (length <= 0)
            continue;
        int bestOverlap = 0;
        for (size_t l = 0; l < leadNotes.size(); ++l)
        {
            const int overlap = overlapFrames(note, leadNotes[l]);
            if (overlap > bestOverlap &&
                static_cast<float>(overlap) >= kMinUnisonOverlap * static_cast<float>(length))
            {
                bestOverlap = overlap;
                matches[d] = {static_cast<int>(l), 0.0f};
            }
        }
    }
    return matches;
}

/** Finding doubles: for each note, the lead note it repeats (largest time
    overlap among the lead notes close enough in pitch), or -1. A note about
    an octave away (within 3 semitones of it) counts as an octave double. */
inline std::vector<Match> matchSimilarNotes(const std::vector<TNote>& doubleNotes,
                                            const std::vector<TNote>& leadNotes,
                                            float maxMiss = kMaxMissSemitones)
{
    std::vector<Match> matches(doubleNotes.size());
    for (size_t d = 0; d < doubleNotes.size(); ++d)
    {
        const auto& note = doubleNotes[d];
        const int length = note.end - note.start;
        if (length <= 0)
            continue;
        int bestOverlap = 0;
        for (size_t l = 0; l < leadNotes.size(); ++l)
        {
            const auto& lead = leadNotes[l];
            const int overlap = overlapFrames(note, lead);
            const int shorter = std::min(length, lead.end - lead.start);
            if (overlap <= 0 || shorter <= 0 ||
                static_cast<float>(overlap) < kMinOverlap * static_cast<float>(shorter))
                continue;
            const float diff = note.midi - lead.midi;
            float octave = 0.0f;
            if (std::abs(diff) > maxMiss)
            {
                if (std::abs(std::abs(diff) - 12.0f) > std::min(maxMiss, 3.0f))
                    continue; // neither the same note nor an octave double
                octave = diff > 0.0f ? 12.0f : -12.0f;
            }
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                matches[d] = {static_cast<int>(l), octave};
            }
        }
    }
    return matches;
}

/** Where a double note goes: from its own pitch `source` toward the lead note
    (in the double's octave) by `strength` (0 stays, 1 note for note). */
inline float unisonMidi(float source, float leadMidi, float octaveShift, float strength)
{
    const float target = leadMidi + octaveShift;
    return source + (target - source) * std::clamp(strength, 0.0f, 1.0f);
}

/** The double note's pitch contour (offsets from its note pitch, one per
    frame) moved toward the lead's: `leadContour` is the lead's pitch at the
    same frames minus the lead note's pitch (NaN where the lead has none).
    0 keeps the double's own contour, 1 copies the lead's (its slides,
    vibrato and drift), so the two sing note for note. */
inline std::vector<float> unisonContour(const std::vector<float>& ownContour,
                                        const std::vector<float>& leadContour, float strength)
{
    const float s = std::clamp(strength, 0.0f, 1.0f);
    std::vector<float> out(ownContour.size());
    for (size_t i = 0; i < ownContour.size(); ++i)
    {
        const float lead = i < leadContour.size() ? leadContour[i] : std::nanf("");
        out[i] = std::isnan(lead) ? ownContour[i] : ownContour[i] + (lead - ownContour[i]) * s;
    }
    return out;
}

/**
 * Natural unison. A double keeps what makes it a separate singer - its
 * vibrato, its jitter, the fast detail of its slides - and takes from the
 * lead where the notes sit, how they move from one to the next and how they
 * drift. Each pitch curve is split in two:
 *
 *   slow = the note pitch (the base curve rendering builds from the notes)
 *          plus the slow part of the contour inside each note;
 *   fast = the rest (vibrato, jitter).
 *
 * The double becomes the lead's slow part plus its own fast part, through an
 * offset added to its own pitch:
 *
 *  - per frame, the lead's slow pitch minus the double's (`leadRef` is the
 *    lead's at the moment the double sings the same sound, NaN where the
 *    lead is silent or unpitched; `ownRef` NaN where the double is);
 *  - octave doubles stay in their octave; frames further off than `maxMiss`
 *    (another line, a detection slip) and lone pitched frames (inside an
 *    "s" or a "th") are ignored;
 *  - between the frames that tell, the offset runs straight;
 *  - from note to note it crosses over in a few frames, so nothing jumps;
 *  - the last `tolerance` semitones are left alone: doubles that sit within
 *    a few cents of each other sound like people, not copies.
 *
 * `noteOf` gives each frame's double note (-1 between notes). Returns the
 * offset in semitones per frame (0 between notes).
 */
struct NaturalOptions
{
    float tolerance = 0.06f; // semitones a double may stay off (its life)
    float joinSigma = 2.5f;  // frames: the crossover from note to note
    float maxMiss = 3.5f;    // semitones: further off is another syllable or a slip, not a missed note
};

// What counts as slow: a Gaussian of this many frames (~40 ms) inside a
// note. Vibrato (5-7 Hz) is mostly faster, a melisma's steps slower.
constexpr float kSlowSigma = 3.5f;

inline float medianOf(std::vector<float> values)
{
    if (values.empty())
        return std::nanf("");
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    if (values.size() % 2 == 1)
        return *mid;
    const float upper = *mid;
    const float lower = *std::max_element(values.begin(), mid);
    return 0.5f * (lower + upper);
}

struct FrameRange
{
    int start, end;
};

/** Each note's frames, in order. */
inline std::vector<FrameRange> noteRanges(const std::vector<int>& noteOf)
{
    std::vector<FrameRange> ranges;
    const int n = static_cast<int>(noteOf.size());
    for (int t = 0; t < n;)
    {
        const int note = noteOf[static_cast<size_t>(t)];
        int e = t + 1;
        while (e < n && noteOf[static_cast<size_t>(e)] == note)
            ++e;
        if (note >= 0)
            ranges.push_back({t, e});
        t = e;
    }
    return ranges;
}

/** Fills the NaN frames of [start, end) in a straight line between the
    known ones (held at the ends). False when none is known. */
inline bool fillStraight(std::vector<float>& x, int start, int end)
{
    int previous = -1;
    for (int t = start; t < end; ++t)
    {
        if (!std::isfinite(x[static_cast<size_t>(t)]))
            continue;
        if (previous < 0)
            for (int u = start; u < t; ++u)
                x[static_cast<size_t>(u)] = x[static_cast<size_t>(t)];
        else
            for (int u = previous + 1; u < t; ++u)
            {
                const float w = static_cast<float>(u - previous) / static_cast<float>(t - previous);
                x[static_cast<size_t>(u)] =
                    x[static_cast<size_t>(previous)] * (1.0f - w) + x[static_cast<size_t>(t)] * w;
            }
        previous = t;
    }
    if (previous < 0)
        return false;
    for (int u = previous + 1; u < end; ++u)
        x[static_cast<size_t>(u)] = x[static_cast<size_t>(previous)];
    return true;
}

/** The slow part of `x` (NaN where unknown) inside each note: a Gaussian of
    `sigma` frames that never reaches into another note, filled straight
    across unknown frames. NaN outside notes and in notes with nothing known. */
inline std::vector<float> slowWithinNotes(const std::vector<float>& x, const std::vector<int>& noteOf,
                                          float sigma = kSlowSigma)
{
    const int n = static_cast<int>(noteOf.size());
    std::vector<float> out(static_cast<size_t>(n), std::nanf(""));
    const float s = std::max(0.5f, sigma);
    const int reach = static_cast<int>(std::ceil(3.0f * s));
    for (const auto& r : noteRanges(noteOf))
    {
        for (int t = r.start; t < r.end; ++t)
        {
            double sum = 0.0, weight = 0.0;
            for (int u = std::max(r.start, t - reach); u < std::min(r.end, t + reach + 1); ++u)
            {
                const float v = u < static_cast<int>(x.size()) ? x[static_cast<size_t>(u)] : std::nanf("");
                if (!std::isfinite(v))
                    continue;
                const double z = (u - t) / static_cast<double>(s);
                const double w = std::exp(-0.5 * z * z);
                sum += w * v;
                weight += w;
            }
            if (weight > 1.0e-3)
                out[static_cast<size_t>(t)] = static_cast<float>(sum / weight);
        }
        fillStraight(out, r.start, r.end);
    }
    return out;
}

inline std::vector<float> unisonOffsets(const std::vector<float>& ownRef, const std::vector<float>& leadRef,
                                        const std::vector<int>& noteOf, const NaturalOptions& options = {})
{
    const int n = static_cast<int>(noteOf.size());
    const float nan = std::nanf("");
    std::vector<float> diff(static_cast<size_t>(n), nan);
    for (int t = 0; t < n; ++t)
    {
        const float a = t < static_cast<int>(ownRef.size()) ? ownRef[static_cast<size_t>(t)] : nan;
        const float b = t < static_cast<int>(leadRef.size()) ? leadRef[static_cast<size_t>(t)] : nan;
        if (noteOf[static_cast<size_t>(t)] >= 0 && std::isfinite(a) && std::isfinite(b))
            diff[static_cast<size_t>(t)] = b - a;
    }

    // Octave doubles: per phrase (notes joined across short gaps), the
    // octave the double sings the lead in.
    for (int t = 0; t < n;)
    {
        if (noteOf[static_cast<size_t>(t)] < 0)
        {
            ++t;
            continue;
        }
        int last = t;
        for (int e = t; e < n && e - last < 26; ++e)
            if (noteOf[static_cast<size_t>(e)] >= 0)
                last = e;
        std::vector<float> values;
        for (int u = t; u <= last; ++u)
            if (std::isfinite(diff[static_cast<size_t>(u)]))
                values.push_back(diff[static_cast<size_t>(u)]);
        const float median = medianOf(values);
        const float octave =
            std::isfinite(median) && std::abs(median) >= 9.0f ? 12.0f * std::round(median / 12.0f) : 0.0f;
        for (int u = t; u <= last; ++u)
        {
            float& d = diff[static_cast<size_t>(u)];
            if (!std::isfinite(d))
                continue;
            d -= octave;
            if (std::abs(d) > options.maxMiss)
                d = nan;
        }
        t = last + 1;
    }

    // Per note: short slips out (a 5-frame median; a lone pitched frame is
    // not a note), then straight across what is unknown.
    const auto ranges = noteRanges(noteOf);
    std::vector<float> offset(static_cast<size_t>(n), nan);
    std::vector<char> hasLead(ranges.size(), 0);
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        const auto& r = ranges[i];
        for (int t = r.start; t < r.end; ++t)
        {
            if (!std::isfinite(diff[static_cast<size_t>(t)]))
                continue;
            std::vector<float> window;
            for (int u = std::max(r.start, t - 2); u <= std::min(r.end - 1, t + 2); ++u)
                if (std::isfinite(diff[static_cast<size_t>(u)]))
                    window.push_back(diff[static_cast<size_t>(u)]);
            if (window.size() >= 3)
                offset[static_cast<size_t>(t)] = medianOf(window);
        }
        hasLead[i] = fillStraight(offset, r.start, r.end) ? 1 : 0;
    }
    // A note the lead does not sing with (a consonant sung early, a tail
    // held longer) moves with the note it is joined to, else stays.
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        if (hasLead[i])
            continue;
        const auto& r = ranges[i];
        float value = 0.0f;
        if (i + 1 < ranges.size() && hasLead[i + 1] && ranges[i + 1].start - r.end <= 2)
            value = offset[static_cast<size_t>(ranges[i + 1].start)];
        else if (i > 0 && hasLead[i - 1] && r.start - ranges[i - 1].end <= 2)
            value = offset[static_cast<size_t>(ranges[i - 1].end - 1)];
        for (int t = r.start; t < r.end; ++t)
            offset[static_cast<size_t>(t)] = value;
    }

    // The double's life: the last few cents stay its own.
    const float tol = std::max(0.0f, options.tolerance);
    if (tol > 0.0f)
        for (auto& o : offset)
            if (std::isfinite(o))
                o -= tol * std::tanh(o / tol);

    // Note to note: a short crossover inside each run of joined notes.
    std::vector<float> result(static_cast<size_t>(n), 0.0f);
    const float join = std::max(0.5f, options.joinSigma);
    const int joinReach = static_cast<int>(std::ceil(3.0f * join));
    for (int t = 0; t < n;)
    {
        if (!std::isfinite(offset[static_cast<size_t>(t)]))
        {
            ++t;
            continue;
        }
        int e = t;
        while (e < n && std::isfinite(offset[static_cast<size_t>(e)]))
            ++e;
        for (int u = t; u < e; ++u)
        {
            double sum = 0.0, weight = 0.0;
            for (int v = std::max(t, u - joinReach); v < std::min(e, u + joinReach + 1); ++v)
            {
                const double z = (v - u) / static_cast<double>(join);
                const double w = std::exp(-0.5 * z * z);
                sum += w * offset[static_cast<size_t>(v)];
                weight += w;
            }
            result[static_cast<size_t>(u)] = static_cast<float>(sum / weight);
        }
        t = e;
    }
    return result;
}

inline int voicedFrames(const std::vector<TNote>& notes)
{
    int total = 0;
    for (const auto& n : notes)
        total += std::max(0, n.end - n.start);
    return total;
}

/** Share (0..1) of a track's sung time that doubles another track. */
inline float doubleScore(const std::vector<TNote>& candidate, const std::vector<TNote>& lead,
                         float maxMiss = kMaxMissSemitones)
{
    const int total = voicedFrames(candidate);
    if (total <= 0)
        return 0.0f;
    const auto matches = matchSimilarNotes(candidate, lead, maxMiss);
    int doubled = 0;
    for (size_t i = 0; i < matches.size(); ++i)
        if (matches[i].lead >= 0)
            doubled += overlapFrames(candidate[i], lead[static_cast<size_t>(matches[i].lead)]);
    return static_cast<float>(doubled) / static_cast<float>(total);
}

struct Group
{
    int lead = -1;
    std::vector<int> doubles;
};

// Doubles usually repeat the lead only in places: this share of a track's
// sung time is enough.
constexpr float kDetectThreshold = 0.35f;

/** Groups of tracks that double each other: tracks are joined when either
    doubles the other for at least `threshold` of its sung time. In each
    group the track with the most sung time is the lead. */
inline std::vector<Group> detectGroups(const std::vector<std::vector<TNote>>& tracks,
                                       float threshold = kDetectThreshold)
{
    const int n = static_cast<int>(tracks.size());
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    const auto findRoot = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x)
            x = parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
        return x;
    };
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b)
            if (doubleScore(tracks[static_cast<size_t>(a)], tracks[static_cast<size_t>(b)]) >= threshold ||
                doubleScore(tracks[static_cast<size_t>(b)], tracks[static_cast<size_t>(a)]) >= threshold)
                parent[static_cast<size_t>(findRoot(a))] = findRoot(b);

    std::vector<Group> groups;
    std::vector<int> groupOfRoot(static_cast<size_t>(n), -1);
    for (int t = 0; t < n; ++t)
    {
        const int root = findRoot(t);
        auto& slot = groupOfRoot[static_cast<size_t>(root)];
        if (slot < 0)
        {
            slot = static_cast<int>(groups.size());
            groups.push_back({});
        }
        groups[static_cast<size_t>(slot)].doubles.push_back(t);
    }
    std::vector<Group> result;
    for (auto& group : groups)
    {
        if (group.doubles.size() < 2)
            continue;
        const auto lead = std::max_element(group.doubles.begin(), group.doubles.end(),
                                           [&](int a, int b) {
                                               return voicedFrames(tracks[static_cast<size_t>(a)]) <
                                                      voicedFrames(tracks[static_cast<size_t>(b)]);
                                           });
        Group out;
        out.lead = *lead;
        for (int t : group.doubles)
            if (t != out.lead)
                out.doubles.push_back(t);
        result.push_back(std::move(out));
    }
    return result;
}
} // namespace DoubleTuning
