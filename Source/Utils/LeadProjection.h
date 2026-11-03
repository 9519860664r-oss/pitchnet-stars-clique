#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include "TimingAligner.h"

#include <cmath>

/**
 * In ARA hosts every clip (region) has its own project, counted in its own
 * audio's time, and the lead's clips and a double's clips sit at different
 * places on the timeline. Unison and Timing compare the two frame by frame,
 * so the lead is first laid out in the double's frames: what the lead sings
 * at each moment of the song, at the double frame that plays at that moment.
 */
namespace LeadProjection
{
/** Where a clip's project plays: frame f sounds at
    offsetSeconds + f * HOP_SIZE / sampleRate, inside [start, end). */
struct Placement
{
    double offsetSeconds = 0.0; // clip start on the timeline - its start in its audio
    double startSeconds = 0.0;  // clip on the timeline
    double endSeconds = 0.0;
};

inline double frameSeconds(const Project &project)
{
    const int rate = project.getAudioData().sampleRate > 0 ? project.getAudioData().sampleRate : SAMPLE_RATE;
    return static_cast<double>(HOP_SIZE) / rate;
}

/** An empty lead of `frames` frames, in the double's frames. */
inline Project makeEmpty(const Project &dub)
{
    Project out;
    auto &audio = out.getAudioData();
    audio.sampleRate = dub.getAudioData().sampleRate;
    const int frames = dub.getAudioData().getNumFrames() > 0
                           ? dub.getAudioData().getNumFrames()
                           : static_cast<int>(dub.getAudioData().basePitch.size());
    audio.basePitch.assign(static_cast<size_t>(std::max(0, frames)), 0.0f);
    audio.deltaPitch.assign(static_cast<size_t>(std::max(0, frames)), 0.0f);
    audio.alignFeatures.assign(static_cast<size_t>(std::max(0, frames)) * TimingAligner::kDims, 0.0f);
    return out;
}

/**
 * Adds one lead clip: its notes, pitch curves and fingerprints (features,
 * from its mel; may be empty) where they sound, moved into the double's
 * frames. Only what the lead clip plays (its span on the timeline) is used.
 */
inline void addClip(Project &out, const Project &lead, const TimingAligner::Features &features,
                    const Placement &leadAt, const Placement &dubAt)
{
    auto &to = out.getAudioData();
    const int frames = static_cast<int>(to.basePitch.size());
    const double hop = frameSeconds(lead);
    const int shift = static_cast<int>(std::lround((leadAt.offsetSeconds - dubAt.offsetSeconds) / hop));
    // Lead frames the clip plays.
    const int first = static_cast<int>(std::ceil((leadAt.startSeconds - leadAt.offsetSeconds) / hop));
    const int last = static_cast<int>(std::floor((leadAt.endSeconds - leadAt.offsetSeconds) / hop));
    const auto &from = lead.getAudioData();
    const int leadFrames = static_cast<int>(std::min(from.basePitch.size(), from.deltaPitch.size()));
    constexpr int dims = TimingAligner::kDims;
    for (int f = std::max(0, first); f < std::min(last, leadFrames); ++f)
    {
        const int g = f + shift;
        if (g < 0 || g >= frames)
            continue;
        to.basePitch[static_cast<size_t>(g)] = from.basePitch[static_cast<size_t>(f)];
        to.deltaPitch[static_cast<size_t>(g)] = from.deltaPitch[static_cast<size_t>(f)];
        if (f < features.frames)
            std::copy(features.at(f), features.at(f) + dims,
                      to.alignFeatures.begin() + static_cast<std::ptrdiff_t>(g) * dims);
    }
    for (const auto &note : lead.getNotes())
    {
        if (note.isRest() || note.getEndFrame() <= first || note.getStartFrame() >= last)
            continue;
        if (note.getEndFrame() + shift <= 0 || note.getStartFrame() + shift >= frames)
            continue;
        Note moved = note;
        const int head = note.getSibilantHead(), tail = note.getSibilantTail();
        const bool known = note.hasSibilantInfo(), byHand = note.getSibilantsSetByHand();
        moved.setSrcStartFrame(note.getSrcStartFrame() + shift);
        moved.setSrcEndFrame(note.getSrcEndFrame() + shift);
        moved.setStartFrame(note.getStartFrame() + shift);
        moved.setEndFrame(note.getEndFrame() + shift);
        if (known)
            moved.setSibilants(head, tail, byHand);
        moved.setWarpAnchors(note.getWarpAnchors());
        moved.setSelected(false);
        out.addNote(std::move(moved));
    }
}
} // namespace LeadProjection
