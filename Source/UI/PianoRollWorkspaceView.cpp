#include "PianoRollWorkspaceView.h"
#include "../Utils/NoteTimeMap.h"
#include "../Undo/NoteActions.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "BinaryData.h"
#include "Dialogs/QuantizePitchDialog.h"
#include "Dialogs/MacroDialog.h"
#include "../Utils/NoteMacros.h"
#include "../Utils/NoteAmplitude.h"
#include "../Undo/TimingAction.h"
#include "../Undo/AmplitudeAction.h"
#include <map>
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/PitchMacroScope.h"
#include "../Undo/UndoActions.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

void PianoRollWorkspaceView::FloatingZoomSliderLookAndFeel::drawLinearSlider(
    juce::Graphics &g,
    int x,
    int y,
    int width,
    int height,
    float sliderPos,
    float minSliderPos,
    float maxSliderPos,
    const juce::Slider::SliderStyle style,
    juce::Slider &slider)
{
  juce::ignoreUnused(minSliderPos, maxSliderPos, slider);

  const auto bounds = juce::Rectangle<float>(static_cast<float>(x),
                                             static_cast<float>(y),
                                             static_cast<float>(width),
                                             static_cast<float>(height));
  const auto thumbRadius = 4.5f;
  const auto trackHeight = 2.0f;

  if (style == juce::Slider::LinearVertical)
  {
    const auto track = juce::Rectangle<float>(
        bounds.getCentreX() - trackHeight * 0.5f,
        bounds.getY(),
        trackHeight,
        juce::jmax(0.0f, bounds.getHeight()));

    g.setColour(Skin::colour(0xFF494949u));
    g.fillRoundedRectangle(track, trackHeight * 0.5f);

    const auto clampedSliderPos = juce::jlimit(track.getY() + thumbRadius,
                                               track.getBottom() - thumbRadius,
                                               sliderPos);
    g.setColour(Skin::colour(0xFF9B9B9Bu));
    g.fillEllipse(bounds.getCentreX() - thumbRadius,
                  clampedSliderPos - thumbRadius,
                  thumbRadius * 2.0f,
                  thumbRadius * 2.0f);
    return;
  }

  const auto track = juce::Rectangle<float>(bounds.getX(),
                                           bounds.getCentreY() - trackHeight * 0.5f,
                                           juce::jmax(0.0f, bounds.getWidth()),
                                           trackHeight);

  g.setColour(Skin::colour(0xFF494949u));
  g.fillRoundedRectangle(track, trackHeight * 0.5f);

  const auto clampedSliderPos = juce::jlimit(track.getX() + thumbRadius,
                                             track.getRight() - thumbRadius,
                                             sliderPos);
  g.setColour(Skin::colour(0xFF9B9B9Bu));
  g.fillEllipse(clampedSliderPos - thumbRadius,
                bounds.getCentreY() - thumbRadius,
                thumbRadius * 2.0f,
                thumbRadius * 2.0f);
}

void PianoRollWorkspaceView::FloatingControlBackground::paint(juce::Graphics &g)
{
  g.setColour(Skin::colour(0xFF0D0B0Bu));
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 5.0f);
}

PianoRollWorkspaceView::PianoRollWorkspaceView(PianoRollComponent &piano)
    : pianoRoll(piano)
{
  pianoRoll.onMacroRequested = [this](int which) { showMacro(which); };
  addAndMakeVisible(noteInspector);
  pianoCard.setPadding(0);
  pianoCard.setCornerRadius(0.0f);
  pianoCard.setBorderColour(juce::Colours::transparentBlack);
  pianoCard.setContentComponent(&pianoRoll);

  overviewCard.setPadding(0);
  overviewCard.setCornerRadius(0.0f);
  overviewCard.setBackgroundColour(Skin::colour(0xFF191717u));
  overviewCard.setBorderColour(Skin::colour(0xFF0D0B0Bu));
  overviewCard.setContentComponent(&overviewPanel);
  overviewPanel.setDrawBackground(false);

  // The thumbnail works in time relative to the canvas's timeline start
  // (0 in Clip mode, the song start in Track mode).
  overviewPanel.getViewState = [this]()
  {
    OverviewPanel::ViewState state;
    const double viewStart = pianoRoll.getTimelineStart();
    const float pps = pianoRoll.getPixelsPerSecond();
    state.totalTime = pianoRoll.getTimelineLength();
    state.cursorTime = pianoRoll.getCursorTime() - viewStart;
    state.scrollX = pianoRoll.getScrollX() - viewStart * pps;
    state.pixelsPerSecond = pps;
    state.visibleWidth = pianoRoll.getVisibleContentWidth();
    state.trackMode = pianoRoll.isTrackViewMode();
    state.viewStartSeconds = viewStart;
    state.displayOffset = pianoRoll.projectToTimeline(0.0);
    if (auto *project = pianoRoll.getProject())
    {
      const auto &ranges = project->getAudioData().playbackRegionRanges;
      if (!ranges.empty() && ranges.front().second > ranges.front().first)
      {
        state.hasActiveSpan = true;
        state.activeSpanStart = ranges.front().first;
        state.activeSpanEnd = ranges.front().second;
      }
    }
    return state;
  };
  overviewPanel.onScrollXChanged = [this](double x)
  {
    pianoRoll.setScrollX(x + pianoRoll.getTimelineStart() *
                                 pianoRoll.getPixelsPerSecond());
    if (pianoRoll.onScrollChanged)
      pianoRoll.onScrollChanged(pianoRoll.getScrollX());
  };
  overviewPanel.onZoomChanged = [this](float pps)
  {
    pianoRoll.setPixelsPerSecond(pps, false);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };
  zoomXSlider.setSliderStyle(juce::Slider::LinearHorizontal);
  zoomXSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomXSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomXSlider.setRange(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, 0.1);
  // Even steps across a wide range: the middle of the slider is the default.
  zoomXSlider.setSkewFactorFromMidPoint(DEFAULT_PIXELS_PER_SECOND);
  zoomXSlider.setValue(pianoRoll.getPixelsPerSecond(),
                       juce::dontSendNotification);
  zoomXSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSecond(static_cast<float>(zoomXSlider.getValue()),
                                 true);
    if (pianoRoll.onZoomChanged)
      pianoRoll.onZoomChanged(pianoRoll.getPixelsPerSecond());
  };

  zoomYSlider.setSliderStyle(juce::Slider::LinearVertical);
  zoomYSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
  zoomYSlider.setLookAndFeel(&floatingZoomSliderLookAndFeel);
  zoomYSlider.setRange(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, 0.1);
  zoomYSlider.setSkewFactorFromMidPoint(DEFAULT_PIXELS_PER_SEMITONE);
  zoomYSlider.setValue(pianoRoll.getPixelsPerSemitone(),
                       juce::dontSendNotification);
  zoomYSlider.onValueChange = [this]()
  {
    pianoRoll.setPixelsPerSemitone(static_cast<float>(zoomYSlider.getValue()),
                                   static_cast<float>(pianoRoll.getVisibleContentHeight()) *
                                       0.5f);
  };

  autoZoomButton.setImage(juce::ImageFileFormat::loadFrom(
      BinaryData::autozoom_png, static_cast<size_t>(BinaryData::autozoom_pngSize)));
  autoZoomButton.setTooltip(TR("tooltip.vertical_auto_zoom"));
  autoZoomButton.onClick = [this]()
  {
    if (onAutoZoomRequested)
      onAutoZoomRequested();
  };

  overviewToggleButton.setImage(juce::ImageFileFormat::loadFrom(
      BinaryData::thumbnail_png, static_cast<size_t>(BinaryData::thumbnail_pngSize)));
  overviewToggleButton.setClickingTogglesState(true);
  overviewToggleButton.setToggleState(overviewVisible,
                                      juce::dontSendNotification);
  overviewToggleButton.onClick = [this]()
  {
    overviewVisible = overviewToggleButton.getToggleState();
    updateOverviewVisibility();
    if (onOverviewVisibilityChanged)
      onOverviewVisibilityChanged(overviewVisible);
  };

  addAndMakeVisible(pianoCard);
  addAndMakeVisible(overviewCard);
  addAndMakeVisible(zoomXBackground);
  addAndMakeVisible(zoomYBackground);
  addAndMakeVisible(autoZoomButton);
  addAndMakeVisible(overviewToggleButton);
  addAndMakeVisible(zoomXSlider);
  addAndMakeVisible(zoomYSlider);

  overviewCard.setVisible(false);
  overviewPanel.setVisible(false);
  pianoRoll.setHorizontalScrollBarVisible(true);
  startTimerHz(30);
}

