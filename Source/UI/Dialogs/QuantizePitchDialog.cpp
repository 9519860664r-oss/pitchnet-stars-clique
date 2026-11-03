#include "QuantizePitchDialog.h"
#include "../Components/AppFont.h"
#include "../Components/DarkLookAndFeel.h"
#include "../Components/StyledWidgets.h"
#include "../../Utils/Localization.h"
#include "../../Utils/UI/Theme.h"
#include <utility>

namespace QuantizePitchDialog {
namespace {

class Content final : public juce::Component
{
public:
  Content(float initialPitchCenter, float initialPitchDrift, bool initialSnapToScale,
          bool initialIncludeManual,
          std::function<void(float, bool, float, bool, bool)> preview,
          std::function<void(bool)> completed, std::function<void()> refreshPreview)
      : onPreview(std::move(preview)), onComplete(std::move(completed)),
        onRefreshPreview(std::move(refreshPreview))
  {
    pitchCenterLabel.setText(TR("quantize.pitch_center"), juce::dontSendNotification);
    pitchCenterLabel.setFont(AppFont::getFont(15.0f));
    pitchCenterLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);

    pitchCenter.setRange(0.0, 100.0, 1.0);
    pitchCenter.setTextValueSuffix(" %");
    pitchCenter.setValue(initialPitchCenter, juce::dontSendNotification);
    pitchCenter.onDragStart = [this] { sliderDragging = true; };
    pitchCenter.onDragEnd = [this] { sliderDragging = false; refreshCorrection(); };
    pitchCenter.onValueChange = [this] { previewCorrection(); };

    pitchDriftLabel.setText(TR("quantize.pitch_drift"), juce::dontSendNotification);
    pitchDriftLabel.setFont(AppFont::getFont(15.0f));
    pitchDriftLabel.setColour(juce::Label::textColourId, APP_COLOR_TEXT_PRIMARY);
    pitchDrift.setRange(0.0, 100.0, 1.0);
    pitchDrift.setTextValueSuffix(" %");
    pitchDrift.setValue(initialPitchDrift, juce::dontSendNotification);
    pitchDrift.onDragStart = [this] { sliderDragging = true; };
    pitchDrift.onDragEnd = [this] { sliderDragging = false; refreshCorrection(); };
    pitchDrift.onValueChange = [this] { driftEdited = true; previewCorrection(); };

    snapToScale.setToggleState(initialSnapToScale, juce::dontSendNotification);
    snapToScale.onClick = [this] { previewCorrection(); };

    snapToScale.setButtonText(TR("quantize.snap_to_scale"));

    // Notes edited by hand are left alone unless included.
    includeManual.setToggleState(initialIncludeManual, juce::dontSendNotification);
    includeManual.onClick = [this] { previewCorrection(); };
    includeManual.setButtonText("Include hand-edited");
    includeManual.setTooltip("Also correct notes whose pitch you moved by hand");

    setupButton(cancelButton, TR("dialog.cancel"));
    setupButton(okButton, TR("dialog.ok"));
    cancelButton.onClick = [this] { finish(false); };
    okButton.onClick = [this] { finish(true); };

    addAndMakeVisible(pitchCenterLabel);
    addAndMakeVisible(pitchCenter);
    addAndMakeVisible(pitchDriftLabel);
    addAndMakeVisible(pitchDrift);
    addAndMakeVisible(snapToScale);
    addAndMakeVisible(includeManual);
    addAndMakeVisible(cancelButton);
    addAndMakeVisible(okButton);
  }

  ~Content() override
  {
    // Covers Escape/window dismissal as well as the explicit Cancel button.
    if (!finished && onComplete)
      onComplete(false);
  }

  void cancel() { finish(false); }

