#include "CustomScaleDialog.h"

#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Models/Project.h"
#include "../../Utils/CustomScaleLibrary.h"
#include "../../Utils/ScaleNames.h"
#include "../../Utils/UI/Theme.h"

#include <array>
#include <memory>

namespace CustomScaleDialog
{
namespace
{
class Content final : public juce::Component
{
public:
    explicit Content(std::function<void()> applied) : onApplied(std::move(applied))
    {
        scale = sharedMacroParameters()->customScale;

        title.setText("Custom scale", juce::dontSendNotification);
        title.setFont(AppFont::getFont(16.0f));
        title.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        addAndMakeVisible(title);

        hint.setText("Notes on = in the scale. Cents tune each note (-50 ... +50).",
                     juce::dontSendNotification);
        hint.setFont(AppFont::getFont(12.0f));
        hint.setColour(juce::Label::textColourId, APP_COLOR_TEXT_MUTED);
        addAndMakeVisible(hint);

        nameEditor.setText(scale.name.isNotEmpty() ? scale.name : "My scale", false);
        nameEditor.setTextToShowWhenEmpty("Name", APP_COLOR_TEXT_MUTED);
        nameEditor.setColour(juce::TextEditor::backgroundColourId,
                             juce::Colours::black.withAlpha(0.3f));
        nameEditor.setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
        nameEditor.setIndents(6, 4);
        addAndMakeVisible(nameEditor);

        presets.setTextWhenNothingSelected("Saved scales");
        refreshPresets();
        presets.onChange = [this] {
            const int index = presets.getSelectedItemIndex();
            const auto saved = CustomScaleLibrary::load();
            if (index >= 0 && index < static_cast<int>(saved.size()))
            {
                scale = saved[static_cast<size_t>(index)];
                showScale();
            }
        };
        addAndMakeVisible(presets);

        for (int i = 0; i < 12; ++i)
        {
            auto& row = rows[static_cast<size_t>(i)];
            row.note.setButtonText(ScaleNames::rootName(i));
            row.note.setClickingTogglesState(true);
            row.note.setColour(juce::TextButton::buttonColourId, Skin::colour(0xFF2A2A2Au));
            row.note.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF3D7BD9));
            row.note.setColour(juce::TextButton::textColourOffId, APP_COLOR_TEXT_MUTED);
            row.note.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
            row.note.onClick = [this, i] {
                scale.used[static_cast<size_t>(i)] = rows[static_cast<size_t>(i)].note.getToggleState();
            };
            addAndMakeVisible(row.note);

            row.cents.setRange(-50.0, 50.0, 0.5);
            row.cents.setTextValueSuffix(" ct");
            row.cents.setDoubleClickReturnValue(true, 0.0);
            row.cents.onValueChange = [this, i] {
                scale.cents[static_cast<size_t>(i)] =
                    static_cast<float>(rows[static_cast<size_t>(i)].cents.getValue());
            };
            addAndMakeVisible(row.cents);
        }