PianoRollWorkspaceView::~PianoRollWorkspaceView()
{
  zoomXSlider.setLookAndFeel(nullptr);
  zoomYSlider.setLookAndFeel(nullptr);
}

void PianoRollWorkspaceView::paint(juce::Graphics &g)
{
  juce::ignoreUnused(g);
}

void PianoRollWorkspaceView::resized()
{
  auto bounds = getLocalBounds();

  const float progress = juce::jlimit(0.0f, 1.0f, overviewAnimationProgress);
  if (progress > 0.001f)
  {
    const int animatedOverviewHeight =
        static_cast<int>(std::round(static_cast<float>(overviewHeight) * progress));
    const int animatedGap =
        static_cast<int>(std::round(static_cast<float>(cardGap) * progress));
    const auto fullBounds = bounds;

    bounds.removeFromBottom(animatedOverviewHeight + animatedGap);

    auto overviewBounds = juce::Rectangle<int>(
        fullBounds.getX(),
        fullBounds.getBottom() - animatedOverviewHeight,
        fullBounds.getWidth(), overviewHeight);
    overviewBounds.removeFromLeft(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromRight(thumbnailOuterHorizontalPadding);
    overviewBounds.removeFromBottom(thumbnailOuterBottomPadding);
    overviewCard.setBounds(overviewBounds);
  }
  else
  {
    overviewCard.setBounds({});
  }

  // Stars Clique: the inspector and the editor float with room around them.
  if (Skin::starsClique())
    bounds = bounds.withTrimmedLeft(6).withTrimmedRight(8).withTrimmedBottom(8).withTrimmedTop(4);
  // Note Inspector, above the editor.
  {
    auto strip = bounds.removeFromTop(NoteInspector::preferredHeightForSkin());
    strip.removeFromLeft(thumbnailOuterHorizontalPadding);
    strip.removeFromRight(thumbnailOuterHorizontalPadding);
    noteInspector.setBounds(strip);
    bounds.removeFromTop(Skin::starsClique() ? 12 : 2);
  }
  pianoCard.setBounds(bounds);

  auto overlay = pianoCard.getBounds();
  const int sliderBottom = overlay.getBottom() - floatingControlInset - 3;
  const int sliderRight = overlay.getRight() - floatingControlInset;
  const int zoomXHeight = floatingSliderThickness;
  const int zoomXTop = sliderBottom - zoomXHeight;
  const int zoomYLeft = sliderRight - zoomSliderWidth - 5;
  const int controlColumnCentreX = zoomYLeft + zoomSliderWidth / 2;
  const int thumbnailButtonX = controlColumnCentreX - thumbnailButtonSize / 2;

  auto zoomXRect = juce::Rectangle<int>(
      thumbnailButtonX - overlayControlGap - zoomSliderLength, zoomXTop,
      zoomSliderLength, zoomXHeight);

  overviewToggleButton.setBounds(
      thumbnailButtonX,
      zoomXRect.getY() + (zoomXHeight - thumbnailButtonSize) / 2,
      thumbnailButtonSize, thumbnailButtonSize);

  autoZoomButton.setBounds(
      controlColumnCentreX - autoZoomButtonSize / 2,
      overviewToggleButton.getY() - autoZoomButtonSize - overlayVerticalGap,
      autoZoomButtonSize, autoZoomButtonSize);

  auto zoomYRect = juce::Rectangle<int>(
      zoomYLeft,
      autoZoomButton.getY() - overlayVerticalGap - zoomSliderHeight,
      zoomSliderWidth, zoomSliderHeight);

  zoomXSlider.setBounds(zoomXRect);
  zoomYSlider.setBounds(zoomYRect);

  zoomXBg = zoomXRect.toFloat();
  zoomYBg = zoomYRect.toFloat();
  toggleBg = overviewToggleButton.getBounds().toFloat();

  zoomXBackground.setBounds(zoomXBg.toNearestInt());
  zoomYBackground.setBounds(zoomYBg.toNearestInt());
}

void PianoRollWorkspaceView::setTrackViewModeAvailable(bool available)
{
  trackViewModeAvailable = available;
  if (!available && pianoRoll.isTrackViewMode())
    setTrackViewMode(false);
}

void PianoRollWorkspaceView::setTrackViewMode(bool track)
{
  track = track && trackViewModeAvailable;
  if (pianoRoll.isTrackViewMode() == track)
    return;
  pianoRoll.setTrackViewMode(track);
  overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::setProject(Project *project)
{
  dismissPitchCenterPopup();
  overviewPanel.setProject(project);
  if (project != nullptr)
    setOverviewVisible(project->thumbnailVisible);
}

void PianoRollWorkspaceView::dismissPitchCenterPopup()
{
  QuantizePitchDialog::dismissPopup();
}

void PianoRollWorkspaceView::showPitchCenterPopup()
{
  dismissPitchCenterPopup();
  auto* activeProject = pianoRoll.getProject();
  if (!activeProject) return;
  const bool track = pianoRoll.multiTrackEditing() && bool(getTrackPitchMacroTargets);
  auto targets = track ? getTrackPitchMacroTargets()
                       : std::vector<TrackPitchMacroTarget>{{activeProject, {}, {}, {}}};
  if (targets.empty()) return;

  struct NoteCenter {
    Note* note;
    float midiNote, sourceMidiNote, pitchDrift;
    // Correct Pitch value the note remembered when the popup opened (-1:
    // never corrected), and the one it keeps when the popup is accepted.
    float macro = -1.0f;
    float finalMidi = 0.0f, finalDrift = 1.0f, finalMacro = -1.0f;
    float renderedMidi = midiNote, renderedDrift = pitchDrift;
  };
  struct State {
    TrackPitchMacroTarget target;
    float originalPitchCenter;
    std::vector<NoteCenter> notes;
  };
  auto states = std::make_shared<std::vector<State>>();
  bool anySelection = false;
  for (auto& target : targets)
    anySelection |= pitchMacroScopeHasSelection(*target.project, track, target.frames);
  for (auto& target : targets) {
    State state{target, target.project->getPitchCenter(), {}};
    for (auto* note : collectPitchMacroEditNotes(*target.project, track, target.frames,
                                                 anySelection))
      if (!note->hasUnisonSource()) // doubles in unison follow their lead
      state.notes.push_back({note, note->getMidiNote(), note->getLastNonMacroMidiNote(),
                             note->getPitchDrift(), note->getPitchCenterMacro()});
    states->push_back(std::move(state));
  }
  // The popup opens with the value the notes keep from the last applied
  // correction when they all share one; otherwise (never corrected, or
  // different values) with 0.
  std::vector<float> remembered;
  for (const auto& state : *states)
    for (const auto& center : state.notes) remembered.push_back(center.macro);
  auto amount = std::make_shared<float>(NoteMacros::rememberedMacroValue(remembered));
  auto previewEdited = std::make_shared<bool>(false);
  auto safeThis = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  const auto refreshCurves = [safeThis, states] {
    if (!safeThis) return;
    safeThis->pianoRoll.endMacroCurvePreview();
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      PitchCurveProcessor::rebuildBaseFromNotes(*state.target.project);
      if (state.target.onChanged) state.target.onChanged(false);
    }
    safeThis->pianoRoll.invalidateBasePitchCache();
    safeThis->pianoRoll.repaint();
    safeThis->refreshOverview();
  };
  // Track the last requested sound, rather than comparing with popup-open
  // values: a later drag can return to the original and still need a render.
  const auto renderPreview = [safeThis, states] {
    if (!safeThis) return;
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (auto& center : state.notes) {
        const float midi = center.note->getMidiNote(), drift = center.note->getPitchDrift();
        if (std::abs(midi - center.renderedMidi) <= 0.0001f &&
            std::abs(drift - center.renderedDrift) <= 0.0001f) continue;
        center.renderedMidi = midi;
        center.renderedDrift = drift;
        center.note->markDirty();
        center.note->markSynthDirty();
        first = std::min(first, center.note->getStartFrame());
        last = std::max(last, center.note->getEndFrame());
      }
      if (last < first) continue;
      state.target.project->markNoteEditDirtyRange(first, last);
      if (state.target.project == safeThis->pianoRoll.getProject()) {
        if (safeThis->pianoRoll.onPitchEdited) safeThis->pianoRoll.onPitchEdited();
        if (safeThis->pianoRoll.onPitchPreviewRenderRequested)
          safeThis->pianoRoll.onPitchPreviewRenderRequested();
      } else if (state.target.onChanged) state.target.onChanged(true);
    }
  };
  const auto apply = [safeThis, states, amount](bool redo) {
    if (!safeThis) return;
    safeThis->pianoRoll.endMacroCurvePreview();
    bool activeChanged = false;
    for (auto& state : *states) {
      if (state.target.isValid && !state.target.isValid()) continue;
      auto* project = state.target.project;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (const auto& center : state.notes) {
        // The value the note remembers follows undo and redo too.
        center.note->setPitchCenterMacro(redo ? center.finalMacro : center.macro);
        if (std::abs(center.finalMidi - center.midiNote) <= 0.0001f &&
            std::abs(center.finalDrift - center.pitchDrift) <= 0.0001f) continue;
        center.note->setMidiNoteFromPitchCorrection(redo ? center.finalMidi : center.midiNote);
        center.note->setPitchDrift(redo ? center.finalDrift : center.pitchDrift);
        center.note->markDirty();
        center.note->markSynthDirty();
        first = std::min(first, center.note->getStartFrame());
        last = std::max(last, center.note->getEndFrame());
      }
      project->setPitchCenter(redo ? *amount : state.originalPitchCenter);
      project->setModified(true);
      if (last >= first) {
        PitchCurveProcessor::rebuildBaseFromNotes(*project);
        project->markNoteEditDirtyRange(first, last);
      }
      if (project == safeThis->pianoRoll.getProject()) activeChanged = true;
      else if (state.target.onChanged) state.target.onChanged(true);
    }
    safeThis->pianoRoll.invalidateBasePitchCache();
    safeThis->pianoRoll.repaint();
    safeThis->refreshOverview();
    if (activeChanged && safeThis->pianoRoll.onPitchEdited)
      safeThis->pianoRoll.onPitchEdited();
    if (activeChanged && safeThis->pianoRoll.onPitchEditFinished)
      safeThis->pianoRoll.onPitchEditFinished();
  };
  class MacroAction final : public UndoableAction {
  public:
    MacroAction(std::function<void(bool)> callback, bool trackWide)
        : apply(std::move(callback)), trackWide(trackWide) {}
    void undo() override { apply(false); }
    void redo() override { apply(true); }
    juce::String getName() const override { return "Pitch Macro"; }
    bool requiresAudioResynthesis() const override { return false; }
    bool ownsProjectContext() const override { return trackWide; }
  private:
    std::function<void(bool)> apply;
    bool trackWide;
  };
  const auto activeState = std::find_if(states->begin(), states->end(),
      [activeProject](const State& state) { return state.target.project == activeProject; });
  const float initialDrift = activeState == states->end() || activeState->notes.empty() ? 0.0f
      : juce::jlimit(0.0f, 100.0f, (1.0f - activeState->notes.front().pitchDrift) * 100.0f);
  const auto anchor = juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0);
  QuantizePitchDialog::showPopup(this, anchor, *amount, initialDrift, pitchCenterSnapToScale,
      pitchCenterIncludeManual,
      [this, states, targets, amount, previewEdited](float value, bool snapToScale, float drift,
                                                     bool driftEdited, bool includeManual) {
        *previewEdited = true;
        *amount = value;
        pitchCenterSnapToScale = snapToScale;
        pitchCenterIncludeManual = includeManual;
        std::vector<Note*> previewNotes;
        for (const auto& state : *states)
          if (state.target.project == pianoRoll.getProject())
            for (const auto& center : state.notes) previewNotes.push_back(center.note);
        pianoRoll.beginMacroCurvePreview(previewNotes);
        pianoRoll.beginInactiveMacroPreview(targets);
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          const float amount = value;
          const auto selectedScaleMode = state.target.project->getScaleMode();
          const auto correctionScaleMode =
              (selectedScaleMode == ScaleMode::None ||
               selectedScaleMode == ScaleMode::Chromatic)
                  ? state.target.project->getPreferredScaleMode()
                  : selectedScaleMode;
          // Pitch Center is a correction strength: 0% allows the maximum
          // possible deviation (0.5 semitones), while 100% allows none. Notes
          // already inside the remaining allowance are untouched; larger ones
          // are brought down to it while retaining their direction.
          const float maximumDeviation = (100.0f - amount) * 0.005f;
          for (const auto &center : state.notes)
          {
            if (center.note == nullptr) continue;
            if (!includeManual && center.note->hasManualPitchEdit())
            {
              // Left as it is, unless "include" is on.
              center.note->setMidiNoteFromPitchCorrection(center.midiNote);
              center.note->setPitchDrift(center.pitchDrift);
              continue;
            }
            center.note->setPitchCenterMacro(amount);
            if (driftEdited)
              center.note->setPitchDrift(1.0f - drift / 100.0f);
            // Calculate from the latest regular edit, never from an earlier
            // macro result. This lets a lower amount restore that edit without
            // compounding correction or discarding edits made between passes.
            const float sourceMidi = center.sourceMidiNote;
            // The key in force where the note is (key changes), and the
            // custom scale's tuning when that is the key.
            // Key markers are on the song's timeline: convert the note's time.
            auto key = ScaleUtils::keyAt(
                *state.target.project,
                pianoRoll.projectToTimeline(framesToSeconds(center.note->getStartFrame())));
            if (!ScaleUtils::hasKey(key))
              key.mode = correctionScaleMode;
            const int referenceHz = state.target.project->getPitchReferenceHz();
            // "Tune to key": nearest note of the key; otherwise the nearest
            // note, both at the A4 the user set.
            const float corrected =
                ScaleUtils::pitchCorrectionTarget(sourceMidi, key, snapToScale, referenceHz);
            const float signedDeviation = sourceMidi - corrected;
            if (snapToScale)
            {
              // Scale mode interpolates every note toward its closest scale tone:
              // 100% reaches it, while lower values retain that fraction of the
              // note's original scale-distance.
              center.note->setMidiNoteFromPitchCorrection(
                  corrected + signedDeviation *
                  ((100.0f - amount) / 100.0f));
            }
            else if (std::abs(signedDeviation) > maximumDeviation)
              center.note->setMidiNoteFromPitchCorrection(
                  corrected + std::copysign(maximumDeviation, signedDeviation));
            else
              center.note->setMidiNoteFromPitchCorrection(sourceMidi);
          }
        }
        pianoRoll.repaint();
      },
      [safeThis, states, amount, previewEdited, refreshCurves, renderPreview, apply, track](bool accepted) {
        if (!safeThis) return;
        if (accepted && !*previewEdited) return;
        safeThis->pianoRoll.endMacroCurvePreview();
        bool changed = false;
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          changed |= std::abs(*amount - state.originalPitchCenter) > 0.0001f;
          for (auto& center : state.notes) {
            center.finalMidi = center.note->getMidiNote();
            center.finalDrift = center.note->getPitchDrift();
            center.finalMacro = accepted ? center.note->getPitchCenterMacro() : center.macro;
            if (!accepted)
              center.note->setPitchCenterMacro(center.macro); // cancelled: nothing applied
            changed |= std::abs(center.finalMidi - center.midiNote) > 0.0001f ||
                       std::abs(center.finalDrift - center.pitchDrift) > 0.0001f;
            if (!accepted) {
              center.note->setMidiNoteFromPitchCorrection(center.midiNote);
              center.note->setPitchDrift(center.pitchDrift);
            }
          }
        }
        if (!accepted) {
          refreshCurves();
          renderPreview(); // Replace any auditioned audio with the original.
          return;
        }
        if (!changed) return;
        // The release callback already requested the final audio. Commit only
        // settings/history here, without re-dirtying notes or rendering again.
        for (auto& state : *states) {
          if (state.target.isValid && !state.target.isValid()) continue;
          state.target.project->setPitchCenter(*amount);
          state.target.project->setModified(true);
          if (state.target.onCommit) state.target.onCommit();
        }
        if (safeThis->pianoRoll.onPitchEditCommitted)
          safeThis->pianoRoll.onPitchEditCommitted();
        if (auto* undo = safeThis->pianoRoll.getUndoManager())
          undo->addAction(std::make_unique<MacroAction>(apply, track));
      }, [refreshCurves, renderPreview] {
        refreshCurves();
        renderPreview();
      });
}

