#include "CreditsDialog.h"

#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../../Utils/UI/Theme.h"

#if defined(JUCE_APPLICATION_VERSION_STRING)
#define PITCHNET_CREDITS_VERSION JUCE_APPLICATION_VERSION_STRING
#elif defined(JucePlugin_VersionString)
#define PITCHNET_CREDITS_VERSION JucePlugin_VersionString
#else
#define PITCHNET_CREDITS_VERSION ""
#endif

namespace CreditsDialog
{
namespace
{
const char* const releasesUrl = "https://github.com/9519860664r-oss/pitchnet-stars-clique/releases";

// From the repository: its authors (git history), README and
// THIRD_PARTY_NOTICES.txt.
const char* creditsText()
{
    return "PitchNet (Stars Clique ver) " PITCHNET_CREDITS_VERSION "\n"
           "\n"
           "Original authors\n"
           "  PitchNet was created by Session Loops and contributors:\n"
           "  Jie Chen, \xe4\xb8\x80\xe6\x94\xaf\xe7\xb2\x89\xe7\xac\x94, KRKT, hrukalive, Steven Cooper,\n"
           "  hyperrealist, \xe6\x9a\x97\xe9\x9b\xa8\xe5\x86\xa5\n"
           "  github.com/SessionLoops/PitchNet\n"
           "  PitchNet is based on HachiTune by the HachiTune contributors\n"
           "  github.com/KCKT0112/HachiTune\n"
           "\n"
           "Stars Clique edition\n"
           "  Modified and extended by Goth Angel (Mikhail Mikhalchenko), Stars Clique:\n"
           "  multitrack editing, recording, tuning tools and the new skin.\n"
           "  Releases and source code:\n"
           "  github.com/9519860664r-oss/pitchnet-stars-clique\n"
           "\n"
           "Models\n"
           "  PC-NSF-HiFiGAN vocoder and GAME note segmentation - Team OpenVPI\n"
           "  FCPE pitch detection - CN_ChiTu\n"
           "  RMVPE pitch detection - yxlllc\n"
           "\n"
           "Libraries\n"
           "  JUCE - Raw Material Software\n"
           "  ONNX Runtime - Microsoft\n"
           "  r8brain-free-src - Aleksey Vaneev\n"
           "  ARA SDK - Celemony Software GmbH and contributors\n"
           "  AAX SDK - Avid Technology\n"
           "\n"
           "License: GNU Affero General Public License v3 (AGPLv3).\n"
           "Full notices: THIRD_PARTY_NOTICES.txt.";
}

class Content final : public juce::Component
{
public:
    Content()
    {
        title.setText("Credits", juce::dontSendNotification);
        title.setFont(AppFont::getFont(17.0f));
        title.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
        addAndMakeVisible(title);

        text.setMultiLine(true);
        text.setReadOnly(true);
        text.setCaretVisible(false);
        text.setScrollbarsShown(true);
        text.setFont(AppFont::getFont(13.0f));
        text.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        text.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        text.setColour(juce::TextEditor::textColourId, APP_COLOR_TEXT_PRIMARY);
        text.setText(juce::String::fromUTF8(creditsText()), false);
        addAndMakeVisible(text);

        releases.setButtonText("Download / releases on GitHub");
        releases.setURL(juce::URL(releasesUrl));
        releases.setFont(AppFont::getFont(13.0f), false, juce::Justification::centredLeft);
        releases.setColour(juce::HyperlinkButton::textColourId, juce::Colour(0xFFC084FCu));
        addAndMakeVisible(releases);

        close.setButtonText("Close");
        close.setLookAndFeel(&DarkLookAndFeel::getInstance());
        close.onClick = [this] { dismiss(); };
        addAndMakeVisible(close);
    }

    ~Content() override { close.setLookAndFeel(nullptr); }

    void dismiss()
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
        g.setColour(APP_COLOR_SURFACE_RAISED);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
        g.setColour(APP_COLOR_BORDER_HIGHLIGHT.withAlpha(0.7f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(18, 14);
        title.setBounds(area.removeFromTop(26));
        auto bottom = area.removeFromBottom(28);
        close.setBounds(bottom.removeFromRight(80));
        releases.setBounds(bottom.withTrimmedRight(8));
        area.removeFromBottom(8);
        text.setBounds(area);
    }

private:
    juce::Label title;
    juce::TextEditor text;
    juce::HyperlinkButton releases;
    juce::TextButton close;
};

juce::Component::SafePointer<Content> active;
} // namespace

void dismissPopup()
{
    if (auto* content = active.getComponent())
        content->dismiss();
}

void showPopup(juce::Component* parent)
{
    if (parent == nullptr)
        return;
    dismissPopup();
    auto* content = new Content();
    const int width = juce::jmin(520, parent->getWidth() - 24);
    const int height = juce::jmin(540, parent->getHeight() - 24);
    parent->addAndMakeVisible(content);
    content->setBounds((parent->getWidth() - width) / 2, (parent->getHeight() - height) / 2,
                       width, height);
    content->toFront(true);
    active = content;
}
} // namespace CreditsDialog
