/*
  ==============================================================================

    Buttons.cpp
    Created: 23 Jun 2025 10:21:52am
    Author:  200gaga

  ==============================================================================
*/

#include "Buttons.h"
#include "../Utils/UI/Skin.h"
#include "SkinIcons.h"

namespace
{
// Stars Clique look for a button with a vector icon (the approved design).
void paintSkinned(juce::Graphics& g, juce::Component& c, const juce::String& icon,
                  SkinButtonStyle style, float size, bool on, bool hover, bool down,
                  bool showOnBackground)
{
    const bool enabled = c.isEnabled();
    const auto area = c.getLocalBounds().toFloat();
    const float corner = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
    const auto lavender = juce::Colour(0xFFEDE4FAu);
    const auto muted = juce::Colour(0xFF9E8FBCu);
    auto iconColour = !enabled ? muted : (on || hover) ? juce::Colours::white : lavender;

    switch (style)
    {
    case SkinButtonStyle::Round:
        g.setColour(juce::Colours::white.withAlpha(!enabled ? 0.05f
                                                   : (on || down) ? 0.20f
                                                   : hover ? 0.14f : 0.08f));
        g.fillRoundedRectangle(area, corner);
        break;
    case SkinButtonStyle::Tool:
        if (on)
        {
            g.setColour(juce::Colour(0xFFD8B4FEu).withAlpha(0.16f * Skin::glowLevel));
            g.fillRoundedRectangle(area.expanded(1.5f), corner + 1.5f);
            g.setColour(juce::Colours::white.withAlpha(0.88f));
            g.fillRoundedRectangle(area, corner);
            iconColour = juce::Colour(0xFF3B1773u);
        }
        else if (hover)
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(area, corner);
        }
        break;
    case SkinButtonStyle::Plain:
        if (on && showOnBackground)
        {
            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRoundedRectangle(area, corner);
        }
        else if (hover && enabled)
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(area, corner);
        }
        break;
    case SkinButtonStyle::Record:
    {
        const auto pink = juce::Colour(0xFFFF6B9Au);
        if (on)
        {
            g.setColour(pink.withAlpha(0.22f));
            g.fillRoundedRectangle(area, corner);
        }
        else if (hover && enabled)
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(area, corner);
        }
        const auto centre = area.getCentre();
        const float dot = on ? 14.0f : 13.0f;
        g.setColour(pink.withAlpha(on ? 0.40f : 0.22f));
        g.fillEllipse(juce::Rectangle<float>(dot + 7.0f, dot + 7.0f).withCentre(centre));
        g.setColour(enabled ? (on ? pink.brighter(0.15f) : pink) : pink.withAlpha(0.45f));
        g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre(centre));
        return;
    }
    }
    SkinIcons::draw(g, icon, area, size, iconColour);
}
}

void Button::setSkinIcon(const juce::String& id, SkinButtonStyle style, float size)
{
    skinIcon = id;
    skinStyle = style;
    skinIconSize = size;
    repaint();
}

void ToggleButton::setSkinIcon(const juce::String& offId, SkinButtonStyle style,
                               float size, const juce::String& onId)
{
    skinIcon = offId;
    skinIconOn = onId;
    skinStyle = style;
    skinIconSize = size;
    repaint();
}

Button::Button(juce::Image img)
:image(img), textX(0), textY(0)
{
    if (img.isValid())
        setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 3 / 2);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void Button::paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    if (skinIcon.isNotEmpty() && Skin::starsClique())
    {
        paintSkinned(g, *this, skinIcon, skinStyle, skinIconSize, false,
                     shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown, true);
        return;
    }
    if (image.isValid() && Skin::starsClique() && getComponentID() != "skin-keep-colours")
    {
        // Stars Clique: the icon's shape in light lavender (white on hover).
        if (shouldDrawButtonAsHighlighted && isEnabled())
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f),
                                   juce::jmin(getWidth(), getHeight()) * 0.5f);
        }
        g.setColour(!isEnabled() ? juce::Colour(0x809E8FBCu)
                    : shouldDrawButtonAsHighlighted ? juce::Colours::white
                                                    : juce::Colour(0xFFEDE4FAu));
        g.drawImage(image, (getWidth() - image.getWidth() / 2) / 2, (getHeight() - image.getHeight() / 3 / 2) / 2, image.getWidth() / 2, image.getHeight() / 3 / 2, 0, 0, image.getWidth(), image.getHeight() / 3, true);
    }
    else if (image.isValid())
        g.drawImage(image, (getWidth() - image.getWidth() / 2) / 2, (getHeight() - image.getHeight() / 3 / 2) / 2, image.getWidth() / 2, image.getHeight() / 3 / 2, 0, image.getHeight() * (shouldDrawButtonAsDown || !isEnabled() ? 2 : (shouldDrawButtonAsHighlighted ? 1 : 0)) / 3, image.getWidth(), image.getHeight() / 3);
    else
    {
        auto baseColour = findColour(juce::TextButton::buttonColourId).withMultipliedSaturation (hasKeyboardFocus (true) ? 1.3f : 0.9f).withMultipliedAlpha (isEnabled() ? 1.0f : 0.5f);
        if ((shouldDrawButtonAsDown || shouldDrawButtonAsHighlighted))
            baseColour = baseColour.contrasting (shouldDrawButtonAsDown ? 0.2f : 0.05f);
        g.setColour(shouldDrawButtonAsDown ? findColour(juce::TextButton::buttonOnColourId) : baseColour);
        g.fillRoundedRectangle(0, 0, getWidth(), getHeight(), getWidth() > 40 ? 13.f : 3.5f);
    }
    g.setFont(getLookAndFeel().getTextButtonFont(*this, getHeight()));
    g.setColour(isEnabled() ? findColour(shouldDrawButtonAsHighlighted ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId) : Skin::colour(0xFF3E3E3Eu));
    g.drawText(getButtonText(), getLocalBounds().withX(textX).withY(textY), juce::Justification::centred);
}

