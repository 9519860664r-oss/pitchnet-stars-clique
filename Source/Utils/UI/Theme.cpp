#include "Theme.h"

// Premium dark theme palette — refined depth with warm accent highlights
// Background hierarchy: BACKGROUND < SURFACE < SURFACE_ALT < SURFACE_RAISED
// Each layer is subtly brighter, creating natural elevation perception.

// ── Core backgrounds ──────────────────────────────────────────────

// ── Borders ───────────────────────────────────────────────────────

// ── Piano roll grid & timeline ────────────────────────────────────

// ── Piano keys ────────────────────────────────────────────────────

// ── Text ──────────────────────────────────────────────────────────

// ── Pitch & notes ─────────────────────────────────────────────────

// ── Accent colours ────────────────────────────────────────────────

// ── Waveform ──────────────────────────────────────────────────────

// ── Knobs & shadows ───────────────────────────────────────────────

// ── Alerts ────────────────────────────────────────────────────────

// ── Overlays & selections ─────────────────────────────────────────

// ── Title bar (macOS traffic lights & Windows close hover) ────────

// ── Scrollbars ────────────────────────────────────────────────────

void styleCanvasScrollBar(juce::ScrollBar& scrollBar)
{
    // Colours only - the default look and feel does the drawing, which is what
    // keeps this identical to the piano-roll bars. That renderer paints the
    // thumb and nothing else, so the gutter behind the bar is the host
    // component's job (see PianoRollComponent::paint, DraggablePanel::paint).
    scrollBar.setColour(juce::ScrollBar::thumbColourId, APP_COLOR_SCROLLBAR_THUMB);
    scrollBar.setColour(juce::ScrollBar::trackColourId, APP_COLOR_SCROLLBAR_TRACK);
    scrollBar.setColour(juce::ScrollBar::backgroundColourId, APP_COLOR_SCROLLBAR_TRACK);
}
