#pragma once

#include "../../JuceHeader.h"

/**
 * Small view choices kept between sessions and shared by every window
 * (%APPDATA%/PitchNet/view.json), such as "Show Sibilants".
 */
namespace ViewPrefs
{
inline juce::File file()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PitchNet")
        .getChildFile("view.json");
}

inline juce::var &cache()
{
    static juce::var values = [] {
        auto parsed = juce::JSON::parse(file());
        return parsed.isObject() ? parsed : juce::var(new juce::DynamicObject());
    }();
    return values;
}

inline bool getBool(const juce::Identifier &name, bool fallback)
{
    const auto value = cache().getProperty(name, juce::var());
    return value.isVoid() ? fallback : static_cast<bool>(value);
}

inline void setBool(const juce::Identifier &name, bool value)
{
    if (auto *object = cache().getDynamicObject())
    {
        object->setProperty(name, value);
        file().getParentDirectory().createDirectory();
        file().replaceWithText(juce::JSON::toString(cache()));
    }
}

inline bool showSibilants() { return getBool("showSibilants", true); }
inline void setShowSibilants(bool show) { setBool("showSibilants", show); }
} // namespace ViewPrefs