  void paint(juce::Graphics &g) override
  {
    g.setColour(Skin::colour(0xFF181818u));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
    g.setColour(APP_COLOR_BORDER.withAlpha(0.55f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 10.0f, 1.0f);
  }

  void resized() override
  {
    auto area = getLocalBounds().reduced(16, 10);
    auto row = area.removeFromTop(32);
    pitchCenterLabel.setBounds(row.removeFromLeft(94));
    pitchCenter.setBounds(row);
    area.removeFromTop(6);
    row = area.removeFromTop(32);
    pitchDriftLabel.setBounds(row.removeFromLeft(94));
    pitchDrift.setBounds(row);
    area.removeFromTop(6);
    includeManual.setBounds(area.removeFromTop(24).removeFromLeft(200));
    area.removeFromTop(6);
    auto actionRow = area.removeFromTop(24);
    snapToScale.setBounds(actionRow.removeFromLeft(148));
    auto buttons = actionRow.removeFromRight(160).translated(1, 0);
    cancelButton.setBounds(buttons.removeFromLeft(75));
    buttons.removeFromLeft(10);
    okButton.setBounds(buttons);
  }

private:
  static void setupButton(juce::TextButton &button, const juce::String &text)
  {
    button.setButtonText(text);
    button.setLookAndFeel(&DarkLookAndFeel::getInstance());
    button.setColour(juce::TextButton::buttonColourId, Skin::colour(0xFF5B5B5Bu));
    button.setColour(juce::TextButton::buttonOnColourId, Skin::colour(0xFF3E3E3Eu));
    button.setColour(juce::TextButton::textColourOffId, Skin::colour(0xFFEFEFEFu));
    button.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  }

public:
  void setAndAccept(float value, bool snap)
  {
    snapToScale.setToggleState(snap, juce::dontSendNotification);
    pitchCenter.setValue(value, juce::dontSendNotification);
    previewCorrection();
    finish(true);
  }

private:
  void finish(bool accepted)
  {
    if (finished)
      return;
    if (accepted) refreshCorrection();
    finished = true;
    if (onComplete)
      onComplete(accepted);
    auto safeThis = juce::Component::SafePointer<Content>(this);
    juce::MessageManager::callAsync([safeThis]
    {
      if (auto *content = safeThis.getComponent())
      {
        if (auto *parent = content->getParentComponent())
          parent->removeChildComponent(content);
        delete content;
      }
    });
  }

  void previewCorrection()
  {
    if (finished)
      return;
    if (onPreview)
      onPreview(static_cast<float>(pitchCenter.getValue()), snapToScale.getToggleState(),
                static_cast<float>(pitchDrift.getValue()), driftEdited,
                includeManual.getToggleState());
    previewPending = true;
    if (!sliderDragging)
      refreshCorrection();
  }

  void refreshCorrection()
  {
    if (!previewPending || finished)
      return;
    previewPending = false;
    if (onRefreshPreview)
      onRefreshPreview();
  }

  std::function<void(float, bool, float, bool, bool)> onPreview;
  std::function<void(bool)> onComplete;
  std::function<void()> onRefreshPreview;
  bool sliderDragging = false;
  bool previewPending = false;
  juce::Label pitchCenterLabel;
  MacroSlider pitchCenter;
  juce::Label pitchDriftLabel;
  MacroSlider pitchDrift;
  bool driftEdited = false;
  StyledToggleButton snapToScale;
  StyledToggleButton includeManual;
  juce::TextButton cancelButton, okButton;
  bool finished = false;
};

juce::Component::SafePointer<Content> activePopup;
} // namespace

void setAndAccept(float pitchCenter, bool snapToScale)
{
  if (auto *content = activePopup.getComponent())
    content->setAndAccept(pitchCenter, snapToScale);
}

void dismissPopup()
{
  if (auto *content = activePopup.getComponent())
    content->cancel();
}

void showPopup(juce::Component *parent, juce::Rectangle<int> anchorBounds,
               float initialPitchCenter, float initialPitchDrift, bool initialSnapToScale,
               bool initialIncludeManual,
               std::function<void(float, bool, float, bool, bool)> onPreview,
               std::function<void(bool)> onComplete,
               std::function<void()> onRefreshPreview)
{
  if (parent == nullptr)
    return;

  dismissPopup();

  constexpr int width = 340;
  constexpr int height = 154;
  auto *content = new Content(initialPitchCenter, initialPitchDrift, initialSnapToScale,
                              initialIncludeManual,
                              std::move(onPreview), std::move(onComplete),
                              std::move(onRefreshPreview));
  content->setSize(width, height);
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

} // namespace QuantizePitchDialog
