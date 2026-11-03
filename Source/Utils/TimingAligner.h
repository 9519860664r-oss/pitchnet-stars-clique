#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

/**
 * Timing alignment of a double ("dub") to its lead ("guide"), the way
 * dedicated vocal alignment tools do it: not note by note but by the
 * shape of the sound.
 * Each frame (11.6 ms) gets a small fingerprint - the spectral shape in 16
 * bands, loudness, onset strength and voicing - and dynamic time warping
 * finds, phrase by phrase, which guide frame each dub frame corresponds to,
 * within a maximum shift. The path is smoothed, differences under the
 * tolerance are left alone (a double stays alive, not a copy) and the rest
 * is applied by the strength, with a soft limit at the maximum shift.
 */
namespace TimingAligner
{
constexpr int kBands = 16;
constexpr int kDims = kBands + 3; // shape..., loudness, onset, voiced

struct Features
{
    int frames = 0;
    std::vector<float> data; // frames * kDims

    const float *at(int frame) const { return data.data() + static_cast<size_t>(frame) * kDims; }
    bool empty() const { return frames <= 0; }
};

/** Fingerprints from a log-mel spectrogram [T][bins] and a voiced mask. */
inline Features fromMel(const std::vector<std::vector<float>> &mel, const std::vector<bool> &voiced)
{
    Features f;
    f.frames = static_cast<int>(mel.size());
    f.data.assign(static_cast<size_t>(f.frames) * kDims, 0.0f);
    std::vector<float> loudness(static_cast<size_t>(f.frames), 0.0f);
    for (int t = 0; t < f.frames; ++t)
    {
        const auto &row = mel[static_cast<size_t>(t)];
        const int bins = static_cast<int>(row.size());
        float *out = f.data.data() + static_cast<size_t>(t) * kDims;
        if (bins <= 0)
            continue;
        float mean = 0.0f;
        for (int b = 0; b < kBands; ++b)
        {
            const int b0 = b * bins / kBands, b1 = std::max(b0 + 1, (b + 1) * bins / kBands);
            float sum = 0.0f;
            for (int k = b0; k < b1 && k < bins; ++k)
                sum += row[static_cast<size_t>(k)];
            out[b] = sum / static_cast<float>(b1 - b0);
            mean += out[b];
        }
        mean /= static_cast<float>(kBands);
        for (int b = 0; b < kBands; ++b)
            out[b] -= mean; // the shape, whatever the level
        loudness[static_cast<size_t>(t)] = mean;
        out[kBands + 2] = t < static_cast<int>(voiced.size()) && voiced[static_cast<size_t>(t)] ? 1.0f : 0.0f;
    }
    // Loudness relative to the track (z-score), and onsets (rises only).
    double sum = 0.0, sq = 0.0;
    for (float v : loudness)
    {
        sum += v;
        sq += static_cast<double>(v) * v;
    }
    const double n = std::max(1, f.frames);
    const double mu = sum / n;
    const double sd = std::sqrt(std::max(1.0e-6, sq / n - mu * mu));
    for (int t = 0; t < f.frames; ++t)
    {
        float *out = f.data.data() + static_cast<size_t>(t) * kDims;
        const float z = static_cast<float>((loudness[static_cast<size_t>(t)] - mu) / sd);
        out[kBands] = z;
        const float prev = t > 0 ? static_cast<float>((loudness[static_cast<size_t>(t - 1)] - mu) / sd) : z;
        out[kBands + 1] = std::max(0.0f, z - prev) * 2.0f;
    }
    return f;
}

inline float distance(const float *a, const float *b)
{
    float shape = 0.0f;
    for (int i = 0; i < kBands; ++i)
        shape += std::abs(a[i] - b[i]);
    shape /= static_cast<float>(kBands);
    return shape + 0.5f * std::abs(a[kBands] - b[kBands]) +
           0.8f * std::abs(a[kBands + 1] - b[kBands + 1]) +
           0.6f * std::abs(a[kBands + 2] - b[kBands + 2]);
}

struct Options
{
    double strength = 1.0;     // 0..1: how far toward the lead
    int toleranceFrames = 2;   // differences up to this are left alone (~23 ms)
    int maxShiftFrames = 26;   // ~300 ms: the search band and the soft limit
    double flexibility = 0.15; // extra cost of a stretch/squeeze step
};

/**
 * The guide frame matching each dub frame in [start, end), by DTW inside a
 * band of +-maxShift around the same frame. Returns end - start values.
 */
inline std::vector<double> matchPhrase(const Features &dub, const Features &guide, int start, int end,
                                       const Options &options)
{
    const int length = end - start;
    std::vector<double> match(static_cast<size_t>(std::max(0, length)), 0.0);
    if (length <= 0)
        return match;
    const int band = std::max(1, options.maxShiftFrames);
    const int width = 2 * band + 1;
    const float inf = std::numeric_limits<float>::infinity();
    // cost[i][k]: dub frame start+i matched with guide frame start+i+k-band.
    std::vector<float> cost(static_cast<size_t>(length) * width, inf);
    std::vector<unsigned char> step(static_cast<size_t>(length) * width, 0);
    auto guideFrame = [&](int i, int k) { return start + i + k - band; };
    auto local = [&](int i, int k) -> float {
        const int g = guideFrame(i, k);
        if (g < 0 || g >= guide.frames || start + i >= dub.frames)
            return inf;
        return distance(dub.at(start + i), guide.at(g));
    };
    const auto penalty = static_cast<float>(options.flexibility);
    for (int k = 0; k < width; ++k)
        cost[static_cast<size_t>(k)] = local(0, k); // free start inside the band
    for (int i = 1; i < length; ++i)
        for (int k = 0; k < width; ++k)
        {
            const float d = local(i, k);
            if (!std::isfinite(d))
                continue;
            // From (i-1, k): both advance one frame (diagonal).
            float best = cost[static_cast<size_t>(i - 1) * width + k];
            unsigned char how = 0;
            // From (i-1, k+1): the guide stays (the dub is stretched here).
            if (k + 1 < width)
            {
                const float c = cost[static_cast<size_t>(i - 1) * width + k + 1] + penalty;
                if (c < best)
                {
                    best = c;
                    how = 1;
                }
            }
            // From (i-1, k-1): the guide jumps two frames (dub squeezed).
            if (k > 0)
            {
                const float c = cost[static_cast<size_t>(i - 1) * width + k - 1] + penalty;
                if (c < best)
                {
                    best = c;
                    how = 2;
                }
            }
            if (std::isfinite(best))
            {
                cost[static_cast<size_t>(i) * width + k] = best + d;
                step[static_cast<size_t>(i) * width + k] = how;
            }
        }
    // Free end: the cheapest cell on the last row, then back.
    int k = band;
    float bestEnd = inf;
    for (int c = 0; c < width; ++c)
        if (cost[static_cast<size_t>(length - 1) * width + c] < bestEnd)
        {
            bestEnd = cost[static_cast<size_t>(length - 1) * width + c];
            k = c;
        }
    if (!std::isfinite(bestEnd))
    {
        for (int i = 0; i < length; ++i)
            match[static_cast<size_t>(i)] = start + i; // nothing to go on
        return match;
    }
    for (int i = length - 1; i >= 0; --i)
    {
        match[static_cast<size_t>(i)] = guideFrame(i, k);
        if (i == 0)
            break;
        const auto how = step[static_cast<size_t>(i) * width + k];
        if (how == 1)
            k = std::min(width - 1, k + 1);
        else if (how == 2)
            k = std::max(0, k - 1);
    }
    return match;
}

/**
 * Per dub frame, how many frames to move it (positive: later), for the
 * phrases given as [start, end) dub frames. Frames outside them get 0.
 */
inline std::vector<double> offsets(const Features &dub, const Features &guide,
                                   const std::vector<std::pair<int, int>> &phrases, const Options &options)
{
    std::vector<double> result(static_cast<size_t>(std::max(0, dub.frames)), 0.0);
    for (const auto &[start, end] : phrases)
    {
        const int s = std::max(0, start), e = std::min(dub.frames, end);
        if (e - s < 4)
            continue;
        const auto match = matchPhrase(dub, guide, s, e, options);
        // Raw offsets, smoothed (a 9-frame moving average) so the warp has
        // no jitter; then monotonic (the dub never runs backwards).
        const int length = e - s;
        std::vector<double> raw(static_cast<size_t>(length));
        for (int i = 0; i < length; ++i)
            raw[static_cast<size_t>(i)] = match[static_cast<size_t>(i)] - (s + i);
        std::vector<double> smooth(static_cast<size_t>(length));
        for (int i = 0; i < length; ++i)
        {
            double sum = 0.0;
            int count = 0;
            for (int j = std::max(0, i - 4); j <= std::min(length - 1, i + 4); ++j)
            {
                sum += raw[static_cast<size_t>(j)];
                ++count;
            }
            smooth[static_cast<size_t>(i)] = sum / count;
        }
        const double tol = options.toleranceFrames;
        const double limit = std::max(1, options.maxShiftFrames);
        double lastPosition = -1.0e12;
        for (int i = 0; i < length; ++i)
        {
            double off = smooth[static_cast<size_t>(i)];
            // Leave small differences alone and take big ones whole (a
            // smooth step between one and two tolerances, so the warp has
            // no jumps), by the strength, never past the maximum shift.
            if (tol > 0.0)
                off *= std::clamp((std::abs(off) - tol) / tol, 0.0, 1.0);
            off *= options.strength;
            off = limit * std::tanh(off / limit);
            // Keep the output in order (at least a quarter frame per frame).
            const double position = std::max(s + i + off, lastPosition + 0.25);
            lastPosition = position;
            result[static_cast<size_t>(s + i)] = position - (s + i);
        }
    }
    return result;
}

/** Phrases of a track: its note spans joined across gaps shorter than
    `joinFrames` (SmartAlign: long silences are left out). */
inline std::vector<std::pair<int, int>> phrasesFrom(std::vector<std::pair<int, int>> spans, int joinFrames = 26)
{
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<int, int>> phrases;
    for (const auto &span : spans)
    {
        if (span.second <= span.first)
            continue;
        if (!phrases.empty() && span.first - phrases.back().second < joinFrames)
            phrases.back().second = std::max(phrases.back().second, span.second);
        else
            phrases.push_back(span);
    }
    return phrases;
}
} // namespace TimingAligner
