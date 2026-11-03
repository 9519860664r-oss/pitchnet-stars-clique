#pragma once

#include "../JuceHeader.h"
#include "../Utils/UI/Skin.h"
#include "SkinIcons.h"
#include <functional>
#include <optional>

class PianoRollComponent;

/**
 * Note Inspector: a strip above the editor with the selected
 * notes' properties, each one editable by typing.
 *
 *   Pitch [A3 +12 ct] 222.5 Hz | Modulation [100 %] | Drift [100 %]
 *   | Formant [0.0 st] | Amplitude [0.0 dB] | [Note Off]
 *
 * Pitch takes a note name ("C3", "F#4 +10") or a signed amount ("+2", "-30 ct").
 * When the selected notes differ, a field shows "–"; typing into it sets
 * every selected note. Nothing selected: the strip is empty.
 */
class NoteInspector : public juce::Component
{
public:
    explicit NoteInspector(PianoRollComponent& pianoRoll);

    /** Re-reads the selection (cheap when nothing changed). */
    void refresh();

    void paint(juce::Graphics& g) override;
    void lookAndFeelChanged() override;
    void resized() override;

    static constexpr int preferredHeight = 30;
    // 48 px capsule in Stars Clique, as in the design.
    static int preferredHeightForSkin() { return Skin::starsClique() ? 48 : preferredHeight; }

private:
    // Text button in Classic; a round icon button in Stars Clique.
    struct NoteOffButton : juce::TextButton
    {
        bool muted = false;
        void paintButton(juce::Graphics& g, bool hover, bool down) override
        {
            if (!Skin::starsClique())
            {
                juce::TextButton::paintButton(g, hover, down);
                return;
            }
            const auto r = getLocalBounds().toFloat();
            g.setColour(juce::Colours::white.withAlpha(muted ? 0.30f : (hover || down) ? 0.18f : 0.12f));
            g.fillEllipse(r);
            SkinIcons::draw(g, "noteoff", r, 17.0f,
                            juce::Colours::white.withAlpha(isEnabled() ? 1.0f : 0.45f));
        }
    };

    struct Field
    {
        juce::Label caption;
        juce::TextEditor editor;
        juce::String suffix;
    };

    void setupField(Field& field, const juce::String& caption, const juce::String& suffix,
                    std::function<void(const juce::String&)> onCommit);
    void showValue(Field& field, std::optional<float> value, int decimals);
    juce::String selectionSignature() const;

    PianoRollComponent& pianoRoll;
    Field pitch, modulation, drift, formant, amplitude;
    juce::Label hz;
    NoteOffButton noteOff;
    juce::Label empty;
    // Never equal to a real signature, so the first refresh lays out the strip.
    juce::String lastSignature{"<none>"};
};
