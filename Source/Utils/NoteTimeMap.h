#pragma once

#include "../Models/Note.h"
#include "CurveResampler.h"

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Where a note's output frames read their source sound from.
 *
 * A retimed note is not stretched evenly:
 *  - its sibilants keep their length; they only move with the
 *    note ("sweet" stretched becomes s-weeee-t, not sss-weee-ttt);
 *  - warp anchors (timing alignment of doubles) pin moments
 *    inside the note, so a syllable can stretch while the next one squeezes.
 *
 * The map is piecewise linear between knots (source frame -> output frame).
 * With no sibilants and no anchors it is the plain even stretch.
 */
namespace NoteTimeMap
{
struct Knot
{
    double src = 0.0;
    double dst = 0.0;
};

// The pitched part may be squeezed down to this share of its own length
// while the sibilants keep theirs; beyond that the whole note stretches.
constexpr double kMinPitchedShare = 0.25;

inline std::vector<Knot> knots(const Note &note)
{
    const double s0 = note.getSrcStartFrame(), s1 = note.getSrcEndFrame();
    const double d0 = note.getStartFrame(), d1 = note.getEndFrame();
    const double srcLength = s1 - s0, dstLength = d1 - d0;
    std::vector<Knot> result{{s0, d0}};
    if (srcLength <= 0.0 || dstLength <= 0.0)
    {
        result.push_back({s1, d1});
        return result;
    }

    double head = std::max(0, note.getSibilantHead());
    double tail = std::max(0, note.getSibilantTail());
    if (head >= srcLength)
        head = tail = 0.0; // all sibilant (a breath): it stretches as a whole
    head = std::min(head, srcLength);
    tail = std::min(tail, srcLength - head);
    const double pitchedSrc = srcLength - head - tail;
    const double pitchedDst = dstLength - head - tail;
    const bool keepSibilants =
        (head > 0.0 || tail > 0.0) && pitchedSrc > 0.0 &&
        pitchedDst >= std::max(1.0, pitchedSrc * kMinPitchedShare);

    const double innerSrc0 = keepSibilants ? s0 + head : s0;
    const double innerSrc1 = keepSibilants ? s1 - tail : s1;
    const double innerDst0 = keepSibilants ? d0 + head : d0;
    const double innerDst1 = keepSibilants ? d1 - tail : d1;
    if (keepSibilants && head > 0.0)
        result.push_back({innerSrc0, innerDst0});

    // Warp anchors, as shares of the note's source and output lengths, kept
    // strictly inside the pitched part and in order.
    Knot last{innerSrc0, innerDst0};
    for (const auto &anchor : note.getWarpAnchors())
    {
        const Knot k{s0 + anchor.src * srcLength, d0 + anchor.dst * dstLength};
        if (k.src <= last.src + 0.5 || k.dst <= last.dst + 0.5 ||
            k.src >= innerSrc1 - 0.5 || k.dst >= innerDst1 - 0.5)
            continue;
        result.push_back(k);
        last = k;
    }

    if (keepSibilants && tail > 0.0)
        result.push_back({innerSrc1, innerDst1});
    result.push_back({s1, d1});
    return result;
}

/** True when the note reads its source unevenly (sibilants kept, anchors). */
inline bool isUneven(const std::vector<Knot> &k) { return k.size() > 2; }

/** Source position for an output position (frames, continuous). */
inline double sourceAt(const std::vector<Knot> &k, double dst)
{
    if (k.empty())
        return dst;
    if (dst <= k.front().dst)
        return k.front().src + (dst - k.front().dst);
    for (size_t i = 1; i < k.size(); ++i)
        if (dst <= k[i].dst)
        {
            const double span = k[i].dst - k[i - 1].dst;
            const double t = span > 0.0 ? (dst - k[i - 1].dst) / span : 0.0;
            return k[i - 1].src + t * (k[i].src - k[i - 1].src);
        }
    return k.back().src + (dst - k.back().dst);
}

/** Source frame read by output frame `frame` (frame centres, as the even
    stretch has always done: (frame + 0.5) maps to (source + 0.5)). */
inline double sourceFrameFor(const std::vector<Knot> &k, int frame)
{
    return sourceAt(k, static_cast<double>(frame) + 0.5) - 0.5;
}

/** Output position of a source position (the map read the other way). */
inline double outputAt(const std::vector<Knot> &k, double src)
{
    if (k.empty())
        return src;
    if (src <= k.front().src)
        return k.front().dst + (src - k.front().src);
    for (size_t i = 1; i < k.size(); ++i)
        if (src <= k[i].src)
        {
            const double span = k[i].src - k[i - 1].src;
            const double t = span > 0.0 ? (src - k[i - 1].src) / span : 0.0;
            return k[i - 1].dst + t * (k[i].dst - k[i - 1].dst);
        }
    return k.back().dst + (src - k.back().src);
}

/** Where the note's sibilants sit in output frames: they cover
    [start, headEnd) and [tailStart, end). With no sibilant on a side, its
    bound lies far outside the note. */
struct SibilantBounds
{
    double headEnd = -1.0e12;
    double tailStart = 1.0e12;
};

inline SibilantBounds sibilantBounds(const Note &note)
{
    SibilantBounds b;
    if (!note.hasSibilantInfo())
        return b;
    if (note.isAllSibilant())
    {
        b.headEnd = 1.0e12; // the whole note
        return b;
    }
    const auto k = knots(note);
    if (note.getSibilantHead() > 0)
        b.headEnd = outputAt(k, note.getSrcStartFrame() + note.getSibilantHead());
    if (note.getSibilantTail() > 0)
        b.tailStart = outputAt(k, note.getSrcEndFrame() - note.getSibilantTail());
    return b;
}

/** Gain of the note's Sibilant Balance at output sample `sample` (10 ms
    ramps at the sibilant edges); 1 everywhere when the balance is zero. */
inline float balanceGain(const Note &note, const SibilantBounds &b, double sample, int hopSize)
{
    const float balance = note.getSibilantBalance();
    if (std::abs(balance) < 0.0005f || !note.hasSibilantInfo())
        return 1.0f;
    // Never quite zero (-80 dB), so a balance edit can always be undone.
    const double sibilantGain = std::max(1.0e-4, balance < 0.0f ? 1.0 + balance : 1.0);
    const double pitchedGain = std::max(1.0e-4, balance > 0.0f ? 1.0 - balance : 1.0);
    constexpr double kRampSamples = 441.0;
    const double inHead = std::clamp((b.headEnd * hopSize - sample) / kRampSamples + 0.5, 0.0, 1.0);
    const double inTail = std::clamp((sample - b.tailStart * hopSize) / kRampSamples + 0.5, 0.0, 1.0);
    const double w = std::max(inHead, inTail);
    return static_cast<float>(w * sibilantGain + (1.0 - w) * pitchedGain);
}

/**
 * A per-frame curve recorded over the note's source frames, laid out over
 * its `outputFrames` output frames along the map. Curves of any other
 * length (already edited for the output) are resampled evenly.
 */
inline std::vector<float> toOutput(const std::vector<float> &curve, const Note &note,
                                   int outputFrames)
{
    std::vector<float> out(static_cast<size_t>(std::max(0, outputFrames)), 0.0f);
    if (curve.empty() || outputFrames <= 0)
        return out;
    const int srcLength = note.getSrcDurationFrames();
    const auto k = knots(note);
    const bool mapped = static_cast<int>(curve.size()) == srcLength &&
                        outputFrames == note.getDurationFrames() && isUneven(k);
    if (!mapped)
        return static_cast<int>(curve.size()) == outputFrames
                   ? curve
                   : CurveResampler::resampleLinear(curve, outputFrames);
    const double last = static_cast<double>(curve.size() - 1);
    for (int i = 0; i < outputFrames; ++i)
    {
        double pos = sourceFrameFor(k, note.getStartFrame() + i) - note.getSrcStartFrame();
        pos = std::clamp(pos, 0.0, last);
        const auto left = static_cast<size_t>(std::floor(pos));
        const auto right = std::min(left + 1, curve.size() - 1);
        const auto t = static_cast<float>(pos - static_cast<double>(left));
        out[static_cast<size_t>(i)] = curve[left] * (1.0f - t) + curve[right] * t;
    }
    return out;
}
} // namespace NoteTimeMap
