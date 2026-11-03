#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

/**
 * Replace Ranges (plug-in Transfer): the parts of the DAW track
 * where the plug-in's audio replaces the track. Everywhere else the track
 * plays as recorded. Seconds on the host timeline, sorted, never overlapping.
 * An empty list means "the whole track" (projects from before ranges).
 */
namespace ReplaceRanges
{
using Range = std::pair<double, double>;
using List = std::vector<Range>;

// Shorter pieces are dropped; closer neighbours are joined.
constexpr double kMinLengthSeconds = 0.01;

inline List normalize(List ranges, double joinGapSeconds = 0.0)
{
    for (auto& r : ranges)
        if (r.first > r.second)
            std::swap(r.first, r.second);
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                [](const Range& r) {
                                    return !std::isfinite(r.first) || !std::isfinite(r.second) ||
                                           r.second - r.first < kMinLengthSeconds;
                                }),
                 ranges.end());
    std::sort(ranges.begin(), ranges.end());
    List merged;
    for (const auto& r : ranges)
    {
        const Range clipped{std::max(0.0, r.first), std::max(0.0, r.second)};
        if (!merged.empty() && clipped.first <= merged.back().second + joinGapSeconds)
            merged.back().second = std::max(merged.back().second, clipped.second);
        else
            merged.push_back(clipped);
    }
    return merged;
}

inline List add(List ranges, double start, double end)
{
    ranges.emplace_back(start, end);
    return normalize(std::move(ranges));
}

inline bool contains(const List& ranges, double seconds)
{
    for (const auto& r : ranges)
        if (seconds >= r.first && seconds < r.second)
            return true;
    return false;
}

/** Index of the range edge within `toleranceSeconds` of `seconds`, or -1;
    `isStart` says which edge. The nearest edge wins. */
inline int edgeAt(const List& ranges, double seconds, double toleranceSeconds, bool& isStart)
{
    int best = -1;
    double bestDistance = toleranceSeconds;
    for (int i = 0; i < static_cast<int>(ranges.size()); ++i)
    {
        const double ds = std::abs(ranges[static_cast<size_t>(i)].first - seconds);
        const double de = std::abs(ranges[static_cast<size_t>(i)].second - seconds);
        if (ds <= bestDistance) { best = i; bestDistance = ds; isStart = true; }
        if (de <= bestDistance) { best = i; bestDistance = de; isStart = false; }
    }
    return best;
}

/** "Set Replace Ranges to Notes": the parts of the existing ranges covered by
    the given note spans (deleted notes leave holes). Gaps shorter than
    `joinGapSeconds` between notes stay replaced. */
inline List clipToNotes(const List& ranges, List noteSpans, double joinGapSeconds = 0.1)
{
    noteSpans = normalize(std::move(noteSpans), joinGapSeconds);
    if (ranges.empty())
        return noteSpans;
    List result;
    for (const auto& r : ranges)
        for (const auto& n : noteSpans)
        {
            const double s = std::max(r.first, n.first), e = std::min(r.second, n.second);
            if (e > s)
                result.emplace_back(s, e);
        }
    return normalize(std::move(result));
}
} // namespace ReplaceRanges
