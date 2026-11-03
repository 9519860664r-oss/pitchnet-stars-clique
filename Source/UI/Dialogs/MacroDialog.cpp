#include "MacroDialog.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Utils/Localization.h"
#include "../../Utils/UI/Theme.h"
#include <memory>
#include <utility>

namespace MacroDialog {
namespace {

constexpr int rowHeight = 30;
constexpr int rowGap = 6;
constexpr int labelWidth = 110;

class Content final : public juce::Component
{
public:
    Content(const juce::String& titleText, std::vector<Field> fieldList,
            std::function<void(const Values&, bool)> preview,
            std::function<void(bool, const Values&)> completed)
        : fields(std::move(fieldList)), onPreview(std::move(preview)),
          onComplete(std::move(completed))
    {
        title.setText(titleText, juce::dontSendNotification);
        title.setFont(AppFont::getFont(15.0f));
        title.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        addAndMakeVisible(title);

        for (auto& field : fields)
        {
            auto row = std::make_unique<Row>();
            row->label.setText(field.label, juce::dontSendNotification);
            row->label.setFont(AppFont::getFont(14.0f));
            row->label.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
            addAndMakeVisible(row->label);
            const auto id = field.id;
            switch (field.kind)
            {
                case Field::Kind::Slider:
                    row->slider = std::make_unique<MacroSlider>();
                    row->slider->setRange(field.min, field.max, field.step);
                    row->slider->setTextValueSuffix(field.suffix);
                    row->slider->setValue(field.value, juce::dontSendNotification);
                    row->slider->onDragStart = [this] { dragging = true; };
                    row->slider->onDragEnd = [this] { dragging = false; changed(); };
                    row->slider->onValueChange = [this] { changed(); };
                    addAndMakeVisible(*row->slider);
                    break;
                case Field::Kind::Choice:
                    row->choice = std::make_unique<juce::ComboBox>();
                    row->choice->setLookAndFeel(&DarkLookAndFeel::getInstance());
                    for (int i = 0; i < field.options.size(); ++i)
                        row->choice->addItem(field.options[i], i + 1);
                    row->choice->setSelectedItemIndex(static_cast<int>(field.value),
                                                      juce::dontSendNotification);
                    row->choice->onChange = [this] { changed(); };
                    addAndMakeVisible(*row->choice);
                    break;
                case Field::Kind::Toggle:
                    row->toggle = std::make_unique<StyledToggleButton>();
                    row->toggle->setToggleState(field.value > 0.5, juce::dontSendNotification);
                    row->toggle->onClick = [this] { changed(); };
                    addAndMakeVisible(*row->toggle);
                    break;
            }
            rows.push_back(std::move(row));
        }

        setupButton(cancelButton, TR("dialog.cancel"));
        setupButton(okButton, TR("dialog.ok"));
        cancelButton.onClick = [this] { finish(false); };
        okButton.onClick = [this] { finish(true); };
        addAndMakeVisible(cancelButton);
        addAndMakeVisible(okButton);
    }

    ~Content() override
    {
        for (auto& row : rows)
            if (row->choice)
                row->choice->setLookAndFeel(nullptr);
        if (!finished && onComplete)
            onComplete(false, values());
    }

    int idealHeight() const
    {
        return 10 + 26 + static_cast<int>(rows.size()) * (rowHeight + rowGap) + 8 + 26 + 12;
    }