void PianoRollWorkspaceView::setRegionPreviews(
    MainViewRegionPreviewList previews)
{
  // The overview shows this track only; pinned tracks are canvas-only.
  overviewPanel.setRegionPreviews(withoutPinnedRegions(previews));
}

void PianoRollWorkspaceView::refreshOverview()
{
  if (overviewVisible)
    overviewPanel.invalidateThumbnailCache();
}

void PianoRollWorkspaceView::repaintOverviewViewport()
{
  if (overviewVisible)
    overviewPanel.repaint();
}

void PianoRollWorkspaceView::setShowSegmentsDebug(bool show)
{
  overviewPanel.setShowSegmentsDebug(show);
}

void PianoRollWorkspaceView::setOverviewVisible(bool visible)
{
  overviewVisible = visible;
  overviewToggleButton.setToggleState(visible, juce::dontSendNotification);
  overviewAnimationActive = false;
  overviewAnimationProgress = visible ? 1.0f : 0.0f;
  overviewCard.setVisible(visible);
  overviewPanel.setVisible(visible);
  pianoRoll.setHorizontalScrollBarVisible(!visible);
  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewVisibility()
{
  overviewAnimationStartProgress = overviewAnimationProgress;
  overviewAnimationStartMs = juce::Time::getMillisecondCounter();
  overviewAnimationActive = true;

  if (overviewVisible)
  {
    overviewCard.setVisible(true);
    overviewPanel.setVisible(true);
    pianoRoll.setHorizontalScrollBarVisible(false);
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::updateOverviewAnimation()
{
  if (!overviewAnimationActive)
    return;

  const float target = overviewVisible ? 1.0f : 0.0f;
  const auto now = juce::Time::getMillisecondCounter();
  const float elapsed =
      static_cast<float>(now - overviewAnimationStartMs) /
      static_cast<float>(overviewAnimationMs);
  const float t = juce::jlimit(0.0f, 1.0f, elapsed);
  const float eased = 1.0f - std::pow(1.0f - t, 3.0f);

  overviewAnimationProgress =
      overviewAnimationStartProgress +
      (target - overviewAnimationStartProgress) * eased;

  if (t >= 1.0f)
  {
    overviewAnimationProgress = target;
    overviewAnimationActive = false;

    if (!overviewVisible)
    {
      overviewCard.setVisible(false);
      overviewPanel.setVisible(false);
      pianoRoll.setHorizontalScrollBarVisible(true);
    }
  }

  resized();
  repaint();
}

void PianoRollWorkspaceView::timerCallback()
{
  updateOverviewAnimation();
  noteInspector.refresh();

  const float pps = pianoRoll.getPixelsPerSecond();
  if (std::abs(zoomXSlider.getValue() - pps) > 0.05)
    zoomXSlider.setValue(pps, juce::dontSendNotification);

  const float ppsY = pianoRoll.getPixelsPerSemitone();
  if (std::abs(zoomYSlider.getValue() - ppsY) > 0.05)
    zoomYSlider.setValue(ppsY, juce::dontSendNotification);

  // Relative to the timeline start, like the thumbnail's view state.
  const double cursorTime =
      pianoRoll.getCursorTime() - pianoRoll.getTimelineStart();
  if (overviewVisible && std::abs(lastOverviewCursorTime - cursorTime) > 0.0001)
  {
    const double previousCursorTime = lastOverviewCursorTime;
    lastOverviewCursorTime = cursorTime;
    overviewPanel.repaintPlayhead(previousCursorTime, cursorTime);
  }
}

void PianoRollWorkspaceView::refreshLocalisedText()
{
  autoZoomButton.setTooltip(TR("tooltip.vertical_auto_zoom"));
}

// ---------------------------------------------------------------------------
// Quantize Time, Align to Reference and Leveling macros.
// ---------------------------------------------------------------------------

namespace {

// Notes a macro edits: the selection, or every note when nothing is
// selected. Rests are never edited directly.
std::vector<Note *> macroNotes(Project &project) {
  std::vector<Note *> selected, all;
  for (auto &note : project.getNotes()) {
    if (note.isRest()) continue;
    all.push_back(&note);
    if (note.isSelected()) selected.push_back(&note);
  }
  return selected.empty() ? all : selected;
}

// Frames per quarter note at the project's tempo.
double framesPerBeat(const Project &project) {
  const double bpm = std::max(1.0, project.getTimelineTempoBpm());
  return 60.0 / bpm * static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
}

// Undo/redo through two callbacks.
class LambdaAction final : public UndoableAction {
public:
  LambdaAction(juce::String actionName, std::function<void()> undoFn,
               std::function<void()> redoFn)
      : name(std::move(actionName)), undoCallback(std::move(undoFn)),
        redoCallback(std::move(redoFn)) {}
  void undo() override { if (undoCallback) undoCallback(); }
  void redo() override { if (redoCallback) redoCallback(); }
  juce::String getName() const override { return name; }
  bool requiresAudioResynthesis() const override { return false; }
private:
  juce::String name;
  std::function<void()> undoCallback, redoCallback;
};

// New note spans plus the neighbours joined to them (rests included)
// moving their shared edge along, as the Timing Tool does.
std::vector<NoteTimingState> spansWithNeighbours(
    Project &project, const std::vector<Note *> &notes,
    const std::vector<NoteMacros::Span> &spans) {
  std::map<Note *, NoteTimingState> after;
  auto &all = project.getNotes();
  const auto stateOf = [&](Note *note) -> NoteTimingState & {
    auto it = after.find(note);
    if (it == after.end())
      it = after.emplace(note, NoteTimingState::capture(*note)).first;
    return it->second;
  };
  const auto isEdited = [&](const Note *note) {
    return std::find(notes.begin(), notes.end(), note) != notes.end();
  };
  for (size_t i = 0; i < notes.size() && i < spans.size(); ++i) {
    Note *note = notes[i];
    const int oldStart = note->getStartFrame(), oldEnd = note->getEndFrame();
    auto &state = stateOf(note);
    state.startFrame = spans[i].start;
    state.endFrame = spans[i].end;
    for (auto &other : all) {
      if (&other == note || isEdited(&other)) continue;
      if (other.getEndFrame() == oldStart && spans[i].start != oldStart) {
        auto &prev = stateOf(&other);
        prev.endFrame = std::max(prev.startFrame + 1, spans[i].start);
      }
      if (other.getStartFrame() == oldEnd && spans[i].end != oldEnd) {
        auto &next = stateOf(&other);
        next.startFrame = std::min(next.endFrame - 1, spans[i].end);
      }
    }
  }
  std::vector<NoteTimingState> result;
  for (auto &[note, state] : after)
    if (state.startFrame != note->getStartFrame() || state.endFrame != note->getEndFrame())
      result.push_back(state);
  return result;
}

// A note's musical start comes after its leading sibilant ("s"
// in "sweet") and its musical end before the trailing one; timing macros
// place those, and the sibilants move along unstretched.
int sibilantLead(const Note &note) {
  if (!note.hasSibilantInfo() || note.isAllSibilant() || note.getSibilantHead() <= 0)
    return 0;
  const auto b = NoteTimeMap::sibilantBounds(note);
  return std::clamp(static_cast<int>(std::lround(b.headEnd)) - note.getStartFrame(), 0,
                    std::max(0, note.getDurationFrames() - 1));
}

int sibilantTrail(const Note &note) {
  if (!note.hasSibilantInfo() || note.isAllSibilant() || note.getSibilantTail() <= 0)
    return 0;
  const auto b = NoteTimeMap::sibilantBounds(note);
  return std::clamp(note.getEndFrame() - static_cast<int>(std::lround(b.tailStart)), 0,
                    std::max(0, note.getDurationFrames() - 1 - sibilantLead(note)));
}

void restoreTiming(const std::vector<NoteTimingState> &states) {
  for (const auto &state : states)
    if (state.note) {
      state.note->setStartFrame(state.startFrame);
      state.note->setEndFrame(state.endFrame);
    }
}

} // namespace

void PianoRollWorkspaceView::showMacro(int which) {
  if (which == 0) showQuantizeTimePopup();
  else if (which == 1) showAlignPopup();
  else if (which == 2) showLevelingPopup();
  else if (which == 3) showRandomDeviationsPopup();
}

void PianoRollWorkspaceView::commitTiming(Project &project,
                                          const std::vector<NoteTimingState> &before,
                                          const std::vector<NoteTimingState> &after,
                                          const juce::String &name) {
  // As the Timing Tool commits: restore, apply, record, render.
  restoreTiming(before);
  if (after.empty()) return;
  applyNoteTimingStates(project, after);
  pianoRoll.invalidateBasePitchCache();
  if (auto *undo = pianoRoll.getUndoManager())
    undo->addAction(std::make_unique<TimingAction>(project, before, after, name));
  if (pianoRoll.onPitchEditFinished) pianoRoll.onPitchEditFinished();
  pianoRoll.repaint();
}

void PianoRollWorkspaceView::showQuantizeTimePopup() {
  auto *project = pianoRoll.getProject();
  if (!project) return;
  const auto notes = macroNotes(*project);
  if (notes.empty()) return;

  // Every note's current span, to preview from and to restore.
  auto before = std::make_shared<std::vector<NoteTimingState>>();
  for (auto &note : project->getNotes())
    before->push_back(NoteTimingState::capture(note));
  const auto references = pianoRoll.referenceTrackNotes();

  juce::StringArray grids{"Auto (1/16)", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
                          "1/4 T", "1/8 T", "1/16 T"};
  if (!references.empty()) grids.add("Reference track");
  std::vector<MacroDialog::Field> fields;
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Choice; f.id = "grid";
    f.label = "Groove"; f.options = grids; f.value = 0; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.id = "intensity"; f.label = "Intensity";
    f.value = 100; f.suffix = " %"; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Toggle; f.id = "manual";
    f.label = "Include hand-timed"; f.value = 0; fields.push_back(f);
  }

  const auto compute = [project, notes, before, references](const MacroDialog::Values &v) {
    restoreTiming(*before);
    const int grid = static_cast<int>(v.at("grid"));
    const double beat = framesPerBeat(*project);
    const double fractions[] = {0.25, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,
                                2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0};
    const bool useReference = grid >= 10;
    std::vector<int> refStarts;
    if (useReference)
      for (const auto &ref : references) refStarts.push_back(ref.start);
    const bool includeManual = v.at("manual") > 0.5;

    std::vector<NoteMacros::Span> spans;
    std::vector<bool> movable;
    std::vector<Note *> order;
    std::vector<int> leads;
    for (auto &note : project->getNotes()) {
      order.push_back(&note);
      leads.push_back(sibilantLead(note));
      spans.push_back({note.getStartFrame() + leads.back(), note.getEndFrame()});
      const bool chosen = std::find(notes.begin(), notes.end(), &note) != notes.end();
      const bool handTimed = note.getStartFrame() != note.getSrcStartFrame();
      movable.push_back(chosen && !note.isRest() && (includeManual || !handTimed));
    }
    const auto result = NoteMacros::quantizeStarts(
        spans, useReference ? 0.0 : beat * fractions[std::clamp(grid, 0, 9)], 0.0,
        v.at("intensity") / 100.0, refStarts, static_cast<int>(beat), movable);
    std::vector<NoteTimingState> after;
    for (size_t i = 0; i < order.size(); ++i)
      if (result[i].start != spans[i].start || result[i].end != spans[i].end)
        after.push_back({order[i], result[i].start - leads[i], result[i].end});
    return after;
  };

  auto safe = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  MacroDialog::showPopup(
      this, juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0),
      "Quantize Time", fields,
      [safe, compute](const MacroDialog::Values &v, bool) {
        if (!safe) return;
        restoreTiming(compute(v)); // writes the previewed spans
        safe->pianoRoll.invalidateBasePitchCache();
        safe->pianoRoll.repaint();
      },
      [safe, project, before, compute](bool accepted, const MacroDialog::Values &v) {
        if (!safe) return;
        const auto after = accepted ? compute(v) : std::vector<NoteTimingState>{};
        safe->commitTiming(*project, *before, after, "Quantize Time");
        safe->pianoRoll.invalidateBasePitchCache();
        safe->pianoRoll.repaint();
      });
}

void PianoRollWorkspaceView::showAlignPopup() {
  auto *project = pianoRoll.getProject();
  if (!project) return;
  const auto notes = macroNotes(*project);
  const auto references = pianoRoll.referenceTrackNotes();
  if (notes.empty()) return;
  if (references.empty()) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Align to Reference",
        "Turn on the grey Reference button of the track to follow (the lead), "
        "then run Align to Reference on this track (the double or backing).");
    return;
  }

  auto before = std::make_shared<std::vector<NoteTimingState>>();
  auto midiBefore = std::make_shared<std::map<Note *, float>>();
  for (auto &note : project->getNotes()) {
    before->push_back(NoteTimingState::capture(note));
    (*midiBefore)[&note] = note.getMidiNote();
  }

  std::vector<MacroDialog::Field> fields;
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Choice; f.id = "timing";
    f.label = "Timing"; f.value = 2;
    f.options = juce::StringArray{"Leave", "Starts", "Starts and ends"};
    fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.id = "tIntensity"; f.label = "Timing amount";
    f.value = 100; f.suffix = " %"; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Choice; f.id = "pitch";
    f.label = "Pitch"; f.value = 0;
    f.options = juce::StringArray{"Leave", "Unison", "Major third up (+4)",
                                  "Minor third up (+3)", "Fourth up (+5)", "Fifth up (+7)",
                                  "Octave up (+12)", "Minor third down (-3)",
                                  "Major third down (-4)", "Fourth down (-5)",
                                  "Fifth down (-7)", "Octave down (-12)"};
    fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.id = "pIntensity"; f.label = "Pitch amount";
    f.value = 100; f.suffix = " %"; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.id = "reach"; f.label = "Max distance";
    f.min = 10; f.max = 500; f.step = 10; f.value = 150; f.suffix = " ms";
    fields.push_back(f);
  }

  const auto compute = [project, notes, before, midiBefore, references](
                           const MacroDialog::Values &v) {
    restoreTiming(*before);
    for (auto &[note, midi] : *midiBefore) note->setMidiNote(midi);
    std::vector<NoteMacros::Span> spans;
    std::vector<float> midi;
    std::vector<std::pair<int, int>> sibilants;
    for (auto *note : notes) {
      sibilants.emplace_back(sibilantLead(*note), sibilantTrail(*note));
      spans.push_back({note->getStartFrame() + sibilants.back().first,
                       note->getEndFrame() - sibilants.back().second});
      midi.push_back(note->getMidiNote());
    }
    NoteMacros::AlignOptions options;
    const int timing = static_cast<int>(v.at("timing"));
    options.timingIntensity = timing == 0 ? 0.0 : v.at("tIntensity") / 100.0;
    options.alignEnds = timing == 2;
    options.maxReachFrames = secondsToFrames(static_cast<float>(v.at("reach") / 1000.0));
    const int pitch = static_cast<int>(v.at("pitch"));
    const double intervals[] = {0, 0, 4, 3, 5, 7, 12, -3, -4, -5, -7, -12};
    options.pitchMode = pitch == 0 ? 0 : (pitch == 1 ? 1 : 2);
    options.intervalSemitones = intervals[std::clamp(pitch, 0, 11)];
    options.pitchIntensity = v.at("pIntensity") / 100.0;
    auto aligned = NoteMacros::alignToReference(spans, midi, references, options);
    for (size_t i = 0; i < aligned.size() && i < sibilants.size(); ++i) {
      aligned[i].span.start -= sibilants[i].first;
      aligned[i].span.end += sibilants[i].second;
    }
    return aligned;
  };

  auto safe = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  MacroDialog::showPopup(
      this, juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0),
      "Align to Reference", fields,
      [safe, project, notes, compute](const MacroDialog::Values &v, bool) {
        if (!safe) return;
        const auto aligned = compute(v);
        for (size_t i = 0; i < notes.size() && i < aligned.size(); ++i) {
          notes[i]->setStartFrame(aligned[i].span.start);
          notes[i]->setEndFrame(aligned[i].span.end);
          notes[i]->setMidiNote(aligned[i].midi);
        }
        PitchCurveProcessor::rebuildBaseFromNotes(*project);
        safe->pianoRoll.invalidateBasePitchCache();
        safe->pianoRoll.repaint();
      },
      [safe, project, notes, before, midiBefore, compute](bool accepted,
                                                          const MacroDialog::Values &v) {
        if (!safe) return;
        const auto aligned = accepted ? compute(v) : std::vector<NoteMacros::AlignedNote>{};
        // compute() restored spans and pitches; on Cancel that is all.
        restoreTiming(*before);
        for (auto &[note, midi] : *midiBefore) note->setMidiNote(midi);
        PitchCurveProcessor::rebuildBaseFromNotes(*project);
        if (!accepted) {
          safe->pianoRoll.invalidateBasePitchCache();
          safe->pianoRoll.repaint();
          return;
        }

        // Pitch: its own undo step.
        auto midiNew = std::make_shared<std::map<Note *, float>>();
        auto midiOld = std::make_shared<std::map<Note *, float>>();
        for (size_t i = 0; i < notes.size() && i < aligned.size(); ++i)
          if (std::abs(notes[i]->getMidiNote() - aligned[i].midi) > 1e-4f) {
            (*midiNew)[notes[i]] = aligned[i].midi;
            (*midiOld)[notes[i]] = notes[i]->getMidiNote();
          }
        if (!midiNew->empty()) {
          const auto setMidi = [project](const std::map<Note *, float> &values) {
            int first = std::numeric_limits<int>::max(), last = -1;
            for (const auto &[note, midi] : values) {
              note->setMidiNote(midi);
              note->markDirty();
              note->markSynthDirty();
              first = std::min(first, note->getStartFrame());
              last = std::max(last, note->getEndFrame());
            }
            PitchCurveProcessor::rebuildBaseFromNotes(*project);
            if (last >= first) project->markNoteEditDirtyRange(first, last);
          };
          setMidi(*midiNew);
          if (auto *undo = safe->pianoRoll.getUndoManager())
            undo->addAction(std::make_unique<LambdaAction>(
                "Align Pitch to Reference", [setMidi, midiOld] { setMidi(*midiOld); },
                [setMidi, midiNew] { setMidi(*midiNew); }));
        }

        // Timing: through the Timing Tool's path, neighbours following.
        std::vector<NoteMacros::Span> spans;
        for (const auto &a : aligned) spans.push_back(a.span);
        const auto after = spansWithNeighbours(*project, notes, spans);
        if (!after.empty()) {
          safe->commitTiming(*project, *before, after, "Align Timing to Reference");
          return;
        }
        safe->pianoRoll.invalidateBasePitchCache();
        if (safe->pianoRoll.onPitchEditFinished) safe->pianoRoll.onPitchEditFinished();
        safe->pianoRoll.repaint();
      });
}

