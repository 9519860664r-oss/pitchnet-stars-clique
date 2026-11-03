#pragma once

#include "../../JuceHeader.h"

/**
 * UI skins: Classic and Stars Clique. Every colour of the interface is
 * written in the Classic skin's tones; Skin::colour() turns one into the
 * active skin's. Stars Clique moves the neutral greys to violets and the
 * orange accent to lavender, and leaves the notes' pitch colours as they
 * are. The choice is saved for every project.
 */
namespace Skin
{
enum class Id
{
    Classic = 0,
    // Stars Clique: dark violet, Apple visionOS-like: a soft glow behind
    // translucent panels, a floating toolbar capsule, light icons.
    StarsClique = 2
};

Id active();
inline bool starsClique() { return active() == Id::StarsClique; }

/** Stars Clique glass panels (fill, edge, top highlight): every alpha is
    scaled by this, so the glass can be made lighter or darker in one place. */
inline constexpr float glassLevel = 0.65f;
inline juce::Colour glass(float alpha) { return juce::Colours::white.withAlpha(alpha * glassLevel); }
/** Soft glows around active controls, scaled the same way. */
inline constexpr float glowLevel = 0.5f;

/** Stars Clique: the window's glowing backdrop, as seen by `component` (the
    same picture behind every panel). Drawn from a cached image, so it is
    cheap to repaint. */
void paintBackdrop(juce::Graphics& g, const juce::Component& component);
void setActive(Id skin);
juce::String name(Id skin);

/** A colour written for the Classic skin, in the active skin. */
juce::Colour colour(juce::uint32 classicArgb);
} // namespace Skin
