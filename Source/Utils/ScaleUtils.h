#pragma once

#include "../Models/Project.h"
#include "Constants.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>

namespace ScaleUtils
{
inline constexpr std::uint16_t intervalBit(int semitone)
{
    return static_cast<std::uint16_t>(1u << semitone);
}

inline constexpr std::uint16_t getIntervalMaskForMode(ScaleMode mode)
{
    switch (mode) {
    case ScaleMode::Major:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Minor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Dorian:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Phrygian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Lydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::Mixolydian:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(10);
    case ScaleMode::Locrian:
        return intervalBit(0) | intervalBit(1) | intervalBit(3) |
               intervalBit(5) | intervalBit(6) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::Blues:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(6) | intervalBit(7) | intervalBit(10);
    case ScaleMode::HarmonicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(11);
    case ScaleMode::MajorPentatonic:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(7) | intervalBit(9);
    case ScaleMode::MelodicMinor:
        return intervalBit(0) | intervalBit(2) | intervalBit(3) |
               intervalBit(5) | intervalBit(7) | intervalBit(9) |
               intervalBit(11);
    case ScaleMode::MinorPentatonic:
        return intervalBit(0) | intervalBit(3) | intervalBit(5) |
               intervalBit(7) | intervalBit(10);
    case ScaleMode::PhrygianDominant:
        return intervalBit(0) | intervalBit(1) | intervalBit(4) |
               intervalBit(5) | intervalBit(7) | intervalBit(8) |
               intervalBit(10);
    case ScaleMode::WholeTone:
        return intervalBit(0) | intervalBit(2) | intervalBit(4) |
               intervalBit(6) | intervalBit(8) | intervalBit(10);
    case ScaleMode::None:
        return 0;
    case ScaleMode::Chromatic:
        return 0x0FFFu;
    case ScaleMode::Custom:
        return 0x0FFFu; // absolute notes: see isPitchClassInScale
    }
    return 0;
}

// The user's custom scale, shared by every track.
inline const CustomScale& customScale()
{
    return sharedMacroParameters()->customScale;
}

inline bool isPitchClassInScale(ScaleMode mode, int pitchClass, int rootNote)
{
    if (mode == ScaleMode::None || rootNote < 0)
        return false;
    if (mode == ScaleMode::Chromatic)
        return true;
    if (mode == ScaleMode::Custom)
        return customScale().used[static_cast<size_t>((pitchClass % 12 + 12) % 12)];

    const int normalizedPitch = (pitchClass % 12 + 12) % 12;
    const int normalizedRoot = (rootNote % 12 + 12) % 12;
    const int relativeSemitone = (normalizedPitch - normalizedRoot + 12) % 12;
    const auto intervalMask = getIntervalMaskForMode(mode);
    return (intervalMask & intervalBit(relativeSemitone)) != 0;
}

inline bool detectMajorOrRelativeMinor(const Project& project,
                                       int& detectedRootNote,
                                       ScaleMode& detectedMode)
{
    std::array<int, 12> pitchClassCounts {};
    int noteCount = 0;

    for (const auto& note : project.getNotes()) {
        if (note.isRest())
            continue;

        const int roundedMidi = static_cast<int>(std::round(note.getMidiNote()));
        const int pitchClass = (roundedMidi % 12 + 12) % 12;
        ++pitchClassCounts[static_cast<size_t>(pitchClass)];
        ++noteCount;
    }

    if (noteCount == 0)
        return false;

    int bestMajorRoot = 0;
    int bestFitCount = -1;
    for (int candidateRoot = 0; candidateRoot < 12; ++candidateRoot) {
        int fitCount = 0;
        for (int pitchClass = 0; pitchClass < 12; ++pitchClass) {
            if (isPitchClassInScale(ScaleMode::Major, pitchClass,
                                    candidateRoot))
                fitCount += pitchClassCounts[static_cast<size_t>(pitchClass)];
        }

        if (fitCount > bestFitCount) {
            bestFitCount = fitCount;
            bestMajorRoot = candidateRoot;
        }
    }

    const int relativeMinorRoot = (bestMajorRoot + 9) % 12;
    const int majorRootCount =
        pitchClassCounts[static_cast<size_t>(bestMajorRoot)];
    const int minorRootCount =
        pitchClassCounts[static_cast<size_t>(relativeMinorRoot)];

    if (minorRootCount > majorRootCount) {
        detectedRootNote = relativeMinorRoot;
        detectedMode = ScaleMode::Minor;
    } else {
        detectedRootNote = bestMajorRoot;
        detectedMode = ScaleMode::Major;
    }

    return true;
}

inline void detectAndApplyScale(Project& project)
{
    int rootNote = 0;
    ScaleMode mode = ScaleMode::Major;
    if (!detectMajorOrRelativeMinor(project, rootNote, mode))
        return;

    project.setDetectedKey(rootNote, mode);

    // Detection is a suggestion, not an explicit region override. Seed the
    // default once for standalone projects; the plugin adopts its first result.
    if (!project.getMacroParameters()->projectKeyInitialized)
        project.setProjectKey(rootNote, mode);
}

/** The key in force at a time on the host timeline: the last key change at
    or before it, else the project's key. */
struct Key
{
    int root = 0;
    ScaleMode mode = ScaleMode::Major;
};

inline Key keyAt(const Project& project, double seconds)
{
    // The key shown on the key button (shared by every track), whatever the
    // old "scale view" switch says; None still means no key at all.
    const auto view = project.getScaleMode();
    Key key{project.getScaleRootNote(),
            view == ScaleMode::None ? ScaleMode::None : project.getPreferredScaleMode()};
    for (const auto& marker : project.getMacroParameters()->keyMarkers)
    {
        if (marker.seconds > seconds + 1.0e-9)
            break;
        key = {marker.root, marker.mode};
    }
    return key;
}

/** Cents the custom scale tunes a note to, when it is the key in force. */
inline float customCents(int midiNote)
{
    return customScale().cents[static_cast<size_t>((midiNote % 12 + 12) % 12)];
}

inline float getReferenceOffsetSemitones(int referenceHz)
{
    const int normalized = juce::jlimit(400, 480, referenceHz);
    return 12.0f * std::log2(static_cast<float>(normalized) / FREQ_A4);
}

inline float snapMidiToSemitone(float midi, int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    return std::round(midi - offset) + offset;
}

/** Nearest semitone, tuned by the custom scale's cents when the custom
    scale is the key (`mode`). Every note of the octave is a target. */
inline float snapMidiToTunedSemitone(float midi, ScaleMode mode,
                                     int referenceHz = static_cast<int>(FREQ_A4))
{
    if (mode != ScaleMode::Custom)
        return snapMidiToSemitone(midi, referenceHz);
    const float offset = getReferenceOffsetSemitones(referenceHz);
    const float normalized = midi - offset;
    const int centre = static_cast<int>(std::round(normalized));
    float best = static_cast<float>(centre), bestDistance = std::numeric_limits<float>::max();
    for (int candidate = centre - 1; candidate <= centre + 1; ++candidate)
    {
        const float target = static_cast<float>(candidate) + customCents(candidate) / 100.0f;
        if (std::abs(target - normalized) < bestDistance)
        {
            bestDistance = std::abs(target - normalized);
            best = target;
        }
    }
    return best + offset;
}

inline float snapMidiToScale(float midi,
                             ScaleMode mode,
                             int rootNote,
                             int referenceHz = static_cast<int>(FREQ_A4))
{
    const float offset = getReferenceOffsetSemitones(referenceHz);
    const float normalizedMidi = midi - offset;
    const int roundedMidi = static_cast<int>(std::round(normalizedMidi));
    if (mode == ScaleMode::None || mode == ScaleMode::Chromatic || rootNote < 0)
        return snapMidiToSemitone(midi, referenceHz);

    constexpr int kSearchRadius = 24;
    constexpr float kTieEpsilon = 1.0e-4f;

    int bestMidi = roundedMidi;
    float bestDistance = std::numeric_limits<float>::max();
    int bestRoundedDistance = std::numeric_limits<int>::max();

    const int searchStart = roundedMidi - kSearchRadius;
    const int searchEnd = roundedMidi + kSearchRadius;
    const bool custom = mode == ScaleMode::Custom;
    float bestTarget = static_cast<float>(roundedMidi);
    for (int candidate = searchStart; candidate <= searchEnd; ++candidate) {
        if (!isPitchClassInScale(mode, candidate, rootNote))
            continue;

        const float target = static_cast<float>(candidate) +
                             (custom ? customCents(candidate) / 100.0f : 0.0f);
        const float distance = std::abs(target - normalizedMidi);
        const int roundedDistance = std::abs(candidate - roundedMidi);
        const bool isCloser = distance < bestDistance - kTieEpsilon;
        const bool isDistanceTie = std::abs(distance - bestDistance) <= kTieEpsilon;
        const bool betterRoundedDistance =
            roundedDistance < bestRoundedDistance;
        const bool lowerPitchTie =
            roundedDistance == bestRoundedDistance && candidate < bestMidi;

        if (isCloser || (isDistanceTie && (betterRoundedDistance || lowerPitchTie))) {
            bestDistance = distance;
            bestRoundedDistance = roundedDistance;
            bestMidi = candidate;
            bestTarget = target;
        }
    }

    return bestTarget + offset;
}

/** True when `key` is a real key (not None or Chromatic). */
inline bool hasKey(const Key& key)
{
    return key.mode == ScaleMode::Custom ||
           (key.mode != ScaleMode::None && key.mode != ScaleMode::Chromatic && key.root >= 0);
}

/** Where pitch correction moves a note. "Tune to key" on (and a key set):
    the nearest note of the key, a custom scale with its cents. Off: the
    nearest semitone (tuned by the custom scale's cents when that is the key).
    Both follow the reference pitch A4. */
inline float pitchCorrectionTarget(float midi, const Key& key, bool toKey, int referenceHz)
{
    if (toKey && hasKey(key))
        return snapMidiToScale(midi, key.mode, key.root, referenceHz);
    return snapMidiToTunedSemitone(midi, key.mode, referenceHz);
}

/** How far a note misses the key, 0..1: 0 exactly on a note of the key
    (custom scales with their cents), 1 at half a semitone or more away, so a
    note on a semitone outside the key is 1. Without a key, the distance to the
    nearest semitone. Drives the note colour: blue, violet, pink, red. */
inline float keyMissAmount(float midi, const Key& key, int referenceHz)
{
    const float target = hasKey(key) ? snapMidiToScale(midi, key.mode, key.root, referenceHz)
                                     : snapMidiToSemitone(midi, referenceHz);
    return juce::jlimit(0.0f, 1.0f, std::abs(midi - target) / 0.5f);
}

}
