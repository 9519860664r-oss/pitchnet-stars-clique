#pragma once

#include "../../JuceHeader.h"
#include "Skin.h"

// Theme colours, in the active skin (see Skin.h). Written in the
// Classic tones; Skin::colour maps them.

// UI theme colors (defined in Theme.cpp)
#define APP_COLOR_BACKGROUND (Skin::colour(0xFF1C1C1Cu))
#define APP_COLOR_SURFACE (Skin::colour(0xFF161922u))
#define APP_COLOR_SURFACE_ALT (Skin::colour(0xFF0D0B0Bu))
#define APP_COLOR_SURFACE_RAISED (Skin::colour(0xFF222737u))
#define APP_COLOR_BORDER (Skin::colour(0xFF282E40u))
#define APP_COLOR_BORDER_SUBTLE (Skin::colour(0xFF1E2333u))
#define APP_COLOR_BORDER_HIGHLIGHT (Skin::colour(0xFF3D4562u))
#define APP_COLOR_GRID (Skin::colour(0xFF0D0B0Bu))
#define APP_COLOR_GRID_BAR (Skin::colour(0xFF0D0B0Bu))
#define APP_COLOR_TIMELINE (Skin::colour(0xFF131620u))
#define APP_COLOR_PIANO_WHITE (Skin::colour(0xFF272D40u))
#define APP_COLOR_PIANO_BLACK (Skin::colour(0xFF171B28u))
#define APP_COLOR_PIANO_TEXT (Skin::colour(0xFFD0D6E8u))
#define APP_COLOR_PIANO_TEXT_DIM (Skin::colour(0xFF7E8899u))
#define APP_COLOR_TEXT_PRIMARY (Skin::colour(0xFFECEFF5u))
#define APP_COLOR_TEXT_MUTED (Skin::colour(0xFF8891A6u))
#define APP_COLOR_PITCH_CURVE (Skin::colour(0xFFE6E6E6u))
#define APP_COLOR_NOTE_NORMAL (Skin::colour(0xFF7C6AFFu))
#define APP_COLOR_NOTE_SELECTED (Skin::colour(0xFFA594FFu))
#define APP_COLOR_NOTE_HOVER (Skin::colour(0xFF8E7EFFu))
#define APP_COLOR_PRIMARY (Skin::colour(0xFF7C6AFFu))
#define APP_COLOR_PRIMARY_GLOW (Skin::colour(0xFFBAAEFFu))
#define APP_COLOR_SECONDARY (Skin::colour(0xFF44D1DBu))
#define APP_COLOR_WAVEFORM (Skin::colour(0xFF232940u))
#define APP_COLOR_KNOB_SHADOW (Skin::colour(0xFF08090Du))
#define APP_COLOR_ALERT_WARNING (Skin::colour(0xFFFFB626u))
#define APP_COLOR_ALERT_ERROR (Skin::colour(0xFFFF4D5Au))
#define APP_COLOR_OVERLAY_DIM (Skin::colour(0xC0000008u))
#define APP_COLOR_OVERLAY_SHADOW (Skin::colour(0xA0000008u))
#define APP_COLOR_SELECTION_OVERLAY (Skin::colour(0x18000000u))
#define APP_COLOR_SELECTION_HIGHLIGHT (Skin::colour(0x406C5AFFu))
#define APP_COLOR_SELECTION_HIGHLIGHT_STRONG (Skin::colour(0xC06C5AFFu))
#define APP_COLOR_TITLEBAR_CLOSE_MAC (Skin::colour(0xFFFF5F57u))
#define APP_COLOR_TITLEBAR_MINIMIZE_MAC (Skin::colour(0xFFFEBC2Eu))
#define APP_COLOR_TITLEBAR_MAXIMIZE_MAC (Skin::colour(0xFF28C840u))
#define APP_COLOR_TITLEBAR_CLOSE_HOVER (Skin::colour(0xFFE81123u))

// ── Scrollbars ────────────────────────────────────────────────────
// One look for every scrollbar in the app: a dim grey rounded thumb over a
// recessed gutter. The piano-roll canvas set the look; styleCanvasScrollBar()
// is how everything else adopts it, so the side panel and the canvas cannot
// drift apart. The default JUCE renderer draws the thumb only - whoever hosts
// the bar fills APP_COLOR_SCROLLBAR_TRACK behind it.
#define APP_COLOR_SCROLLBAR_THUMB (Skin::colour(0xFF565656u))
#define APP_COLOR_SCROLLBAR_TRACK (Skin::colour(0xFF0D0B0Bu))
constexpr int APP_SCROLLBAR_THICKNESS = 8;

void styleCanvasScrollBar(juce::ScrollBar& scrollBar);
