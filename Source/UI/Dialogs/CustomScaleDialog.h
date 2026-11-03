#pragma once

#include "../../JuceHeader.h"
#include <functional>

/**
 * Custom scale editor: pick the notes the scale uses, detune any note by
 * cents, name it and save it to the library. "Use" makes it the key of every
 * track (notes are corrected and snapped to its tuning).
 */
namespace CustomScaleDialog
{
// onApplied runs after "Use" puts the scale in force.
void showPopup(juce::Component* parent, juce::Rectangle<int> anchorBounds,
               std::function<void()> onApplied);

void dismissPopup();
} // namespace CustomScaleDialog
