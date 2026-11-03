#pragma once

#include "../../JuceHeader.h"
#include <functional>

namespace QuantizePitchDialog {

// Preview arguments: pitch center %, snap to scale, pitch drift %, drift edited.
// Drift is correction strength: 0% original (default), 100% removed.
// The edited flag preserves mixed note drift values until the slider is changed.
// The completion callback receives true only when the user explicitly
// confirms the edit. onPreview updates note positions during dragging;
// onRefreshPreview rebuilds curves and requests audio after release (or a non-drag edit).
void showPopup(juce::Component *parent,
               juce::Rectangle<int> anchorBounds,
               float initialPitchCenter, float initialPitchDrift,
               bool initialSnapToScale, bool initialIncludeManual,
               std::function<void(float, bool, float, bool, bool)> onPreview,
               std::function<void(bool)> onComplete,
               std::function<void()> onRefreshPreview);

// Cancels the active preview, if any, and removes its popup.
void dismissPopup();

// Test scripts: set the open popup's values and press OK.
void setAndAccept(float pitchCenter, bool snapToScale);

} // namespace QuantizePitchDialog
