#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "Localization.h"
#include <array>

/** Display names of keys and scales, shared by the menus that pick them. */
namespace ScaleNames
{
struct ModeOption
{
    ScaleMode mode;
    const char* labelKey; // looked up in the string table
};

inline const std::array<const char*, 12>& rootLabels()
{
    static const std::array<const char*, 12> labels{
        {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}};
    return labels;
}

inline const std::array<ModeOption, 14>& modeOptions()
{
    static const std::array<ModeOption, 14> options{{
        {ScaleMode::Major, "scale.major"},
        {ScaleMode::Minor, "scale.minor"},
        {ScaleMode::Blues, "scale.blues"},
        {ScaleMode::Dorian, "scale.dorian"},
        {ScaleMode::HarmonicMinor, "scale.harmonic_minor"},
        {ScaleMode::Locrian, "scale.locrian"},
        {ScaleMode::Lydian, "scale.lydian"},
        {ScaleMode::MajorPentatonic, "scale.major_pentatonic"},
        {ScaleMode::MelodicMinor, "scale.melodic_minor"},
        {ScaleMode::MinorPentatonic, "scale.minor_pentatonic"},
        {ScaleMode::Mixolydian, "scale.mixolydian"},
        {ScaleMode::Phrygian, "scale.phrygian"},
        {ScaleMode::PhrygianDominant, "scale.phrygian_dominant"},
        {ScaleMode::WholeTone, "scale.whole_tone"},
    }};
    return options;
}

inline juce::String rootName(int root)
{
    return rootLabels()[static_cast<size_t>(((root % 12) + 12) % 12)];
}

/** "D Minor", or the custom scale's own name. */
inline juce::String keyName(int root, ScaleMode mode)
{
    if (mode == ScaleMode::Custom)
    {
        const auto& name = sharedMacroParameters()->customScale.name;
        return name.isNotEmpty() ? name : TR("scale.custom");
    }
    for (const auto& option : modeOptions())
        if (option.mode == mode)
            return rootName(root) + " " + TR(option.labelKey);
    return rootName(root);
}
} // namespace ScaleNames