    void cancel() { finish(false); }
    void accept() { finish(true); }
    void set(const juce::String& id, double v)
    {
        for (size_t i = 0; i < fields.size(); ++i)
            if (fields[i].id == id)
            {
                auto& row = *rows[i];
                if (row.slider) row.slider->setValue(v, juce::sendNotificationSync);
                if (row.choice) row.choice->setSelectedItemIndex(static_cast<int>(v), juce::sendNotificationSync);
                if (row.toggle) row.toggle->setToggleState(v > 0.5, juce::sendNotificationSync);
            }
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
        auto area = getLocalBounds().reduced(16, 10);
        title.setBounds(area.removeFromTop(26));
        for (size_t i = 0; i < rows.size(); ++i)
        {
            auto row = area.removeFromTop(rowHeight);
            area.removeFromTop(rowGap);
            rows[i]->label.setBounds(row.removeFromLeft(labelWidth));
            if (rows[i]->slider) rows[i]->slider->setBounds(row);
            if (rows[i]->choice) rows[i]->choice->setBounds(row.reduced(0, 2));
            if (rows[i]->toggle) rows[i]->toggle->setBounds(row.removeFromLeft(50));
        }
        area.removeFromTop(8);
        auto buttons = area.removeFromTop(26).removeFromRight(160);
        cancelButton.setBounds(buttons.removeFromLeft(75));
        buttons.removeFromLeft(10);
        okButton.setBounds(buttons);
    }

private:
    struct Row
    {
        juce::Label label;
        std::unique_ptr<MacroSlider> slider;
        std::unique_ptr<juce::ComboBox> choice;
        std::unique_ptr<StyledToggleButton> toggle;
    };

    static void setupButton(juce::TextButton& button, const juce::String& text)
    {
        button.setButtonText(text);
        button.setLookAndFeel(&DarkLookAndFeel::getInstance());
        button.setColour(juce::TextButton::buttonColourId, Skin::colour(0xFF5B5B5Bu));
        button.setColour(juce::TextButton::buttonOnColourId, Skin::colour(0xFF3E3E3Eu));
        button.setColour(juce::TextButton::textColourOffId, Skin::colour(0xFFEFEFEFu));
        button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    Values values() const
    {
        Values result;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            const auto& row = *rows[i];
            double v = fields[i].value;
            if (row.slider) v = row.slider->getValue();
            if (row.choice) v = row.choice->getSelectedItemIndex();
            if (row.toggle) v = row.toggle->getToggleState() ? 1.0 : 0.0;
            result[fields[i].id] = v;
        }
        return result;
    }

    void changed()
    {
        if (!finished && onPreview)
            onPreview(values(), dragging);
    }

    void finish(bool accepted)
    {
        if (finished)
            return;
        finished = true;
        if (onComplete)
            onComplete(accepted, values());
        auto safeThis = juce::Component::SafePointer<Content>(this);
        juce::MessageManager::callAsync([safeThis] {
            if (auto* content = safeThis.getComponent())
            {
                if (auto* parent = content->getParentComponent())
                    parent->removeChildComponent(content);
                delete content;
            }
        });
    }

    juce::Label title;
    std::vector<Field> fields;
    std::vector<std::unique_ptr<Row>> rows;
    std::function<void(const Values&, bool)> onPreview;
    std::function<void(bool, const Values&)> onComplete;
    juce::TextButton cancelButton, okButton;
    bool dragging = false;
    bool finished = false;
};

juce::Component::SafePointer<Content> activePopup;
} // namespace

void dismissPopup()
{
    if (auto* content = activePopup.getComponent())
        content->cancel();
}

void acceptPopup()
{
    if (auto* content = activePopup.getComponent())
        content->accept();
}

void setValue(const juce::String& id, double value)
{
    if (auto* content = activePopup.getComponent())
        content->set(id, value);
}

void showPopup(juce::Component* parent, juce::Rectangle<int> anchorBounds,
               const juce::String& title, std::vector<Field> fields,
               std::function<void(const Values&, bool)> onPreview,
               std::function<void(bool, const Values&)> onComplete)
{
    if (parent == nullptr)
        return;
    dismissPopup();

    auto* content = new Content(title, std::move(fields), std::move(onPreview),
                                std::move(onComplete));
    constexpr int width = 360;
    const int height = content->idealHeight();
    const auto bounds = parent->getLocalBounds();
    const int x = juce::jlimit(8, juce::jmax(8, bounds.getWidth() - width - 8),
                               anchorBounds.getRight() - width);
    const int y = juce::jlimit(0, juce::jmax(0, bounds.getHeight() - height),
                               anchorBounds.getY());
    parent->addAndMakeVisible(content);
    content->setBounds(x, y, width, height);
    content->toFront(false);
    activePopup = content;
}

} // namespace MacroDialog
