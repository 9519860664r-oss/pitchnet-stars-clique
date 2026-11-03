#pragma once

#include "../JuceHeader.h"

// Line icons for the Stars Clique skin, drawn from vector paths on a 24 x 24
// grid (the same shapes as the approved design). Stroked icons use a 2 px
// line at 24 px; it scales with the icon.
namespace SkinIcons
{
inline juce::Path parse(const char* d)
{
    return juce::Drawable::parseSVGPath(d);
}

inline void addRoundRect(juce::Path& p, float x, float y, float w, float h, float r)
{
    p.addRoundedRectangle(x, y, w, h, r);
}

inline void addCircle(juce::Path& p, float cx, float cy, float r)
{
    p.addEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
}

/** Builds the icon named `id`. `filled` is set for solid shapes. Returns an
    empty path for an unknown name. */
inline juce::Path build(const juce::String& id, bool& filled, float& strokeWidth)
{
    filled = false;
    strokeWidth = 2.0f;
    juce::Path p;
    if (id == "tracks")
        p = parse("M9 6h12M9 12h12M9 18h12M4 6h.01M4 12h.01M4 18h.01");
    else if (id == "play") {
        p = parse("M7 4l13 8-13 8z");
        filled = true;
    }
    else if (id == "pause") {
        addRoundRect(p, 6.0f, 4.5f, 4.0f, 15.0f, 1.2f);
        addRoundRect(p, 14.0f, 4.5f, 4.0f, 15.0f, 1.2f);
        filled = true;
    }
    else if (id == "stop") {
        addRoundRect(p, 6.5f, 6.5f, 11.0f, 11.0f, 3.0f);
        filled = true;
    }
    else if (id == "loop")
        p = parse("M17 2l4 4-4 4M3 11V9a3 3 0 0 1 3-3h15M7 22l-4-4 4-4M21 13v2a3 3 0 0 1-3 3H3");
    else if (id == "follow")
        p = parse("M3 12h13M12 7l5 5-5 5M20 5v14");
    else if (id == "select") {
        p = parse("M5 3l14 8-6.2 1.8L10 19z");
        filled = true;
    }
    else if (id == "split") {
        p = parse("M20 4L8.1 15.9M14.5 14.5L20 20M8.1 8.1L12 12");
        addCircle(p, 6.0f, 6.0f, 3.0f);
        addCircle(p, 6.0f, 18.0f, 3.0f);
    }
    else if (id == "draw") {
        p = parse("M3 17c3-7 5 2 8-3s4-6 7-4");
        p.addPath(parse("M17 3l4 4-6 6-4-4z"));
    }
    else if (id == "timing")
        p = parse("M12 4v16M3 12h6M15 12h6M6 9l-3 3 3 3M18 9l3 3-3 3");
    else if (id == "chevron") {
        p = parse("M6 9l6 6 6-6");
        strokeWidth = 2.4f;
    }
    else if (id == "centre") {
        addRoundRect(p, 4.0f, 4.0f, 16.0f, 16.0f, 3.0f);
        p.addPath(parse("M12 7v10M9 9.5L12 7l3 2.5M9 14.5L12 17l3-2.5"));
    }
    else if (id == "audition-off") {
        p = parse("M4 9h4l5-4v14l-5-4H4z");
        p.addPath(parse("M17 9l4 6M21 9l-4 6"));
    }
    else if (id == "audition-on") {
        p = parse("M4 9h4l5-4v14l-5-4H4z");
        p.addPath(parse("M16.5 8.5a5 5 0 0 1 0 7M19.5 5.5a9 9 0 0 1 0 13"));
    }
    else if (id == "undo")
        p = parse("M9 14L4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3");
    else if (id == "redo")
        p = parse("M15 14l5-5-5-5M20 9H9a5 5 0 0 0 0 10h3");
    else if (id == "panel") {
        addRoundRect(p, 3.0f, 4.0f, 18.0f, 16.0f, 3.0f);
        p.addPath(parse("M15 4v16"));
    }
    else if (id == "clip") {
        addRoundRect(p, 3.0f, 7.0f, 18.0f, 10.0f, 3.0f);
    }
    else if (id == "track-lanes") {
        addRoundRect(p, 3.0f, 4.0f, 18.0f, 5.0f, 2.0f);
        addRoundRect(p, 3.0f, 15.0f, 18.0f, 5.0f, 2.0f);
    }
    else if (id == "noteoff") {
        p = parse("M9 18V5l10-2v13");
        addCircle(p, 6.0f, 18.0f, 3.0f);
        addCircle(p, 16.0f, 16.0f, 3.0f);
        p.addPath(parse("M3 3l18 18"));
    }
    else if (id == "overview") {
        addRoundRect(p, 3.0f, 5.0f, 18.0f, 14.0f, 3.0f);
        p.addPath(parse("M3 14h18"));
    }
    else if (id == "star") {
        p = parse("M12 2l3 7 7 .6-5.3 4.6 1.7 7.3L12 17.8 5.6 21.5l1.7-7.3L2 9.6 9 9z");
        filled = true;
    }
    return p;
}

/** Draws icon `id`, `size` px square, centred in `area`. */
inline void draw(juce::Graphics& g, const juce::String& id,
                 juce::Rectangle<float> area, float size, juce::Colour colour)
{
    bool filled = false;
    float strokeWidth = 2.0f;
    auto path = build(id, filled, strokeWidth);
    if (path.isEmpty())
        return;
    const float scale = size / 24.0f;
    const auto origin = area.getCentre() - juce::Point<float>(size, size) * 0.5f;
    path.applyTransform(juce::AffineTransform::scale(scale).translated(origin));
    g.setColour(colour);
    if (filled)
        g.fillPath(path);
    else
        g.strokePath(path, juce::PathStrokeType(strokeWidth * scale,
                                                juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
}
}
