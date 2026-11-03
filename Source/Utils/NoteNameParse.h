#pragma once

#include "../JuceHeader.h"
#include <cmath>
#include <cstring>
#include <optional>

/**
 * Note Inspector pitch entry: a note name ("C3", "F#4",
 * "Bb2 +15") sets the pitch, a signed amount moves it ("+2" semitones,
 * "-30 ct" cents). Octaves as on the piano keys here: C4 is MIDI 60.
 */
namespace NoteNameParse
{
inline juce::String nameFor(float midi)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B"};
    const int nearest = static_cast<int>(std::lround(midi));
    const int cents = static_cast<int>(std::lround((midi - static_cast<float>(nearest)) * 100.0f));
    const int pitchClass = ((nearest % 12) + 12) % 12;
    const int octave = static_cast<int>(std::floor(nearest / 12.0)) - 1;
    juce::String text = juce::String(names[pitchClass]) + juce::String(octave);
    if (cents != 0)
        text << (cents > 0 ? " +" : " ") << cents << " ct";
    return text;
}

// Amount with an optional unit: "ct"/"c"/"cent(s)" for cents, else semitones.
inline std::optional<float> parseAmount(juce::String text)
{
    text = text.trim().toLowerCase().removeCharacters(" ");
    bool cents = false;
    for (const char* unit : {"cents", "cent", "ct", "c", "st"})
        if (text.endsWith(unit))
        {
            cents = juce::String(unit) != "st";
            text = text.dropLastCharacters(static_cast<int>(std::strlen(unit)));
            break;
        }
    if (text.isEmpty() || !text.containsOnly("+-0123456789.,"))
        return std::nullopt;
    const float value = text.replaceCharacter(',', '.').getFloatValue();
    return cents ? value / 100.0f : value;
}

inline std::optional<float> parse(const juce::String& input, float currentMidi)
{
    const auto text = input.trim();
    if (text.isEmpty())
        return std::nullopt;
    if (text[0] == '+' || text[0] == '-')
    {
        if (auto amount = parseAmount(text))
            return currentMidi + *amount;
        return std::nullopt;
    }

    static const int letterSemitones[] = {9, 11, 0, 2, 4, 5, 7}; // A..G
    const juce::juce_wchar letter = juce::CharacterFunctions::toUpperCase(text[0]);
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    int semitone = letterSemitones[letter - 'A'];
    int pos = 1;
    if (pos < text.length() && (text[pos] == '#' || text[pos] == 'b'))
    {
        semitone += text[pos] == '#' ? 1 : -1;
        ++pos;
    }
    // Octave: optional minus, digits.
    int end = pos;
    if (end < text.length() && text[end] == '-')
        ++end;
    while (end < text.length() && juce::CharacterFunctions::isDigit(text[end]))
        ++end;
    const auto octaveText = text.substring(pos, end);
    if (octaveText.isEmpty() || octaveText == "-")
        return std::nullopt;
    float midi = static_cast<float>((octaveText.getIntValue() + 1) * 12 + semitone);
    const auto rest = text.substring(end).trim();
    if (rest.isNotEmpty())
    {
        auto amount = parseAmount(rest);
        if (!amount)
            return std::nullopt;
        // A bare number after a note name is cents ("A3 +15").
        const bool explicitSemitones = rest.toLowerCase().endsWith("st");
        midi += explicitSemitones ? *amount
                                  : (rest.containsAnyOf("cC") ? *amount
                                                               : *amount / 100.0f);
    }
    return midi;
}
} // namespace NoteNameParse