void PianoRollWorkspaceView::showLevelingPopup() {
  auto *project = pianoRoll.getProject();
  if (!project) return;
  const auto notes = macroNotes(*project);
  if (notes.size() < 2) return;

  std::vector<float> levels, original;
  std::vector<int> lengths;
  for (auto *note : notes) {
    NoteAmplitude::update(*note, project->getAudioData());
    const float source = std::max(1e-6f, note->getSourceAmplitude());
    levels.push_back(20.0f * std::log10(source) + note->getVolumeDb());
    lengths.push_back(note->getEndFrame() - note->getStartFrame());
    original.push_back(note->getVolumeDb());
  }

  std::vector<MacroDialog::Field> fields;
  {
    MacroDialog::Field f; f.id = "quiet"; f.label = "Quiet notes up";
    f.value = 0; f.suffix = " %"; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.id = "loud"; f.label = "Loud notes down";
    f.value = 0; f.suffix = " %"; fields.push_back(f);
  }

  const auto targets = [notes, levels, lengths, original](const MacroDialog::Values &v) {
    const auto gains = NoteMacros::levelingGains(levels, lengths, v.at("quiet") / 100.0,
                                                 v.at("loud") / 100.0);
    std::vector<float> result;
    for (size_t i = 0; i < notes.size(); ++i)
      result.push_back(original[i] <= NoteGainCurve::kDeletedNoteDb
                           ? original[i]
                           : juce::jlimit(-60.0f, 24.0f, original[i] + gains[i]));
    return result;
  };

  auto safe = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  MacroDialog::showPopup(
      this, juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0),
      "Leveling", fields,
      [safe, notes, targets](const MacroDialog::Values &v, bool) {
        if (!safe) return;
        const auto values = targets(v);
        for (size_t i = 0; i < notes.size(); ++i) notes[i]->setVolumeDb(values[i]);
        safe->pianoRoll.repaint();
      },
      [safe, project, notes, original, targets](bool accepted, const MacroDialog::Values &v) {
        if (!safe) return;
        for (size_t i = 0; i < notes.size(); ++i) notes[i]->setVolumeDb(original[i]);
        if (accepted) {
          // Through the amplitude path, as the amplitude handle commits.
          auto action = std::make_unique<AmplitudeAction>(
              *project, notes, original, targets(v),
              [safe] {
                if (safe && safe->pianoRoll.onPitchEdited) safe->pianoRoll.onPitchEdited();
              },
              "Leveling");
          action->redo();
          if (auto *undo = safe->pianoRoll.getUndoManager())
            undo->addAction(std::move(action));
        }
        safe->pianoRoll.repaint();
      });
}