        for (auto* button : {&saveButton, &deleteButton, &useButton, &closeButton})
        {
            button->setLookAndFeel(&DarkLookAndFeel::getInstance());
            button->setColour(juce::TextButton::buttonColourId, Skin::colour(0xFF5B5B5Bu));
            button->setColour(juce::TextButton::textColourOffId, Skin::colour(0xFFEFEFEFu));
            addAndMakeVisible(*button);
        }
        saveButton.setButtonText("Save");
        deleteButton.setButtonText("Delete");
        useButton.setButtonText("Use");
        closeButton.setButtonText("Close");
        useButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF3D7BD9));
        saveButton.onClick = [this] {
            scale.name = nameEditor.getText().trim();
            if (scale.name.isEmpty())
                scale.name = "My scale";
            nameEditor.setText(scale.name, false);
            CustomScaleLibrary::store(scale);
            refreshPresets();
        };
        deleteButton.onClick = [this] {
            CustomScaleLibrary::remove(nameEditor.getText().trim());
            refreshPresets();
        };
        useButton.onClick = [this] {
            scale.name = nameEditor.getText().trim();
            auto& shared = *sharedMacroParameters();
            shared.customScale = scale;
            shared.projectKeyMode = ScaleMode::Custom;
            shared.projectKeyInitialized = true;
            if (shared.scaleMode != ScaleMode::None && shared.scaleMode != ScaleMode::Chromatic)
                shared.scaleMode = ScaleMode::Custom;
            if (onApplied)
                onApplied();
        };
        closeButton.onClick = [this] { close(); };
        showScale();
    }

    void close()
    {
        auto safe = juce::Component::SafePointer<Content>(this);
        juce::MessageManager::callAsync([safe] {
            if (auto* content = safe.getComponent())
            {
                if (auto* parent = content->getParentComponent())
                    parent->removeChildComponent(content);
                delete content;
            }
        });
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(Skin::colour(0xFF181818u));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
        g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 12);
        title.setBounds(area.removeFromTop(24));
        hint.setBounds(area.removeFromTop(18));
        area.removeFromTop(6);
        auto top = area.removeFromTop(26);
        nameEditor.setBounds(top.removeFromLeft(top.getWidth() / 2 - 4));
        top.removeFromLeft(8);
        presets.setBounds(top);
        area.removeFromTop(10);
        // Two columns of six notes.
        auto columns = area.removeFromTop(6 * 30);
        auto left = columns.removeFromLeft(columns.getWidth() / 2 - 6);
        columns.removeFromLeft(12);
        for (int i = 0; i < 12; ++i)
        {
            auto& column = i < 6 ? left : columns;
            auto row = column.removeFromTop(30).reduced(0, 3);
            rows[static_cast<size_t>(i)].note.setBounds(row.removeFromLeft(44));
            row.removeFromLeft(6);
            rows[static_cast<size_t>(i)].cents.setBounds(row);
        }
        area.removeFromTop(10);
        auto buttons = area.removeFromTop(26);
        closeButton.setBounds(buttons.removeFromRight(70));
        buttons.removeFromRight(8);
        useButton.setBounds(buttons.removeFromRight(70));
        deleteButton.setBounds(buttons.removeFromLeft(70));
        buttons.removeFromLeft(8);
        saveButton.setBounds(buttons.removeFromLeft(70));
    }

private:
    struct Row
    {
        juce::TextButton note;
        MacroSlider cents;
    };

    void showScale()
    {
        if (scale.name.isNotEmpty())
            nameEditor.setText(scale.name, false);
        for (int i = 0; i < 12; ++i)
        {
            auto& row = rows[static_cast<size_t>(i)];
            row.note.setToggleState(scale.used[static_cast<size_t>(i)], juce::dontSendNotification);
            row.cents.setValue(scale.cents[static_cast<size_t>(i)], juce::dontSendNotification);
        }
    }

    void refreshPresets()
    {
        presets.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& saved : CustomScaleLibrary::load())
            presets.addItem(saved.name, id++);
    }

    std::function<void()> onApplied;
    CustomScale scale;
    juce::Label title, hint;
    juce::TextEditor nameEditor;
    juce::ComboBox presets;
    std::array<Row, 12> rows;
    juce::TextButton saveButton, deleteButton, useButton, closeButton;
};

juce::Component::SafePointer<Content> activePopup;
} // namespace

void dismissPopup()
{
    if (auto* content = activePopup.getComponent())
        content->close();
}

void showPopup(juce::Component* parent, juce::Rectangle<int> anchorBounds,
               std::function<void()> onApplied)
{
    if (parent == nullptr)
        return;
    dismissPopup();
    constexpr int width = 460;
    constexpr int height = 340;
    auto* content = new Content(std::move(onApplied));
    const auto bounds = parent->getLocalBounds();
    const int x = juce::jlimit(8, juce::jmax(8, bounds.getWidth() - width - 8),
                               anchorBounds.getCentreX() - width / 2);
    const int y = juce::jlimit(0, juce::jmax(0, bounds.getHeight() - height),
                               anchorBounds.getBottom() + 4);
    parent->addAndMakeVisible(content);
    content->setBounds(x, y, width, height);
    content->toFront(true);
    activePopup = content;
}
} // namespace CustomScaleDialog
