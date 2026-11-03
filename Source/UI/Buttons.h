/*
  ==============================================================================

    Buttons.h
    Created: 23 Jun 2025 10:21:52am
    Author:  200gaga

  ==============================================================================
*/

#pragma once
#include "../JuceHeader.h"

// How a button looks in the Stars Clique skin when it has a vector icon.
enum class SkinButtonStyle
{
    Plain,  // inside a capsule; switched on = soft white pill, white icon
    Tool,   // edit tool; switched on = light pill with a deep violet icon
    Round,  // free-standing round button with a faint fill
    Record  // pink dot; armed = glowing
};

class Button : public juce::TextButton
{
public:
    Button(juce::Image img = juce::Image());
    void paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void setEnabled (bool shouldBeEnabled);
    void setImage(juce::Image img);
    void setTextPosition(int x, int y);
    // Stars Clique: draw this vector icon (see SkinIcons.h) instead of the sprite.
    void setSkinIcon(const juce::String& id, SkinButtonStyle style, float size = 17.0f);
private:
    juce::Image image;
    int textX, textY;
    juce::String skinIcon;
    SkinButtonStyle skinStyle = SkinButtonStyle::Plain;
    float skinIconSize = 17.0f;
};

class ToggleButton : public juce::ToggleButton
{
public:
    ToggleButton();
    ~ToggleButton();
    void paintButton (juce::Graphics &g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void setEnabled (bool shouldBeEnabled);
    void setImage(juce::Image img);
    // Stars Clique: vector icons for off and on (empty = same as off).
    void setSkinIcon(const juce::String& offId, SkinButtonStyle style,
                     float size = 17.0f, const juce::String& onId = {});
    // Plain style only: false keeps the capsule clear when switched on
    // (play shows a pause icon instead).
    void setSkinOnBackground(bool show) { skinOnBackground = show; }
private:
    juce::Image image;
    juce::String skinIcon, skinIconOn;
    SkinButtonStyle skinStyle = SkinButtonStyle::Plain;
    float skinIconSize = 17.0f;
    bool skinOnBackground = true;
};
