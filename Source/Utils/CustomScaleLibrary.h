#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include <vector>

/**
 * The user's saved custom scales, kept in one file for every project and
 * every track: %APPDATA%/PitchNet/custom_scales.json (Windows) or the
 * platform's equivalent.
 */
namespace CustomScaleLibrary
{
inline juce::File file()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("PitchNet")
        .getChildFile("custom_scales.json");
}

inline juce::var toVar(const CustomScale& scale)
{
    auto* object = new juce::DynamicObject();
    object->setProperty("name", scale.name);
    juce::Array<juce::var> used, cents;
    for (size_t i = 0; i < 12; ++i)
    {
        used.add(scale.used[i]);
        cents.add(scale.cents[i]);
    }
    object->setProperty("used", used);
    object->setProperty("cents", cents);
    return juce::var(object);
}

inline CustomScale fromVar(const juce::var& value)
{
    CustomScale scale;
    scale.name = value.getProperty("name", "").toString();
    const auto* used = value.getProperty("used", juce::var()).getArray();
    const auto* cents = value.getProperty("cents", juce::var()).getArray();
    for (int i = 0; i < 12; ++i)
    {
        if (used != nullptr && i < used->size())
            scale.used[static_cast<size_t>(i)] = static_cast<bool>((*used)[i]);
        if (cents != nullptr && i < cents->size())
            scale.cents[static_cast<size_t>(i)] =
                juce::jlimit(-50.0f, 50.0f, static_cast<float>(static_cast<double>((*cents)[i])));
    }
    return scale;
}

inline std::vector<CustomScale> load()
{
    std::vector<CustomScale> scales;
    const auto json = juce::JSON::parse(file());
    if (const auto* list = json.getArray())
        for (const auto& item : *list)
        {
            auto scale = fromVar(item);
            if (scale.name.isNotEmpty())
                scales.push_back(std::move(scale));
        }
    return scales;
}

inline bool save(const std::vector<CustomScale>& scales)
{
    juce::Array<juce::var> list;
    for (const auto& scale : scales)
        list.add(toVar(scale));
    auto target = file();
    target.getParentDirectory().createDirectory();
    return target.replaceWithText(juce::JSON::toString(juce::var(list)));
}

/** Saves `scale` under its name, replacing one of the same name. */
inline bool store(const CustomScale& scale)
{
    auto scales = load();
    bool replaced = false;
    for (auto& existing : scales)
        if (existing.name == scale.name)
        {
            existing = scale;
            replaced = true;
        }
    if (!replaced)
        scales.push_back(scale);
    return save(scales);
}

inline bool remove(const juce::String& name)
{
    auto scales = load();
    scales.erase(std::remove_if(scales.begin(), scales.end(),
                                [&name](const CustomScale& s) { return s.name == name; }),
                 scales.end());
    return save(scales);
}
} // namespace CustomScaleLibrary