void PianoRollWorkspaceView::showRandomDeviationsPopup() {
  // "Add Random Deviations": humanise pitch and/or timing of the
  // selected notes, light / medium / strong.
  auto *project = pianoRoll.getProject();
  if (!project) return;
  const auto notes = macroNotes(*project);
  if (notes.empty()) return;

  auto before = std::make_shared<std::vector<NoteTimingState>>();
  for (auto &note : project->getNotes()) before->push_back(NoteTimingState::capture(note));
  std::vector<float> midis, offsets;
  for (auto *note : notes) {
    midis.push_back(note->getMidiNote());
    offsets.push_back(note->getPitchOffset());
  }
  // Fixed draws per note, so moving a choice back and forth is stable.
  std::vector<float> pitchDraw, timeDraw;
  juce::Random random(static_cast<juce::int64>(juce::Time::getMillisecondCounter()));
  for (size_t i = 0; i < notes.size(); ++i) {
    pitchDraw.push_back(random.nextFloat() * 2.0f - 1.0f);
    timeDraw.push_back(random.nextFloat() * 2.0f - 1.0f);
  }

  const juce::StringArray amounts{"Off", "Light", "Medium", "Strong"};
  std::vector<MacroDialog::Field> fields;
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Choice; f.id = "pitch";
    f.label = "Pitch"; f.options = amounts; f.value = 1; fields.push_back(f);
  }
  {
    MacroDialog::Field f; f.kind = MacroDialog::Field::Kind::Choice; f.id = "time";
    f.label = "Timing"; f.options = amounts; f.value = 0; fields.push_back(f);
  }
  // Largest deviation per step: cents, milliseconds.
  const float centsFor[] = {0.0f, 6.0f, 14.0f, 28.0f};
  const float msFor[] = {0.0f, 8.0f, 18.0f, 35.0f};

  const auto pitchTargets = [notes, midis, pitchDraw, centsFor](const MacroDialog::Values &v) {
    const int step = juce::jlimit(0, 3, static_cast<int>(v.at("pitch")));
    std::vector<float> result;
    for (size_t i = 0; i < notes.size(); ++i)
      result.push_back(midis[i] + pitchDraw[i] * centsFor[step] / 100.0f);
    return result;
  };
  const auto timeTargets = [project, notes, before, timeDraw, msFor](const MacroDialog::Values &v) {
    restoreTiming(*before);
    const int step = juce::jlimit(0, 3, static_cast<int>(v.at("time")));
    std::vector<NoteMacros::Span> spans;
    const double framesPerMs = static_cast<double>(SAMPLE_RATE) / HOP_SIZE / 1000.0;
    for (size_t i = 0; i < notes.size(); ++i) {
      const int shift = static_cast<int>(std::lround(timeDraw[i] * msFor[step] * framesPerMs));
      const int start = notes[i]->getStartFrame(), end = notes[i]->getEndFrame();
      const int newStart = juce::jlimit(0, std::max(0, end - 1), start + shift);
      spans.push_back({newStart, end});
    }
    return spansWithNeighbours(*project, notes, spans);
  };

  auto safe = juce::Component::SafePointer<PianoRollWorkspaceView>(this);
  MacroDialog::showPopup(
      this, juce::Rectangle<int>(pianoCard.getRight(), pianoCard.getY(), 0, 0),
      "Add Random Deviations", fields,
      [safe, notes, pitchTargets, timeTargets](const MacroDialog::Values &v, bool) {
        if (!safe) return;
        const auto pitch = pitchTargets(v);
        for (size_t i = 0; i < notes.size(); ++i) {
          notes[i]->setMidiNoteFromPitchCorrection(pitch[i]);
          notes[i]->setPitchOffset(0.0f);
        }
        restoreTiming(timeTargets(v));
        PitchCurveProcessor::rebuildBaseFromNotes(*safe->pianoRoll.getProject());
        safe->pianoRoll.invalidateBasePitchCache();
        safe->pianoRoll.repaint();
      },
      [safe, project, notes, midis, offsets, before, pitchTargets, timeTargets](
          bool accepted, const MacroDialog::Values &v) {
        if (!safe) return;
        const auto pitch = pitchTargets(v);
        const auto timing = accepted ? timeTargets(v) : std::vector<NoteTimingState>{};
        for (size_t i = 0; i < notes.size(); ++i) {
          notes[i]->setMidiNoteFromPitchCorrection(midis[i]);
          notes[i]->setPitchOffset(offsets[i]);
        }
        if (accepted) {
          std::vector<Note *> changed;
          std::vector<float> oldMidi, oldOffset, newMidi;
          for (size_t i = 0; i < notes.size(); ++i)
            if (std::abs(pitch[i] - midis[i]) > 1.0e-4f) {
              changed.push_back(notes[i]);
              oldMidi.push_back(midis[i]);
              oldOffset.push_back(offsets[i]);
              newMidi.push_back(pitch[i]);
            }
          if (!changed.empty()) {
            auto rebuild = [project](const std::vector<Note *> &edited) {
              PitchCurveProcessor::rebuildBaseFromNotes(*project);
              int first = std::numeric_limits<int>::max(), last = -1;
              for (auto *n : edited) {
                n->markSynthDirty();
                first = std::min(first, n->getStartFrame());
                last = std::max(last, n->getEndFrame());
              }
              if (last >= first) project->markNoteEditDirtyRange(first, last);
            };
            auto action = std::make_unique<MultiNoteSnapToSemitoneAction>(
                changed, oldMidi, oldOffset, newMidi, rebuild);
            action->redo();
            if (auto *undo = safe->pianoRoll.getUndoManager()) undo->addAction(std::move(action));
          }
          safe->commitTiming(*project, *before, timing, "Add Random Deviations");
          if (!changed.empty() && timing.empty()) {
            safe->pianoRoll.invalidateBasePitchCache();
            if (safe->pianoRoll.onPitchEdited) safe->pianoRoll.onPitchEdited();
            if (safe->pianoRoll.onPitchEditFinished) safe->pianoRoll.onPitchEditFinished();
          }
        } else {
          restoreTiming(*before);
          PitchCurveProcessor::rebuildBaseFromNotes(*project);
        }
        safe->pianoRoll.invalidateBasePitchCache();
        safe->pianoRoll.repaint();
      });
}