void Button::setEnabled (bool shouldBeEnabled)
{
    juce::TextButton::setEnabled(shouldBeEnabled);
    setMouseCursor(shouldBeEnabled ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void Button::setImage(juce::Image img)
{
    image = img;
    setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 3 / 2);
    repaint();
}

void Button::setTextPosition(int x, int y)
{
    textX = x;
    textY = y;
}

ToggleButton::ToggleButton()
{
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

ToggleButton::~ToggleButton()
{
    
}

void ToggleButton::setEnabled (bool shouldBeEnabled)
{
    juce::ToggleButton::setEnabled(shouldBeEnabled);
    setMouseCursor(shouldBeEnabled ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void ToggleButton::setImage(juce::Image img)
{
    image = img;
    setSize((img.getWidth() + 1) / 2, (img.getHeight() + 1) / 4 / 2);
    repaint();
}

void ToggleButton::paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    if (skinIcon.isNotEmpty() && Skin::starsClique())
    {
        setAlpha(isEnabled() ? 1.f : 0.45f);
        const bool on = getToggleState();
        paintSkinned(g, *this, on && skinIconOn.isNotEmpty() ? skinIconOn : skinIcon,
                     skinStyle, skinIconSize, on, shouldDrawButtonAsHighlighted,
                     shouldDrawButtonAsDown, skinOnBackground);
        return;
    }
    setAlpha(isEnabled() ? 1.f : 0.3f);
    if (image.isValid() && Skin::starsClique())
    {
        // Stars Clique: a switched-on tool sits on a light capsule with a deep
        // violet icon; otherwise the icon is light lavender.
        const auto area = getLocalBounds().toFloat().reduced(1.0f);
        const float corner = juce::jmin(area.getWidth(), area.getHeight()) * 0.5f;
        if (getToggleState())
        {
            g.setColour(juce::Colours::white.withAlpha(0.88f));
            g.fillRoundedRectangle(area, corner);
        }
        else if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.fillRoundedRectangle(area, corner);
        }
        g.setColour(getToggleState() ? juce::Colour(0xFF3B1773u) : juce::Colour(0xFFEDE4FAu));
        g.drawImage(image, (getWidth() - (image.getWidth() + 1) / 2) / 2, (getHeight() - (image.getHeight() / 4 + 1) / 2) / 2, (image.getWidth() + 1) / 2, (image.getHeight() / 4  + 1) / 2, 0, 0, image.getWidth(), image.getHeight() / 4, true);
    }
    else if (image.isValid())
    {
        g.drawImage(image, (getWidth() - (image.getWidth() + 1) / 2) / 2, (getHeight() - (image.getHeight() / 4 + 1) / 2) / 2, (image.getWidth() + 1) / 2, (image.getHeight() / 4  + 1) / 2, 0, ((shouldDrawButtonAsHighlighted ? 1 : 0) + (getToggleState() ? 2 : 0)) * image.getHeight() / 4 , image.getWidth(), image.getHeight() / 4);
    }
    else
    {
        g.setColour(juce::Colour(shouldDrawButtonAsHighlighted ? 0xFF727272 : 0xFF565656));
        g.fillRoundedRectangle(0, 0, getWidth(), getHeight(), getWidth() > 40 ? 13.f : 3.5f);
    }
    g.setFont(juce::Font("Montserrat", "Medium", 14.f).withPointHeight(14.f));
    g.setColour(juce::Colour(getToggleState() ? 0xFFE6E6E6 : 0xFF9B9B9B));
    g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
}