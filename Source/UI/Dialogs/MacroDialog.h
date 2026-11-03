#pragma once

#include "../../JuceHeader.h"
#include <functional>
#include <map>
#include <vector>

/**
 * A small popup for a macro, in the style of the Correct Pitch popup: a
 * column of sliders, choices and switches, previewed live, with Cancel / OK.
 * Used by Quantize Time, Align to Reference and Leveling.
 */
namespace MacroDialog {

struct Field
{
    enum class Kind { Slider, Choice, Toggle };
    Kind kind = Kind::Slider;
    juce::String id;
    juce::String label;
    double min = 0.0, max = 100.0, step = 1.0;
    double value = 0.0;            // slider value, choice index, toggle 0/1
    juce::String suffix;           // slider unit, e.g. " %"
    juce::StringArray options;     // choice items
};

using Values = std::map<juce::String, double>;

// onPreview(values, dragging): called on every change; `dragging` is true
// while a slider is held (update visuals only), false otherwise (render).
// onComplete(accepted, values): OK (true) or Cancel / dismissal (false).
void showPopup(juce::Component* parent, juce::Rectangle<int> anchorBounds,
               const juce::String& title, std::vector<Field> fields,
               std::function<void(const Values&, bool)> onPreview,
               std::function<void(bool, const Values&)> onComplete);

// Cancels the active popup, if any.
void dismissPopup();

// Presses OK on the active popup (test scripts).
void acceptPopup();

// Sets a field of the active popup by id (test scripts).
void setValue(const juce::String& id, double value);

} // namespace MacroDialog
