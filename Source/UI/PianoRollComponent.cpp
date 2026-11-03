#include "PianoRollComponent.h"
#include "../Undo/SibilantActions.h"
#include "../Utils/SibilantDetector.h"
#include "../Utils/UI/ViewPrefs.h"
#include <map>
#include "../Utils/PitchMacroScope.h"
#include "../Models/NoteClipboard.h"
#include "../Utils/NoteMacros.h"
#include "../Utils/AppLogger.h"
#include "../Undo/AmplitudeAction.h"
#include "../Undo/NoteActions.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/Constants.h"
#include "../Utils/Localization.h"
#include "../Utils/UI/TimecodeFont.h"
#include "../Utils/UI/Theme.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/ScaleUtils.h"
#include "../Utils/TimingRegionUtils.h"
#include "PianoRoll/PianoRollViewHelpers.h"
#include "PianoRoll/VisualWaveformEnvelope.h"
#include "PianoRoll/States/LoopDragHandler.h"
#include "PianoRoll/States/SelectHandler.h"
#include "PianoRoll/States/SplitHandler.h"
#include "PianoRoll/States/PitchDrawingHandler.h"
#include "PianoRoll/States/TimingHandler.h"
#include "PianoRoll/PitchDrawingConfirmationPanel.h"
#include "Components/PitchPopupMenu.h"
#include "Components/CanvasPopupMenu.h"
#include "../Utils/ReplaceRanges.h"
#include "../Utils/ScaleNames.h"
#include "BinaryData.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace
{
  juce::Image createSplitCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::splitcursor_png,
        static_cast<size_t>(BinaryData::splitcursor_pngSize));
    return image.rescaled(image.getWidth() / 2, image.getHeight() / 2,
                          juce::Graphics::highResamplingQuality);
  }

  juce::Image createMergeCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::mergecursor_png,
        static_cast<size_t>(BinaryData::mergecursor_pngSize));
    return image.rescaled(image.getWidth() / 2, (image.getHeight() + 1) / 2,
                          juce::Graphics::highResamplingQuality);
  }

  juce::Image createZoomCursorImage()
  {
    auto image = juce::ImageFileFormat::loadFrom(
        BinaryData::zoom_png,
        static_cast<size_t>(BinaryData::zoom_pngSize));
    return image.rescaled(image.getWidth() / 2, (image.getHeight() + 1) / 2,
                          juce::Graphics::highResamplingQuality);
  }

  using pianoRollView::getScaleAccentColour;
  using pianoRollView::isBlackKey;
  using pianoRollView::ScaleToneState;
  using pianoRollView::getScaleToneState;
  using pianoRollView::isMultipleOf;

  int normalizeTimelineBeatDenominator(int denominator)
  {
    denominator = juce::jlimit(1, 32, denominator);
    int normalized = 1;
    while (normalized < denominator)
      normalized <<= 1;
    const int lower = normalized >> 1;
    if (lower >= 1 && (denominator - lower) < (normalized - denominator))
      normalized = lower;
    return juce::jlimit(1, 32, normalized);
  }

  double gridDivisionToQuarterNotes(TimelineGridDivision division)
  {
    switch (division)
    {
    case TimelineGridDivision::Whole:
      return 4.0;
    case TimelineGridDivision::Half:
      return 2.0;
    case TimelineGridDivision::Quarter:
      return 1.0;
    case TimelineGridDivision::Eighth:
      return 0.5;
    case TimelineGridDivision::Sixteenth:
      return 0.25;
    case TimelineGridDivision::ThirtySecond:
      return 0.125;
    }
    return 1.0;
  }

  struct NoteEditState
  {
    float midiNote;
    float pitchOffset;
    float volumeDb;
    float formantShift;
    float tiltLeft;
    float tiltRight;
    float vibrato;
    float pitchDrift;
    int smoothLeftFrames;
    int smoothRightFrames;
    float deltaScale;
    float deltaOffset;
    std::vector<float> bakedDeltaPitch;
    std::vector<float> deltaPitch;
    // Correct Pitch value the note remembers; -1 when never corrected.
    float pitchCenterMacro = -1.0f;

    static NoteEditState capture(const Note& note)
    {
      return {note.getMidiNote(), note.getPitchOffset(), note.getVolumeDb(), note.getFormantShift(),
              note.getTiltLeft(), note.getTiltRight(), note.getVibrato(), note.getPitchDrift(),
              note.getSmoothLeftFrames(), note.getSmoothRightFrames(),
              note.getDeltaScale(), note.getDeltaOffset(),
              note.getBakedDeltaPitch(), note.getDeltaPitch(), note.getPitchCenterMacro()};
    }

    static NoteEditState defaultsFor(const Note& note)
    {
      return {note.getOriginalMidiNote(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
              0, 0, 1.0f, 0.0f, {}, note.getOriginalDeltaPitch(), -1.0f};
    }

    void applyTo(Note& note) const
    {
      note.setPitchCenterMacro(pitchCenterMacro);
      note.setMidiNote(midiNote);
      note.setPitchOffset(pitchOffset);
      note.setVolumeDb(volumeDb);
      note.setFormantShift(formantShift);
      note.setTiltLeft(tiltLeft);
      note.setTiltRight(tiltRight);
      note.setVibrato(vibrato);
      note.setPitchDrift(pitchDrift);
      note.setSmoothLeftFrames(smoothLeftFrames);
      note.setSmoothRightFrames(smoothRightFrames);
      note.setDeltaScale(deltaScale);
      note.setDeltaOffset(deltaOffset);
      note.setBakedDeltaPitch(bakedDeltaPitch);
      note.setDeltaPitch(deltaPitch);
      note.markDirty();
      note.markSynthDirty();
    }
  };

  // Note::deltaPitch is only the active contour when the note has no
  // analysed originalDeltaPitch (legacy projects). Otherwise it is a scratch
  // copy that stays empty after a fresh scan and is filled from the global
  // curve when a drag starts, so comparing it to the original would flag
  // every untouched note as pitch-edited. Real contour edits live in
  // bakedDeltaPitch, which is compared separately.
  bool hasUnrenderedDeltaEdit(const Note& note, const NoteEditState& current,
                              const NoteEditState& defaults)
  {
    if (note.hasOriginalDeltaPitch())
      return false;
    return current.deltaPitch != defaults.deltaPitch;
  }

  bool hasPitchEdits(const Note& note)
  {
    const auto current = NoteEditState::capture(note);
    const auto defaults = NoteEditState::defaultsFor(note);
    return current.midiNote != defaults.midiNote ||
           current.pitchOffset != defaults.pitchOffset ||
           current.tiltLeft != defaults.tiltLeft ||
           current.tiltRight != defaults.tiltRight ||
           current.vibrato != defaults.vibrato ||
           current.pitchDrift != defaults.pitchDrift ||
           current.smoothLeftFrames != defaults.smoothLeftFrames ||
           current.smoothRightFrames != defaults.smoothRightFrames ||
           current.deltaScale != defaults.deltaScale ||
           current.deltaOffset != defaults.deltaOffset ||
           current.bakedDeltaPitch != defaults.bakedDeltaPitch ||
           hasUnrenderedDeltaEdit(note, current, defaults);
  }

  bool hasFormantEdits(const Note& note) { return note.getFormantShift() != 0.0f; }
  bool hasAmplitudeEdits(const Note& note) { return note.getVolumeDb() != 0.0f; }
  bool hasTimingEdits(const Note& note)
  {
    return note.getStartFrame() != note.getSrcStartFrame() ||
           note.getEndFrame() != note.getSrcEndFrame();
  }

  class ResetNoteEditsAction final : public UndoableAction
  {
  public:
    ResetNoteEditsAction(Project& project, std::vector<Note*> notes,
                         bool pitch, bool formant, bool amplitude,
                         juce::String name, std::unique_ptr<UndoableAction> timing = {})
        : project(project), notes(std::move(notes)), name(std::move(name)),
          timing(std::move(timing))
    {
      before.reserve(this->notes.size());
      after.reserve(this->notes.size());
      for (const auto* note : this->notes)
      {
        before.push_back(NoteEditState::capture(*note));
        auto state = pitch ? NoteEditState::defaultsFor(*note) : before.back();
        state.formantShift = formant ? 0.0f : before.back().formantShift;
        state.volumeDb = amplitude ? 0.0f : before.back().volumeDb;
        after.push_back(std::move(state));
      }
    }

    void undo() override { if (timing) timing->undo(); apply(before); }
    void redo() override { apply(after); if (timing) timing->redo(); }
    juce::String getName() const override { return name; }

  private:
    void apply(const std::vector<NoteEditState>& states)
    {
      int dirtyStart = std::numeric_limits<int>::max();
      int dirtyEnd = std::numeric_limits<int>::min();
      for (size_t i = 0; i < notes.size() && i < states.size(); ++i)
      {
        if (!notes[i])
          continue;
        states[i].applyTo(*notes[i]);
        dirtyStart = std::min(dirtyStart, notes[i]->getStartFrame());
        dirtyEnd = std::max(dirtyEnd, notes[i]->getEndFrame());
      }
      project.setModified(true);
      PitchCurveProcessor::rebuildBaseFromNotes(project);
      // Same range the edit and its undo mark (Project::markNoteEditDirtyRange).
      project.markNoteEditDirtyRange(dirtyStart, dirtyEnd);
    }

    Project& project;
    std::vector<Note*> notes;
    std::vector<NoteEditState> before;
    std::vector<NoteEditState> after;
    juce::String name;
    std::unique_ptr<UndoableAction> timing;
  };
}

PianoRollComponent::PianoRollComponent()
    : splitMouseCursor(createSplitCursorImage(), 10, 8),
      mergeMouseCursor(createMergeCursorImage(), 10, 8),
      zoomMouseCursor(createZoomCursorImage(), 6, 6)
{
  // Initialize modular components
  coordMapper = std::make_unique<CoordinateMapper>();
  pianoKeysRenderer = std::make_unique<PianoKeysRenderer>();
  gridRenderer = std::make_unique<GridRenderer>();
  timelineRenderer = std::make_unique<TimelineRenderer>();
  waveformBackgroundRenderer = std::make_unique<WaveformBackgroundRenderer>();
  noteRenderer = std::make_unique<NoteRenderer>();
  pitchCurveRenderer = std::make_unique<PitchCurveRenderer>();
  // Track mode draws inactive regions with their own renderer instances. They
  // get no interaction handlers, so drags, hovers and selections on the
  // active region can never show up in them.
  previewNoteRenderer = std::make_unique<NoteRenderer>();
  previewCurveRenderer = std::make_unique<PitchCurveRenderer>();
  previewNoteRenderer->setGreyscale(false);
  scrollZoomController = std::make_unique<ScrollZoomController>();
  pitchEditor = std::make_unique<PitchEditor>();
  boxSelector = std::make_unique<BoxSelector>();
  noteSplitter = std::make_unique<NoteSplitter>();
  pitchToolHandles = std::make_unique<PitchToolHandles>();
  pitchToolController = std::make_unique<PitchToolController>();
  interactionContext = std::make_unique<PianoRollInteractionContext>(*this);

  // Initialize interaction handlers
  loopDragHandler_ = std::make_unique<LoopDragHandler>(*this);
  selectHandler_ = std::make_unique<SelectHandler>(*this);
  splitHandler_ = std::make_unique<SplitHandler>(*this);
  pitchDrawingHandler_ = std::make_unique<PitchDrawingHandler>(*this);
  timingHandler_ = std::make_unique<TimingHandler>(*this);
  currentHandler_ = selectHandler_.get();

  // Wire up components
  pianoKeysRenderer->setCoordinateMapper(coordMapper.get());
  gridRenderer->setCoordinateMapper(coordMapper.get());
  timelineRenderer->setCoordinateMapper(coordMapper.get());
  waveformBackgroundRenderer->setCoordinateMapper(coordMapper.get());
  waveformBackgroundRenderer->setPitchToolController(pitchToolController.get());
  // Lets the waveform renderer request a full-quality rebuild once a zoom settles.
  waveformBackgroundRenderer->onRepaintRequested = [this] { repaint(); };
  noteRenderer->setCoordinateMapper(coordMapper.get());
  noteRenderer->setSelectHandler(selectHandler_.get());
  noteRenderer->setSplitHandler(splitHandler_.get());
  noteRenderer->setPitchEditor(pitchEditor.get());
  noteRenderer->setPitchToolController(pitchToolController.get());
  noteRenderer->setBoxSelector(boxSelector.get());
  // Note colours follow the key in force at each note (key changes too).
  noteRenderer->setKeyForNote([this](const Note &note) -> std::optional<ScaleUtils::Key> {
    if (project == nullptr)
      return std::nullopt;
    const auto key = ScaleUtils::keyAt(
        *project, projectToTimeline(framesToSeconds(note.getStartFrame())));
    if (ScaleUtils::hasKey(key))
      return key;
    return std::nullopt;
  });
  pitchCurveRenderer->setCoordinateMapper(coordMapper.get());
  previewNoteRenderer->setCoordinateMapper(coordMapper.get());
  previewCurveRenderer->setCoordinateMapper(coordMapper.get());
  pitchCurveRenderer->setSelectHandler(selectHandler_.get());
  pitchCurveRenderer->setPitchEditor(pitchEditor.get());
  scrollZoomController->setCoordinateMapper(coordMapper.get());
  pitchEditor->setCoordinateMapper(coordMapper.get());
  noteSplitter->setCoordinateMapper(coordMapper.get());

  // Setup scrollZoomController callbacks
  scrollZoomController->onRepaintNeeded = [this]()
  { repaint(); };
  scrollZoomController->onZoomChanged = [this](float pps)
  {
    if (onZoomChanged)
      onZoomChanged(pps);
  };
  scrollZoomController->onScrollChanged = [this](double x)
  {
    if (onScrollChanged)
      onScrollChanged(x);
  };

  // Setup pitchEditor callbacks
  pitchEditor->onNoteSelected = [this](Note *note)
  {
    if (onNoteSelected)
      onNoteSelected(note);
  };
  pitchEditor->onPitchEdited = [this]()
  {
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };
  pitchEditor->onPitchEditFinished = [this]()
  {
    if (onPitchEditFinished)
      onPitchEditFinished();
  };
  pitchEditor->onBasePitchCacheInvalidated = [this]()
  {
    invalidateBasePitchCache();
  };

  // Setup pitchToolController callbacks
  pitchToolController->onPitchEdited = [this]()
  {
    invalidateWaveformCache();
    updatePitchToolHandlesFromSelection();
    updatePreviewButtonBounds();
    repaint();
    if (onPitchEdited)
      onPitchEdited();
  };

  pitchToolController->onAmplitudeEdited = [this]()
  {
    invalidateWaveformCache();
    updatePitchToolHandlesFromSelection();
    updatePreviewButtonBounds();
    repaint();
    if (onAmplitudeEdited) onAmplitudeEdited();
  };

  // Setup noteSplitter callbacks
  noteSplitter->onNoteSplit = [this]()
  {
    invalidateBasePitchCache();
    repaint();
  };

  addAndMakeVisible(horizontalScrollBar);
  addAndMakeVisible(verticalScrollBar);

  // Use scrollZoomController's scrollbars
  addAndMakeVisible(scrollZoomController->getHorizontalScrollBar());
  addAndMakeVisible(scrollZoomController->getVerticalScrollBar());

  horizontalScrollBar.addListener(this);
  horizontalScrollBar.addMouseListener(&scrollBarDoubleClick, true);
  verticalScrollBar.addMouseListener(&scrollBarDoubleClick, true);
  verticalScrollBar.addListener(this);

  // Style scrollbars to match theme
  styleCanvasScrollBar(horizontalScrollBar);
  styleCanvasScrollBar(verticalScrollBar);

  // Set initial scroll range
  verticalScrollBar.setRangeLimits(0, (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) *
                                          pixelsPerSemitone);
  verticalScrollBar.setCurrentRange(0, 500);

  // Default view centered on C3-C4 (MIDI 48-60)
  centerOnPitchRange(48.0f, 60.0f);

  // Enable keyboard focus for shortcuts
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  // No extra controls here; overview lives outside the piano roll.
  int previewImageSize = 0;
  if (const auto *previewImageData =
          BinaryData::getNamedResource("preview_png", previewImageSize))
  {
    previewButton.setImage(
        juce::ImageFileFormat::loadFrom(previewImageData, previewImageSize));
    previewButtonWidth = std::max(1, previewButton.getWidth());
    previewButtonHeight = std::max(1, previewButton.getHeight());
  }
  previewButton.setTooltip(TR("tooltip.context_audition"));
  previewButton.setVisible(false);
  previewButton.onClick = [this]()
  {
    if (hoveredNote)
      triggerPreviewForNote(*hoveredNote);
  };
  addAndMakeVisible(previewButton);

  int resetImageSize = 0;
  if (const auto *resetImageData =
          BinaryData::getNamedResource("reset_png", resetImageSize))
  {
    resetButton.setImage(
        juce::ImageFileFormat::loadFrom(resetImageData, resetImageSize));
    resetButtonWidth = std::max(1, resetButton.getWidth());
    resetButtonHeight = std::max(1, resetButton.getHeight());
  }
  resetButton.setTooltip(TR("tooltip.restore_original"));
  resetButton.setVisible(false);
  resetButton.onClick = [this]()
  {
    if (hoveredNote)
      showResetMenu(*hoveredNote);
  };
  addAndMakeVisible(resetButton);

  pitchDrawingConfirmationPanel = std::make_unique<PitchDrawingConfirmationPanel>();
  pitchDrawingConfirmationPanel->onApply = [this]
  {
    if (!pitchDrawingHandler_ || !pitchDrawingHandler_->apply())
      return;

    // The preview has already been rendered. Confirmation only promotes the
    // current transient state into undo history and persistent project state.
    invalidateBasePitchCache();
    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditCommitted)
      onPitchEditCommitted();
  };
  pitchDrawingConfirmationPanel->onCancel = [this]
  {
    if (pitchDrawingHandler_)
      pitchDrawingHandler_->cancel();
  };
  addAndMakeVisible(*pitchDrawingConfirmationPanel);
  pitchDrawingConfirmationPanel->setVisible(false);
}

PianoRollComponent::~PianoRollComponent()
{
  horizontalScrollBar.removeListener(this);
  horizontalScrollBar.removeMouseListener(&scrollBarDoubleClick);
  verticalScrollBar.removeMouseListener(&scrollBarDoubleClick);
  verticalScrollBar.removeListener(this);
}

int PianoRollComponent::getVisibleContentWidth() const
{
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  return std::max(0, getWidth() - pianoKeysWidth - verticalScrollBarSize);
}

int PianoRollComponent::getVisibleContentHeight() const
{
  constexpr int horizontalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  return std::max(0, getHeight() - headerHeight -
                         (showHorizontalScrollBar ? horizontalScrollBarSize : 0));
}

void PianoRollComponent::setHorizontalScrollBarVisible(bool shouldShow)
{
  if (showHorizontalScrollBar == shouldShow)
    return;

  showHorizontalScrollBar = shouldShow;
  horizontalScrollBar.setVisible(showHorizontalScrollBar);
  resized();
  repaint();
}

void PianoRollComponent::paint(juce::Graphics &g)
{
  updatePitchToolHandlesFromSelection();

  // Stars Clique: the editor is a rounded card.
  if (Skin::starsClique())
  {
    juce::Path card;
    card.addRoundedRectangle(getLocalBounds().toFloat(), 22.0f);
    g.reduceClipRegion(card);
  }

  // Background (solid to keep grid clean). Stars Clique: the backdrop under a
  // dark violet veil, the header a touch lighter.
  if (Skin::starsClique())
  {
    Skin::paintBackdrop(g, *this);
    g.fillAll(juce::Colour(0x9E100824u));
    g.setColour(juce::Colours::white.withAlpha(0.03f));
    g.fillRect(0, 0, getWidth(), headerHeight);
  }
  else
  {
    g.fillAll(APP_COLOR_BACKGROUND);
    g.setColour(Skin::colour(0xFF232323u));
    g.fillRect(0, 0, getWidth(), headerHeight);
  }

  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  auto contentBounds = getLocalBounds();

  g.setColour(APP_COLOR_SCROLLBAR_TRACK);
  g.fillRect(getWidth() - verticalScrollBarSize, 0, verticalScrollBarSize,
             headerHeight);
  g.fillRect(getWidth() - verticalScrollBarSize, headerHeight,
             verticalScrollBarSize,
             getHeight() - headerHeight - horizontalScrollBarSize);
  if (showHorizontalScrollBar)
  {
    g.fillRect(0, getHeight() - horizontalScrollBarSize, pianoKeysWidth,
               horizontalScrollBarSize);
    g.fillRect(pianoKeysWidth, getHeight() - horizontalScrollBarSize,
               getWidth() - pianoKeysWidth - verticalScrollBarSize,
               horizontalScrollBarSize);
    g.fillRect(getWidth() - verticalScrollBarSize,
               getHeight() - horizontalScrollBarSize,
               verticalScrollBarSize, horizontalScrollBarSize);
  }

  // Create clipping region for main area (below timelines)
  auto mainArea = contentBounds
                      .withTrimmedLeft(pianoKeysWidth)
                      .withTrimmedTop(headerHeight)
                      .withTrimmedBottom(horizontalScrollBarSize)
                      .withTrimmedRight(verticalScrollBarSize);

  // Draw row backgrounds first, then waveform, then grid lines/content.
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));
    drawGrid(g, true, false);
  }

  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    drawBackgroundWaveform(g, mainArea);
  }

  // Draw scrolled content (grid lines, notes, pitch curves, handles)
  {
    juce::Graphics::ScopedSaveState saveState(g);
    g.reduceClipRegion(mainArea);
    g.setOrigin(pianoKeysWidth - static_cast<int>(scrollX),
                headerHeight - static_cast<int>(scrollY));

    drawGrid(g, false, true);
    drawRegionPreviews(g);
    drawInactiveRegionEdits(g, mainArea);
    drawPinnedTrackNotes(g, mainArea);
    drawAudioSourceRegionOverlay(g);
    drawLoopOverlay(g);

    // Track mode shows the active modification only through its region's
    // window; the rest of the take would sit on top of the other regions.
    std::optional<juce::Graphics::ScopedSaveState> trackClip;
    if (const auto span = getTrackModeActiveSpan())
    {
      trackClip.emplace(g);
      const float height =
          (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
      g.reduceClipRegion(juce::Rectangle<float>(
                             timeToX(span->first), 0.0f,
                             timeToX(span->second) - timeToX(span->first),
                             height)
                             .getSmallestIntegerContainer());
    }

    drawGameChunksDebugOverlay(g);
    if (ownTrackEditable)
    {
      drawNotes(g, NoteRenderPass::Body);
      drawNotes(g, NoteRenderPass::HoverShadow);
      drawNotes(g, NoteRenderPass::HoveredBody);
      drawPitchCurves(g);
      if (currentHandler_)
        currentHandler_->draw(g);
      drawNotes(g, NoteRenderPass::Overlay);
    }
    drawTrackAffiliationDimming(g);
    drawGameValuesDebugOverlay(g);
    drawSelectionRect(g);

    // Draw pitch tool handles in world space (transform applied by g.setOrigin above)
    if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty())
    {
      pitchToolHandles->draw(g);
    }
  }

  if (trackGestureActive && trackBoxSelecting) {
    const auto rect = juce::Rectangle<float>(trackGestureStart, trackGestureEnd);
    g.setColour(juce::Colours::white.withAlpha(0.05f)); g.fillRect(rect);
    g.setColour(juce::Colours::white.withAlpha(0.25f)); g.drawRect(rect);
  }
  // Draw timeline (above grid, scrolls horizontally)
  drawTimeline(g);
  drawReplaceRanges(g);
  drawKeyMarkers(g);
  drawLoopTimeline(g);

  // Timing boundaries use the full canvas height. Hide the stationary
  // playhead in Timing mode so it cannot be mistaken for an editable edge.
  if (editMode != EditMode::Timing || playbackActive)
  {
    float x = static_cast<float>(pianoKeysWidth) + timeToX(cursorTime) -
              static_cast<float>(scrollX);
    float cursorTop = static_cast<float>(headerHeight);
    float cursorBottom =
        static_cast<float>(getHeight() -
                           horizontalScrollBarSize); // Exclude scrollbar

    // Only draw if cursor is in visible area
    if (x >= pianoKeysWidth && x < getWidth() - verticalScrollBarSize)
    {
      g.setColour(APP_COLOR_PITCH_CURVE);
      g.fillRect(x - 0.5f, cursorTop, 1.0f, cursorBottom - cursorTop);
    }
  }

  // Draw piano keys
  drawPianoKeys(g);

  g.setColour(Skin::colour(0xFF0D0B0Bu));
  g.drawVerticalLine(pianoKeysWidth, 0.0f,
                     static_cast<float>(getHeight() - horizontalScrollBarSize));

  const auto canvasBorderBounds = getLocalBounds()
                                      .withTrimmedRight(verticalScrollBarSize)
                                      .withTrimmedBottom(horizontalScrollBarSize);
  g.setColour(Skin::colour(0xFF3C3C3Cu));
  g.drawRect(canvasBorderBounds, 1);
}

void PianoRollComponent::resized()
{
  auto bounds = getLocalBounds();
  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;

  if (showHorizontalScrollBar)
  {
    horizontalScrollBar.setBounds(
        pianoKeysWidth, bounds.getHeight() - horizontalScrollBarSize,
        bounds.getWidth() - pianoKeysWidth - verticalScrollBarSize,
        horizontalScrollBarSize);
  }
  else
  {
    horizontalScrollBar.setBounds({});
  }

  verticalScrollBar.setBounds(
      bounds.getWidth() - verticalScrollBarSize, headerHeight,
      verticalScrollBarSize,
      bounds.getHeight() - horizontalScrollBarSize - headerHeight);

  updateScrollBars();
  updatePreviewButtonBounds();
  updatePitchDrawingConfirmationPopup();
}

void PianoRollComponent::drawBackgroundWaveform(
    juce::Graphics &g, const juce::Rectangle<int> &visibleArea)
{
  if (!ownTrackEditable)
    return; // the own track is switched off: none of it shows

  // Cheap when unchanged; picks up a region resize restamping the span.
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  waveformBackgroundRenderer->draw(g, visibleArea);
}

void PianoRollComponent::invalidateWaveformCache()
{
  waveformBackgroundRenderer->invalidateCache();
}

void PianoRollComponent::beginMacroCurvePreview(const std::vector<Note*>& notes)
{
  pitchCurveRenderer->beginMacroPreview(notes);
}

void PianoRollComponent::beginInactiveMacroPreview(
    const std::vector<TrackPitchMacroTarget>& targets)
{
  if (!multiTrackEditing() || !regionPreviews || !inactiveMacroPreviews.empty()) return;
  for (const auto& region : *regionPreviews) {
    if (region.active || region.pinned || !region.project) continue;
    auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& item)
        { return item.project == region.sourceProject; });
    if (target == targets.end() || (target->isValid && !target->isValid())) continue;
    InactiveMacroPreview preview;
    preview.project = region.project;
    preview.isValid = target->isValid;
    std::multimap<int, const Note*> sourceNotes;
    for (const auto& note : target->project->getNotes())
      sourceNotes.emplace(note.getStartFrame(), &note);
    for (auto& display : region.project->getNotes()) {
      const auto matches = sourceNotes.equal_range(display.getStartFrame() + region.sourceFirstFrame);
      for (auto it = matches.first; it != matches.second; ++it)
        if (it->second->getEndFrame() == display.getEndFrame() + region.sourceFirstFrame) {
          preview.bindings.push_back({&display, it->second,
                                     display.getMidiNote(), display.getPitchDrift()});
          break;
        }
    }
    inactiveMacroPreviews.push_back(std::move(preview));
  }
  lastInactiveMacroUpdate = 0.0;
}

void PianoRollComponent::endMacroCurvePreview()
{
  pitchCurveRenderer->endMacroPreview();
  for (auto& preview : inactiveMacroPreviews)
    for (const auto& binding : preview.bindings) {
      binding.display->setMidiNoteFromPitchCorrection(binding.originalMidi);
      binding.display->setPitchDrift(binding.originalDrift);
    }
  inactiveMacroPreviews.clear();
  inactiveLayer = {};
}

void PianoRollComponent::invalidateBasePitchCache()
{
  pitchCurveRenderer->invalidateBasePitchCache();
}

void PianoRollComponent::drawGrid(juce::Graphics &g, bool drawRowBackgrounds,
                                  bool drawGridLines)
{
  GridRenderer::Params params;
  const bool scaleViewActive =
      selectedScaleMode != ScaleMode::None &&
      selectedScaleMode != ScaleMode::Chromatic;
  params.scaleMode = scaleViewActive
                         ? previewScaleMode.value_or(selectedScaleMode)
                         : selectedScaleMode;
  params.scaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  params.pitchAxisOffsetSemitones =
      ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
  params.timelineDisplayMode = timelineDisplayMode;
  params.gridSeconds = getTimelineGridSeconds();
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.timelineDuration = getTimelineDuration();
  params.componentWidth = getWidth();
  params.visibleContentWidth = getVisibleContentWidth();
  params.visibleContentHeight = getVisibleContentHeight();
  params.drawRowBackgrounds = drawRowBackgrounds;
  params.pitchLines = pitchLinesBackground;
  params.drawGridLines = drawGridLines;

  // Key changes: each stretch between them is drawn with its own key.
  const auto *markers = project ? &project->getMacroParameters()->keyMarkers : nullptr;
  if (!scaleViewActive || markers == nullptr || markers->empty() ||
      previewScaleMode.has_value() || previewScaleRootNote.has_value())
  {
    gridRenderer->draw(g, params);
    return;
  }
  const float height = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const auto drawStretch = [&](float x1, float x2, int root, ScaleMode mode)
  {
    if (x2 <= x1)
      return;
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(juce::Rectangle<float>(x1, -1.0f, x2 - x1, height + 2.0f)
                           .getSmallestIntegerContainer());
    auto stretch = params;
    stretch.scaleRootNote = root;
    stretch.scaleMode = mode;
    gridRenderer->draw(g, stretch);
  };
  constexpr float farLeft = -1.0e7f, farRight = 1.0e7f;
  float left = farLeft;
  int root = selectedScaleRootNote;
  ScaleMode mode = selectedScaleMode;
  for (const auto &marker : *markers)
  {
    const float x = timeToX(timelineToProject(marker.seconds));
    drawStretch(left, x, root, mode);
    left = x;
    root = marker.root;
    mode = marker.mode;
  }
  drawStretch(left, farRight, root, mode);
}

void PianoRollComponent::drawAudioSourceRegionOverlay(juce::Graphics &g)
{
  if (trackViewMode) return;
  if (!project)
    return;

  const auto &audioData = project->getAudioData();
  const double duration = static_cast<double>(audioData.getDuration());
  if (audioData.waveform.getNumSamples() <= 0 || duration <= 0.0)
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  auto drawRange = [&](double startSeconds, double endSeconds,
                       bool drawBoundaries) {
    startSeconds = std::max(0.0, startSeconds);
    endSeconds = std::max(startSeconds, endSeconds);
    if (endSeconds <= startSeconds)
      return;
    const float startX = timeToX(startSeconds);
    const float endX = timeToX(endSeconds);
    g.setColour(juce::Colours::white.withAlpha(0.04f));
    g.fillRect(startX, 0.0f, endX - startX, height);
    if (drawBoundaries) {
      g.setColour(juce::Colours::white.withAlpha(0.25f));
      g.fillRect(startX - 0.5f, 0.0f, 1.0f, height);
      g.fillRect(endX - 0.5f, 0.0f, 1.0f, height);
    }
  };

  if (!audioData.playbackRegionRanges.empty()) {
    for (const auto &[start, end] : audioData.playbackRegionRanges)
      drawRange(start, end, true);
  } else {
    // Standalone is one continuous source: shade all visible bars as its
    // region and do not show a false audio-end boundary in the canvas.
    const double visibleEndSeconds =
        xToTime(static_cast<float>(scrollX + getVisibleContentWidth()));
    drawRange(audioData.timelineOffsetSeconds,
              std::max(duration, visibleEndSeconds), false);
  }
}

void PianoRollComponent::setRegionPreviews(MainViewRegionPreviewList previews)
{
  regionPreviews = std::move(previews);
  waveformBackgroundRenderer->setRegionPreviews(
      withoutPinnedRegions(regionPreviews));
  regionPreviewsEndSeconds = 0.0;
  if (regionPreviews)
    for (const auto &region : *regionPreviews)
      regionPreviewsEndSeconds =
          std::max(regionPreviewsEndSeconds, region.endSeconds);

  // The pointer may now sit over a region that changed state.
  if (swallowGestureUntilMouseUp && !isMouseButtonDown())
    swallowGestureUntilMouseUp = false;

  updateScrollBars();
  repaint();
}

const MainViewRegionPreview *PianoRollComponent::findActiveRegionPreview() const
{
  if (!regionPreviews)
    return nullptr;
  for (const auto &region : *regionPreviews)
    if (region.active)
      return &region;
  return nullptr;
}

bool PianoRollComponent::previewContains(const MainViewRegionPreview &region,
                                         double projectSeconds) const
{
  // Previews are placed in host-timeline seconds; the canvas is in the active
  // modification's time.
  return region.contains(projectToTimeline(projectSeconds));
}

const MainViewRegionPreview* PianoRollComponent::findTrackRegionAtPointer(
    juce::Point<float> point, bool notesOnly) const
{
  if (!trackViewMode || !regionPreviews) return nullptr;
  const double hostTime = projectToTimeline(xToTime(point.x - pianoKeysWidth + scrollX));
  const float worldY = point.y - headerHeight + scrollY;
  const MainViewRegionPreview* background = nullptr;
  // Stable drawing order, with actual note hits before empty region space.
  // The routing/bound-region flag must not influence interaction priority.
  for (auto it = regionPreviews->rbegin(); it != regionPreviews->rend(); ++it) {
    const auto& region = *it;
    if (region.pinned || !region.contains(hostTime)) continue;
    if (!background) background = &region;
    const bool live = region.sourceProject && region.sourceProject == project;
    const Project* display = live ? project : region.project.get();
    if (!display) continue;
    const double offset = region.projectHostOffsetSeconds -
        (live ? framesToSeconds(region.sourceFirstFrame) : 0.0);
    for (const auto& note : display->getNotes()) {
      if (note.isRest()) continue;
      const double start = framesToSeconds(note.getVisualStartFrame()) + offset;
      const double end = framesToSeconds(note.getVisualEndFrame()) + offset;
      const float top = coordMapper->midiToY(note.getAdjustedMidiNote());
      if (hostTime >= start && hostTime < end && worldY >= top && worldY < top + pixelsPerSemitone)
        return &region;
    }
  }
  return notesOnly ? nullptr : background;
}

bool PianoRollComponent::prepareTrackPointer(juce::Point<float> point, bool notesOnly)
{
  if (!trackViewMode || !onInactiveRegionClicked || trackGestureActive || isMouseButtonDown()) return false;
  const float worldX = point.x - pianoKeysWidth + scrollX;
  const float worldY = point.y - headerHeight + scrollY;
  if (hoveredNote && pitchToolHandles && pitchToolHandles->containsLayoutPoint(worldX, worldY))
    return false;
  if (pitchDrawingHandler_ && pitchDrawingHandler_->hasAnchors()) return false;
  const auto* region = findTrackRegionAtPointer(point, notesOnly);
  if (!region) return false;
  const double offset = region->projectHostOffsetSeconds - framesToSeconds(region->sourceFirstFrame);
  const auto* boundWindow = findActiveRegionPreview();
  if (region->sourceProject == project && juce::approximatelyEqual(offset, projectToTimeline(0.0)) &&
      boundWindow && boundWindow->key == region->key)
    return false;
  const auto key = region->key;
  const auto* before = project;
  onInactiveRegionClicked(key);
  return before != project || !juce::approximatelyEqual(offset, projectToTimeline(0.0));
}

void PianoRollComponent::drawInactiveRegionEdits(
    juce::Graphics &g, const juce::Rectangle<int> &mainArea)
{
  if (!multiTrackEditing() || !regionPreviews || regionPreviews->empty() ||
      mainArea.isEmpty() || pixelsPerSecond <= 0.0f)
    return;

  // Preserve the active region's waveform geometry with a light-centred grey
  // note gradient, then desaturate the remaining edits in the cached layer.
  // Playhead and hover repaints only blit it.
  InactiveLayerKey key;
  key.scrollX = scrollX;
  key.scrollY = scrollY;
  key.pixelsPerSecond = pixelsPerSecond;
  key.pixelsPerSemitone = pixelsPerSemitone;
  key.width = mainArea.getWidth();
  key.height = mainArea.getHeight();
  key.previews = regionPreviews.get();
  key.displayOffset = projectToTimeline(0.0);
  key.showDeltaPitch = showDeltaPitch;
  key.showBasePitch = showBasePitch;

  const double now = juce::Time::getMillisecondCounterHiRes();
  const bool updateMacro = !inactiveMacroPreviews.empty() &&
                          now - lastInactiveMacroUpdate >= 33.0;
  if (updateMacro) lastInactiveMacroUpdate = now;
  if (!inactiveLayer.isValid() || !(key == inactiveLayerKey) || updateMacro)
  {
    inactiveLayerKey = key;
    inactiveLayer = juce::Image(juce::Image::ARGB, key.width, key.height, true);
    juce::Graphics lg(inactiveLayer);

    const double visibleHostStart =
        projectToTimeline(scrollX / pixelsPerSecond);
    const double visibleHostEnd = projectToTimeline(
        (scrollX + key.width) / pixelsPerSecond);
    const float height =
        (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
    const double savedScrollX = coordMapper->getScrollX();

    for (const auto &region : *regionPreviews)
    {
      if (region.active || region.pinned || region.project == nullptr ||
          region.endSeconds <= visibleHostStart ||
          region.startSeconds >= visibleHostEnd)
        continue;

      // Draw the region's window as if the canvas were scrolled onto it: the
      // window project's zero sits at this canvas time.
      const double zero = timelineToProject(region.projectHostOffsetSeconds);
      const double localScrollX = scrollX - zero * pixelsPerSecond;
      coordMapper->setScrollX(localScrollX);

      juce::Graphics::ScopedSaveState state(lg);
      lg.addTransform(juce::AffineTransform::translation(
          static_cast<float>(-localScrollX),
          static_cast<float>(-scrollY) + spreadOffsetFor(region)));
      const double windowStart =
          region.startSeconds - region.projectHostOffsetSeconds;
      const double windowEnd =
          region.endSeconds - region.projectHostOffsetSeconds;
      lg.reduceClipRegion(
          juce::Rectangle<float>(
              static_cast<float>(windowStart * pixelsPerSecond), 0.0f,
              static_cast<float>((windowEnd - windowStart) * pixelsPerSecond),
              height)
              .getSmallestIntegerContainer());

      auto* curveRenderer = previewCurveRenderer.get();
      auto macro = std::find_if(inactiveMacroPreviews.begin(), inactiveMacroPreviews.end(),
          [&](const auto& item) { return item.project == region.project; });
      if (macro != inactiveMacroPreviews.end() &&
          (!macro->isValid || macro->isValid())) {
        if (!macro->renderer) {
          macro->renderer = std::make_unique<PitchCurveRenderer>();
          macro->renderer->setCoordinateMapper(coordMapper.get());
          macro->renderer->setProject(region.project.get());
          std::vector<Note*> notes;
          for (const auto& binding : macro->bindings) notes.push_back(binding.display);
          macro->renderer->beginMacroPreview(notes);
        }
        for (const auto& binding : macro->bindings) {
          binding.display->setMidiNoteFromPitchCorrection(binding.source->getMidiNote());
          binding.display->setPitchDrift(binding.source->getPitchDrift());
        }
        curveRenderer = macro->renderer.get();
      } else {
        previewCurveRenderer->setProject(region.project.get());
      }
      previewNoteRenderer->setProject(region.project.get());
      previewNoteRenderer->draw(lg, NoteRenderer::Pass::Body, false, getWidth());

      PitchCurveRenderer::Params params;
      params.showDeltaPitch = showDeltaPitch;
      params.showBasePitch = showBasePitch;
      params.showUvInterpolationDebug = false;
      params.showActualF0Debug = false;
      params.showCleanedF0Debug = false;
      params.showVocoderF0Debug = false;
      params.componentWidth = getWidth();
      curveRenderer->draw(lg, params);
    }

    coordMapper->setScrollX(savedScrollX);
    previewNoteRenderer->setProject(nullptr);
    previewCurveRenderer->setProject(nullptr);

  }

  // World coordinates: the graphics origin is already scrolled. JUCE draws
  // images with the current colour's opacity, and the region outlines just
  // set a faint one - so set it explicitly. Slightly below full opacity to
  // keep inactive regions a step darker than the active one.
  constexpr float inactiveLayerOpacity = 1.0f;
  juce::Graphics::ScopedSaveState opaque(g);
  g.setOpacity(inactiveLayerOpacity);
  g.drawImageAt(inactiveLayer, static_cast<int>(scrollX),
                static_cast<int>(scrollY));
}

void PianoRollComponent::drawPinnedTrackNotes(
    juce::Graphics &g, const juce::Rectangle<int> &mainArea)
{
  if ((!trackViewMode && !pinnedNotesInClipMode) || !regionPreviews ||
      regionPreviews->empty() || mainArea.isEmpty() || pixelsPerSecond <= 0.0f)
    return;

  // Ghost (reference) tracks: grey waveform blobs - no pitch curve or
  // handles - so they read as a reference behind this track's own notes.
  // Drawn from the notes and audio each preview carries, in host seconds.
  const double visibleHostStart =
      projectToTimeline(xToTime(static_cast<float>(scrollX)));
  const double visibleHostEnd = projectToTimeline(
      xToTime(static_cast<float>(scrollX + mainArea.getWidth())));
  const float rowHeight = pixelsPerSemitone;
  const float blobHeight = juce::jmax(3.0f, rowHeight * 0.78f);
  const float corner = juce::jmin(blobHeight * 0.5f, 6.0f);

  for (const auto &region : *regionPreviews)
  {
    if (!region.pinned || region.endSeconds <= visibleHostStart ||
        region.startSeconds >= visibleHostEnd)
      continue;

    // Grey waveform blobs, shaped like this track's coloured ones; the grey
    // is lighter the better the note sits: in the key and on its semitone
    // is light, off pitch darker, outside the key darkest.
    const ScaleMode mode = project ? project->getScaleMode() : ScaleMode::None;
    const int root = project ? project->getScaleRootNote() : -1;
    const int referenceHz = project ? project->getPitchReferenceHz() : 440;
    const bool hasKey = mode != ScaleMode::None && mode != ScaleMode::Chromatic && root >= 0;
    const float *samples = region.waveformSamples ? region.waveformSamples->data() : nullptr;
    const int totalSamples =
        region.waveformSamples ? static_cast<int>(region.waveformSamples->size()) : 0;
    const double sampleRate = region.waveformSampleRate;
    for (const auto &note : region.notes)
    {
      if (note.endSeconds <= visibleHostStart ||
          note.startSeconds >= visibleHostEnd)
        continue;
      // The key in force at the note (key changes), tuned by the custom
      // scale when that is the key.
      const auto key = project ? ScaleUtils::keyAt(*project, timelineToProject(note.startSeconds))
                               : ScaleUtils::Key{root, mode};
      const bool noteHasKey = hasKey || key.mode == ScaleMode::Custom ||
          (key.mode != ScaleMode::None && key.mode != ScaleMode::Chromatic && key.root >= 0);
      const float snapped = ScaleUtils::snapMidiToTunedSemitone(note.midi, key.mode, referenceHz);
      const float offPitch = juce::jlimit(0.0f, 1.0f, std::abs(note.midi - snapped) / 0.5f);
      const bool inKey = !noteHasKey || ScaleUtils::isPitchClassInScale(
                                            key.mode, static_cast<int>(std::lround(snapped)), key.root);
      const float accuracy = inKey ? 1.0f - 0.55f * offPitch : 0.15f;
      const juce::Colour centre =
          Skin::colour(0xFF5A5A5Au).interpolatedWith(Skin::colour(0xFFE2E2E2u), accuracy);
      const juce::Colour side = centre.darker(0.9f);

      const float x1 = timeToX(timelineToProject(note.startSeconds));
      const float x2 = timeToX(timelineToProject(note.endSeconds));
      const float w = x2 - x1;
      if (w < 1.0f)
        continue;
      const float centreY = midiToY(note.midi) + rowHeight * 0.5f + spreadOffsetFor(region);

      juce::Path shape;
      if (samples != nullptr && totalSamples > 0 && sampleRate > 0.0 && w > 2.0f)
      {
        const int first = juce::jlimit(0, totalSamples - 1, static_cast<int>(
            (note.startSeconds - region.waveformStartSeconds) * sampleRate));
        const int last = juce::jlimit(first + 1, totalSamples, static_cast<int>(
            (note.endSeconds - region.waveformStartSeconds) * sampleRate));
        const int points = std::max(2, std::min(2048, static_cast<int>(std::ceil(w * 2.0f)) + 1));
        const auto values = VisualWaveformEnvelope::build(samples, totalSamples, first, last,
                                                          points, w, sampleRate,
                                                          pixelsPerSecond, true, 1.0f);
        // Same height as the coloured blobs: three rows for full scale.
        const float half = rowHeight * 1.5f;
        const float minHalf = juce::jmax(1.5f, rowHeight * 0.12f);
        const auto yAt = [&](size_t i, float sign) {
          return centreY + sign * juce::jmax(minHalf, values[i] * half);
        };
        const auto xAt = [&](size_t i) {
          return x1 + w * static_cast<float>(i) / static_cast<float>(values.size() - 1);
        };
        shape.startNewSubPath(xAt(0), yAt(0, -1.0f));
        for (size_t i = 1; i < values.size(); ++i)
          shape.lineTo(xAt(i), yAt(i, -1.0f));
        for (size_t i = values.size(); i-- > 0;)
          shape.lineTo(xAt(i), yAt(i, 1.0f));
        shape.closeSubPath();
      }
      else
      {
        shape.addRoundedRectangle(x1, centreY - blobHeight * 0.5f, w, blobHeight, corner);
      }

      const auto bounds = shape.getBounds();
      juce::ColourGradient gradient(side.withAlpha(0.75f), bounds.getCentreX(), bounds.getY(),
                                    side.withAlpha(0.75f), bounds.getCentreX(), bounds.getBottom(),
                                    false);
      if (bounds.getHeight() > 0.0f)
        gradient.addColour(juce::jlimit(0.0, 1.0, static_cast<double>(
                               (centreY - bounds.getY()) / bounds.getHeight())),
                           centre.withAlpha(0.9f));
      g.setGradientFill(gradient);
      g.fillPath(shape);
      g.setColour(centre.brighter(0.2f).withAlpha(0.55f));
      g.strokePath(shape, juce::PathStrokeType(0.8f));
    }
  }
}

float PianoRollComponent::spreadOffsetFor(const MainViewRegionPreview &region) const
{
  if (!spreadUnison || !regionPreviews || region.active)
    return 0.0f;
  // One step per other track, in list order: a third of a semitone.
  std::vector<juce::String> keys;
  for (const auto &item : *regionPreviews)
    if (!item.active && std::find(keys.begin(), keys.end(), item.key) == keys.end())
      keys.push_back(item.key);
  const auto it = std::find(keys.begin(), keys.end(), region.key);
  const int index = it == keys.end() ? 0 : static_cast<int>(it - keys.begin()) + 1;
  return index * pixelsPerSemitone / 3.0f;
}

std::vector<NoteMacros::RefNote> PianoRollComponent::referenceTrackNotes() const
{
  // Notes of the reference (grey) tracks, in this project's frames, sorted.
  std::vector<NoteMacros::RefNote> notes;
  if (!regionPreviews)
    return notes;
  for (const auto &region : *regionPreviews)
    if (region.pinned)
      for (const auto &note : region.notes)
      {
        NoteMacros::RefNote ref;
        ref.start = secondsToFrames(static_cast<float>(timelineToProject(note.startSeconds)));
        ref.end = secondsToFrames(static_cast<float>(timelineToProject(note.endSeconds)));
        ref.midi = note.midi;
        if (ref.end > ref.start)
          notes.push_back(ref);
      }
  std::sort(notes.begin(), notes.end(),
            [](const auto &a, const auto &b) { return a.start < b.start; });
  return notes;
}

void PianoRollComponent::drawTrackAffiliationDimming(juce::Graphics &g)
{
  // "Highlight Track Affiliation": while a note is held, notes of
  // the other tracks open for editing go dim, so it is clear whose is whose.
  if (affiliationOwner == nullptr || !coEditInClipMode)
    return;
  const auto shade = Skin::colour(0xFF1B1B1Bu).withAlpha(0.7f);
  g.setColour(shade);
  if (project != nullptr && affiliationOwner != project)
    for (const auto &note : project->getNotes())
    {
      if (note.isRest())
        continue;
      const float x1 = timeToX(framesToSeconds(note.getVisualStartFrame()));
      const float x2 = timeToX(framesToSeconds(note.getVisualEndFrame()));
      g.fillRect(x1, midiToY(note.getAdjustedMidiNote()), x2 - x1, pixelsPerSemitone);
    }
  if (!regionPreviews)
    return;
  for (const auto &region : *regionPreviews)
  {
    if (region.pinned || region.active || region.sourceProject == affiliationOwner)
      continue;
    for (const auto &note : region.notes)
    {
      const float x1 = timeToX(timelineToProject(note.startSeconds));
      const float x2 = timeToX(timelineToProject(note.endSeconds));
      g.fillRect(x1, midiToY(note.midi) + spreadOffsetFor(region), x2 - x1,
                 pixelsPerSemitone);
    }
  }
}

juce::String PianoRollComponent::pinnedNoteKeyAt(float worldX, float worldY) const
{
  if ((!trackViewMode && !pinnedNotesInClipMode) || !regionPreviews)
    return {};
  const float rowHeight = pixelsPerSemitone;
  for (const auto &region : *regionPreviews)
  {
    if (!region.pinned)
      continue;
    for (const auto &note : region.notes)
    {
      const float x1 = timeToX(timelineToProject(note.startSeconds));
      const float x2 = timeToX(timelineToProject(note.endSeconds));
      const float y = midiToY(note.midi) + spreadOffsetFor(region);
      if (worldX >= x1 && worldX < x2 && worldY >= y && worldY < y + rowHeight)
        return region.key;
    }
  }
  return {};
}

void PianoRollComponent::drawRegionPreviews(juce::Graphics &g)
{
  // Region frames belong to Track mode; tracks co-edited in Clip mode show
  // only their notes.
  if (!regionPreviews || regionPreviews->empty() ||
      (coEditInClipMode && !trackViewMode))
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  // Previews carry host-timeline seconds; compare in host time and place
  // them on the canvas through the active modification's display offset.
  const double visibleStart =
      projectToTimeline(xToTime(static_cast<float>(scrollX)));
  const double visibleEnd = projectToTimeline(
      xToTime(static_cast<float>(scrollX + getVisibleContentWidth())));
  const auto hx = [this](double hostSeconds) {
    return timeToX(timelineToProject(hostSeconds));
  };

  // drawAudioSourceRegionOverlay() paints the active backdrop from the canvas
  // project. While that project is missing (analysis pending) the active
  // region still gets its backdrop from the preview.
  const bool projectDrawsActiveBackdrop =
      project != nullptr &&
      project->getAudioData().waveform.getNumSamples() > 0 &&
      project->getAudioData().getDuration() > 0.0f;

  for (const auto &region : *regionPreviews)
  {
    // Pinned tracks draw notes only; their region edges would clutter this
    // track's own boundaries.
    if (region.pinned)
      continue;
    if (region.endSeconds <= visibleStart || region.startSeconds >= visibleEnd ||
        region.endSeconds <= region.startSeconds)
      continue;

    const float x1 = hx(region.startSeconds);
    const float x2 = hx(region.endSeconds);

    g.setColour(juce::Colours::white.withAlpha(0.04f));
    g.fillRect(x1, 0.0f, x2 - x1, height);
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    g.fillRect(x1 - 0.5f, 0.0f, 1.0f, height);
    g.fillRect(x2 - 0.5f, 0.0f, 1.0f, height);

  }
}

void PianoRollComponent::drawLoopOverlay(juce::Graphics &g)
{
  if (!project)
    return;

  double loopStartSeconds = 0.0;
  double loopEndSeconds = 0.0;
  bool loopEnabled = false;
  if (loopDragHandler_ && loopDragHandler_->isDragging())
  {
    loopStartSeconds = loopDragHandler_->getDragStartSeconds();
    loopEndSeconds = loopDragHandler_->getDragEndSeconds();
    loopEnabled = true;
  }
  else
  {
    const auto &loopRange = project->getLoopRange();
    loopStartSeconds = loopRange.startSeconds;
    loopEndSeconds = loopRange.endSeconds;
    loopEnabled = loopRange.enabled;
  }

  if (loopStartSeconds > loopEndSeconds)
    std::swap(loopStartSeconds, loopEndSeconds);

  if (loopEndSeconds <= loopStartSeconds)
    return;

  if (!loopEnabled)
    return;

  const float startX = timeToX(loopStartSeconds);
  const float endX = timeToX(loopEndSeconds);

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::white.withAlpha(0.04f));
  g.fillRect(startX, 0.0f, endX - startX, height);
}

void PianoRollComponent::drawGameChunksDebugOverlay(juce::Graphics &g)
{
  if (!showSegmentsDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentChunkRanges.empty())
    return;

  const float height =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

  g.setColour(juce::Colours::orange.withAlpha(0.10f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    g.fillRect(x1, 0.0f, std::max(1.0f, x2 - x1), height);
  }

  g.setColour(juce::Colours::orange.withAlpha(0.75f));
  for (const auto &range : audioData.segmentChunkRanges)
  {
    int startFrame = std::max(0, range.first);
    int endFrame = std::max(startFrame, range.second);
    if (endFrame <= startFrame)
      continue;

    const float x = framesToSeconds(startFrame) * pixelsPerSecond;
    g.drawVerticalLine(static_cast<int>(x), 0.0f, height);
  }
}

void PianoRollComponent::drawGameValuesDebugOverlay(juce::Graphics &g)
{
  if (!showGameValuesDebug || !project)
    return;

  const auto &audioData = project->getAudioData();
  if (audioData.segmentDebugChunks.empty())
    return;

  const int totalFrames = static_cast<int>(audioData.f0.size());
  if (totalFrames <= 0)
    return;

  const int visibleStartFrame = std::max(
      0, static_cast<int>(scrollX / pixelsPerSecond * audioData.sampleRate /
                          HOP_SIZE));
  const int visibleEndFrame = std::min(
      totalFrames, static_cast<int>((scrollX + getVisibleContentWidth()) /
                                    pixelsPerSecond * audioData.sampleRate /
                                    HOP_SIZE) +
                       1);
  if (visibleEndFrame <= visibleStartFrame)
    return;

  const float contentHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  g.setFont(juce::FontOptions(10.5f));
  const int maxChunks = 60;
  int chunksDrawn = 0;

  for (const auto &chunk : audioData.segmentDebugChunks)
  {
    const int startFrame = std::max(0, chunk.startFrame);
    const int endFrame = std::max(startFrame, chunk.endFrame);
    if (endFrame <= startFrame)
      continue;
    if (endFrame <= visibleStartFrame || startFrame >= visibleEndFrame)
      continue;

    int noteCount = 0;
    int restCount = 0;
    int eventLabelsInChunk = 0;

    const float x1 = framesToSeconds(startFrame) * pixelsPerSecond;
    const float x2 = framesToSeconds(endFrame) * pixelsPerSecond;
    const float width = x2 - x1;
    if (width < 8.0f)
      continue;

    const float chunkSeconds =
        static_cast<float>(endFrame - startFrame) * HOP_SIZE /
        static_cast<float>(audioData.sampleRate);

    // Chunk boundary and chunk-level debug label.
    g.setColour(juce::Colours::orange.withAlpha(0.78f));
    g.drawVerticalLine(static_cast<int>(x1), 0.0f, contentHeight);

    // Raw GAME event markers/labels inside this chunk.
    for (size_t i = 0; i < chunk.events.size(); ++i)
    {
      const auto &ev = chunk.events[i];
      if (ev.endFrame <= startFrame || ev.startFrame >= endFrame)
        continue;

      const int overlapStart = std::max(startFrame, ev.startFrame);
      const int overlapEnd = std::min(endFrame, ev.endFrame);
      if (overlapEnd <= overlapStart)
        continue;

      const float ex1 = framesToSeconds(overlapStart) * pixelsPerSecond;
      const float ex2 = framesToSeconds(overlapEnd) * pixelsPerSecond;
      const float ew = std::max(1.0f, ex2 - ex1);

      if (ev.isRest)
      {
        ++restCount;
        // Red: rest segments placed on nearby note lane (not at top).
        float anchorMidi = 60.0f;
        bool foundAnchor = false;
        for (int k = static_cast<int>(i) - 1; k >= 0; --k)
        {
          if (!chunk.events[static_cast<size_t>(k)].isRest)
          {
            anchorMidi = chunk.events[static_cast<size_t>(k)].midiNote;
            foundAnchor = true;
            break;
          }
        }
        if (!foundAnchor)
        {
          for (size_t k = i + 1; k < chunk.events.size(); ++k)
          {
            if (!chunk.events[k].isRest)
            {
              anchorMidi = chunk.events[k].midiNote;
              foundAnchor = true;
              break;
            }
          }
        }
        anchorMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                  static_cast<float>(MAX_MIDI_NOTE),
                                  anchorMidi);
        const float yCenter =
            midiToY(anchorMidi) + pixelsPerSemitone * 0.5f;
        const float restBandHeight = std::max(6.0f, pixelsPerSemitone * 0.62f);
        const float restBandTop = yCenter - restBandHeight * 0.5f;
        g.setColour(juce::Colours::red.withAlpha(0.55f));
        g.fillRect(ex1, restBandTop, ew, restBandHeight);
        g.setColour(juce::Colours::red.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1), restBandTop,
                           restBandTop + restBandHeight);

        if (ew > 40.0f)
        {
          juce::String restTag = "rest";
          if (i == 0)
            restTag = "pre-rest";
          else if (i + 1 == chunk.events.size())
            restTag = "post-rest";
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText(restTag + " d:" + juce::String(overlapEnd - overlapStart),
                           static_cast<int>(ex1) + 2,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 3,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 0.85f);
        }
        else if (ew > 12.0f)
        {
          g.setColour(juce::Colours::white.withAlpha(0.95f));
          g.drawFittedText("R", static_cast<int>(ex1) + 1,
                           static_cast<int>(restBandTop),
                           static_cast<int>(ew) - 1,
                           static_cast<int>(restBandHeight),
                           juce::Justification::centredLeft, 1, 1.0f);
        }
        continue;
      }

      ++noteCount;

      // Black: midi segments (placed on their pitch row).
      const float noteMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                                          static_cast<float>(MAX_MIDI_NOTE),
                                          ev.midiNote);
      const float yCenter = midiToY(noteMidi) + pixelsPerSemitone * 0.5f;
      const float h = std::max(6.0f, pixelsPerSemitone * 0.72f);
      const float ny = yCenter - h * 0.5f;
      g.setColour(juce::Colours::black.withAlpha(0.84f));
      g.fillRoundedRectangle(ex1, ny, ew, h, 2.0f);

      if (ew > 60.0f && eventLabelsInChunk < 80)
      {
        const juce::String noteLabel =
            "ev#" + juce::String(static_cast<int>(i)) + " m:" +
            juce::String(ev.midiNote, 2) + " f:" + juce::String(overlapStart) +
            "-" + juce::String(overlapEnd) + " d:" +
            juce::String(overlapEnd - overlapStart) + " att:" +
            juce::String(ev.attachedStartFrame) + " durS:" +
            juce::String(ev.durationSeconds, 3);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText(noteLabel, static_cast<int>(ex1) + 3,
                         static_cast<int>(ny) - 15,
                         static_cast<int>(std::min(320.0f, ew)), 13,
                         juce::Justification::centredLeft, 1, 0.70f);
        ++eventLabelsInChunk;
      }
      else if (ew > 22.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawFittedText("m:" + juce::String(ev.midiNote, 1),
                         static_cast<int>(ex1) + 2, static_cast<int>(ny),
                         static_cast<int>(ew) - 3, static_cast<int>(h),
                         juce::Justification::centredLeft, 1, 0.9f);
      }
      else if (ew > 8.0f)
      {
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        g.drawVerticalLine(static_cast<int>(ex1 + 0.5f), ny, ny + h);
      }
    }

    const juce::String label =
        "S" + juce::String(chunk.chunkIndex) + " f:" +
        juce::String(startFrame) + "-" + juce::String(endFrame) + " len:" +
        juce::String(endFrame - startFrame) + "f/" +
        juce::String(chunkSeconds, 2) + "s n:" + juce::String(noteCount) +
        " r:" + juce::String(restCount) + " ev:" +
        juce::String(static_cast<int>(chunk.events.size())) + " rstTh:" +
        juce::String(chunk.shortRestThreshold);

    const int textX = static_cast<int>(x1 + 3.0f);
    const int textY = 16;
    const int textWidth = std::max(40, static_cast<int>(width - 6.0f));
    const int textHeight = 14;

    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRect(static_cast<float>(textX), static_cast<float>(textY),
               static_cast<float>(textWidth), static_cast<float>(textHeight));
    g.setColour(juce::Colours::white.withAlpha(0.96f));
    g.drawFittedText(label, textX + 2, textY, textWidth - 4, textHeight,
                     juce::Justification::centredLeft, 1, 0.8f);

    ++chunksDrawn;
    if (chunksDrawn >= maxChunks)
      break;
  }
}

void PianoRollComponent::drawTimeline(juce::Graphics &g)
{
  TimelineRenderer::TimelineParams params;
  params.displayMode = timelineDisplayMode;
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  timelineRenderer->drawTimeline(g, params);
}

bool PianoRollComponent::replaceRangesActive() const
{
  return replaceRangesAvailable && replaceRangesShown && project != nullptr &&
         !project->getReplaceRanges().empty();
}

void PianoRollComponent::drawReplaceRanges(juce::Graphics &g)
{
  if (!replaceRangesActive())
    return;
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(pianoKeysWidth, 0, getWidth() - pianoKeysWidth, timelineHeight);
  for (const auto &r : project->getReplaceRanges())
  {
    const float x1 = static_cast<float>(pianoKeysWidth) + timeToX(r.first) - static_cast<float>(scrollX);
    const float x2 = static_cast<float>(pianoKeysWidth) + timeToX(r.second) - static_cast<float>(scrollX);
    g.setColour(juce::Colours::white.withAlpha(0.13f));
    g.fillRect(x1, 0.0f, x2 - x1, static_cast<float>(timelineHeight - 1));
    g.setColour(juce::Colours::white.withAlpha(0.45f));
    g.fillRect(x1, 0.0f, 1.5f, static_cast<float>(timelineHeight - 1));
    g.fillRect(x2 - 1.5f, 0.0f, 1.5f, static_cast<float>(timelineHeight - 1));
  }
}

void PianoRollComponent::commitReplaceRanges(std::vector<std::pair<double, double>> ranges)
{
  if (!project)
    return;
  project->setReplaceRanges(ReplaceRanges::normalize(std::move(ranges)));
  repaint();
  if (onReplaceRangesEdited)
    onReplaceRangesEdited(project->getReplaceRanges());
}

void PianoRollComponent::setReplaceRangesToNotes()
{
  if (!project)
    return;
  std::vector<std::pair<double, double>> spans;
  for (const auto &note : project->getNotes())
    if (!note.isRest() && note.getVolumeDb() > NoteGainCurve::kDeletedNoteDb + 0.5f)
      spans.emplace_back(framesToSeconds(note.getStartFrame()),
                         framesToSeconds(note.getEndFrame()));
  replaceRangesShown = true;
  commitReplaceRanges(ReplaceRanges::clipToNotes(project->getReplaceRanges(), spans));
}

void PianoRollComponent::resetReplaceRanges()
{
  // Back to the whole transferred audio.
  if (!project)
    return;
  const auto &audio = project->getAudioData();
  if (audio.sampleRate <= 0 || audio.waveform.getNumSamples() == 0)
    return;
  replaceRangesShown = true;
  commitReplaceRanges({{std::max(0.0, audio.timelineOffsetSeconds),
                        audio.waveform.getNumSamples() / static_cast<double>(audio.sampleRate)}});
}

void PianoRollComponent::showReplaceRangesMenu(const juce::MouseEvent &e)
{
  // The time ruler's menu: key changes, and Replace Ranges where Transfer
  // records (hosts without ARA).
  juce::PopupMenu menu;
  const double clickedSeconds = projectToTimeline(std::max(
      0.0, xToTime(e.x - pianoKeysWidth + static_cast<float>(scrollX))));
  const double markerSeconds = projectToTimeline(snapTimeToTimelineGrid(
      timelineToProject(clickedSeconds)));
  const int nearMarker = keyMarkerAt(e.x);

  juce::PopupMenu keyMenu;
  const auto &modes = ScaleNames::modeOptions();
  const auto *macro = project ? project->getMacroParameters().get() : nullptr;
  for (int r = 0; r < 12; ++r)
  {
    juce::PopupMenu rootMenu;
    for (size_t m = 0; m < modes.size(); ++m)
      rootMenu.addItem(1000 + r * 100 + static_cast<int>(m), TR(modes[m].labelKey));
    keyMenu.addSubMenu(ScaleNames::rootName(r), rootMenu);
  }
  keyMenu.addSeparator();
  keyMenu.addItem(1099, ScaleNames::keyName(0, ScaleMode::Custom));
  menu.addSubMenu(TR("keymarker.add"), keyMenu, project != nullptr);
  if (nearMarker >= 0 && macro != nullptr)
  {
    const auto &marker = macro->keyMarkers[static_cast<size_t>(nearMarker)];
    menu.addItem(9, TR("keymarker.remove") + ": " + ScaleNames::keyName(marker.root, marker.mode));
  }
  if (macro != nullptr && !macro->keyMarkers.empty())
    menu.addItem(8, TR("keymarker.clear"));
  if (replaceRangesAvailable)
  {
    menu.addSeparator();
    menu.addItem(1, TR("replace.show"), true, replaceRangesShown);
    menu.addItem(2, TR("replace.to_notes"), project != nullptr);
    menu.addItem(3, TR("replace.reset"), project != nullptr);
  }
  juce::Component::SafePointer<PianoRollComponent> safeThis(this);
  menu.showMenuAsync(juce::PopupMenu::Options()
                         .withTargetScreenArea({e.getScreenX(), e.getScreenY(), 1, 1})
                         .withDeletionCheck(*this),
                     [safeThis, markerSeconds, nearMarker](int result) {
                       if (safeThis == nullptr)
                         return;
                       if (result >= 1000 && result < 2300 && safeThis->project)
                       {
                         const bool custom = result == 1099;
                         const int r = custom ? 0 : (result - 1000) / 100;
                         const int m = (result - 1000) % 100;
                         const auto mode = custom ? ScaleMode::Custom
                             : ScaleNames::modeOptions()[static_cast<size_t>(
                                   juce::jlimit(0, 13, m))].mode;
                         safeThis->addKeyMarker(markerSeconds, r, mode);
                       }
                       else if (result == 9)
                         safeThis->removeKeyMarker(nearMarker);
                       else if (result == 8 && safeThis->project)
                       {
                         safeThis->project->getMacroParameters()->keyMarkers.clear();
                         safeThis->keyMarkersChanged();
                       }
                       else if (result == 1)
                       {
                         safeThis->replaceRangesShown = !safeThis->replaceRangesShown;
                         safeThis->repaint();
                       }
                       else if (result == 2)
                         safeThis->setReplaceRangesToNotes();
                       else if (result == 3)
                         safeThis->resetReplaceRanges();
                     });
}

void PianoRollComponent::drawLoopTimeline(juce::Graphics &g)
{
  TimelineRenderer::LoopParams params;
  params.displayMode = timelineDisplayMode;
  params.beatNumerator = timelineBeatNumerator;
  params.beatSeconds = getTimelineBeatSeconds();
  params.barSeconds = getTimelineBarSeconds();
  params.componentWidth = getWidth();
  params.loopStartSeconds = 0.0;
  params.loopEndSeconds = 0.0;
  params.loopEnabled = false;
  // Match prior behaviour: if there's no project, draw only the empty gutter.
  if (project)
  {
    if (loopDragHandler_ && loopDragHandler_->isDragging())
    {
      params.loopStartSeconds = loopDragHandler_->getDragStartSeconds();
      params.loopEndSeconds = loopDragHandler_->getDragEndSeconds();
      params.loopEnabled = true;
    }
    else
    {
      const auto &loopRange = project->getLoopRange();
      params.loopStartSeconds = loopRange.startSeconds;
      params.loopEndSeconds = loopRange.endSeconds;
      params.loopEnabled = loopRange.enabled;
    }
  }
  timelineRenderer->drawLoopTimeline(g, params);
}

void PianoRollComponent::drawNotes(juce::Graphics &g, NoteRenderPass pass)
{
  auto rendererPass = NoteRenderer::Pass::Overlay;
  if (pass == NoteRenderPass::Body)
    rendererPass = NoteRenderer::Pass::Body;
  else if (pass == NoteRenderPass::HoverShadow)
    rendererPass = NoteRenderer::Pass::HoverShadow;
  else if (pass == NoteRenderPass::HoveredBody)
    rendererPass = NoteRenderer::Pass::HoveredBody;

  const bool splitModeActive = editMode == EditMode::Split;
  noteRenderer->setHoveredNote(hoveredNote);
  noteRenderer->setShowNoteFramesDebug(showNoteFramesDebug);
  noteRenderer->setPreviewPlaybackState(previewPlaybackActive,
                                        previewPlaybackStartFrame,
                                        previewPlaybackEndFrame,
                                        previewPlaybackCurrentTime);
  noteRenderer->draw(g, rendererPass, splitModeActive, getWidth());
}

void PianoRollComponent::drawPitchCurves(juce::Graphics &g)
{
  PitchCurveRenderer::Params params;
  params.showDeltaPitch = showDeltaPitch;
  params.showBasePitch = showBasePitch;
  params.showUvInterpolationDebug = showUvInterpolationDebug;
  params.showActualF0Debug = showActualF0Debug;
  params.showCleanedF0Debug = showCleanedF0Debug;
  params.showVocoderF0Debug = showVocoderF0Debug;
  params.hidePitchCurves = false;
  params.componentWidth = getWidth();

  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
      pitchDrawingHandler_->hasAnchors())
  {
    auto oldParams = params;
    oldParams.showDeltaPitch = true;
    oldParams.showBasePitch = false;
    oldParams.showUvInterpolationDebug = false;
    oldParams.showActualF0Debug = false;
    oldParams.showCleanedF0Debug = false;
    oldParams.showVocoderF0Debug = false;
    oldParams.pitchCurveAlpha = 0.24f;
    oldParams.midiCurveOverride = &pitchDrawingHandler_->getOriginalMidiCurve();
    pitchCurveRenderer->draw(g, oldParams);

    params.showDeltaPitch = true;
    params.pitchCurveAlpha = 1.0f;
    params.midiCurveOverride = &pitchDrawingHandler_->getPreviewMidiCurve();
  }
  pitchCurveRenderer->draw(g, params);
}

void PianoRollComponent::drawPianoKeys(juce::Graphics &g)
{
  ScaleMode activeScaleMode = ScaleMode::Chromatic;
  if (snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale)
  {
    const ScaleMode preferredMode =
        project != nullptr ? project->getPreferredScaleMode() : ScaleMode::Major;
    activeScaleMode = previewScaleMode.value_or(preferredMode);
  }
  const int activeScaleRootNote = previewScaleRootNote.value_or(selectedScaleRootNote);
  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  pianoKeysRenderer->draw(g, getHeight(), horizontalScrollBarSize,
                          activeScaleMode, activeScaleRootNote,
                          ScaleUtils::getReferenceOffsetSemitones(
                              pitchReferenceHz));
}

float PianoRollComponent::midiToY(float midiNote) const
{
  return (MAX_MIDI_NOTE - midiNote) * pixelsPerSemitone;
}

float PianoRollComponent::yToMidi(float y) const
{
  return MAX_MIDI_NOTE - y / pixelsPerSemitone;
}

float PianoRollComponent::timeToX(double time) const
{
  return static_cast<float>(time * pixelsPerSecond);
}

double PianoRollComponent::xToTime(float x) const
{
  return x / pixelsPerSecond;
}

double PianoRollComponent::getTimelineQuarterNoteSeconds() const
{
  const double bpm = juce::jlimit(20.0, 300.0, timelineTempoBpm);
  return bpm > 0.0 ? 60.0 / bpm : (60.0 / 120.0);
}

double PianoRollComponent::getTimelineBeatSeconds() const
{
  const int denominator = normalizeTimelineBeatDenominator(timelineBeatDenominator);
  return getTimelineQuarterNoteSeconds() * (4.0 / static_cast<double>(denominator));
}

double PianoRollComponent::getTimelineBarSeconds() const
{
  const int numerator = juce::jmax(1, timelineBeatNumerator);
  return getTimelineBeatSeconds() * static_cast<double>(numerator);
}

double PianoRollComponent::getTimelineGridSeconds() const
{
  const double quarterNotes = gridDivisionToQuarterNotes(timelineGridDivision);
  return getTimelineQuarterNoteSeconds() * quarterNotes;
}

bool PianoRollComponent::shouldSnapCycleToGrid() const
{
  return timelineDisplayMode == TimelineDisplayMode::Beats &&
         timelineSnapCycle &&
         getTimelineGridSeconds() > 1.0e-6;
}

double PianoRollComponent::snapTimeToTimelineGrid(double timeSeconds) const
{
  if (!shouldSnapCycleToGrid())
    return std::max(getTimelineStart(), timeSeconds);

  const double interval = getTimelineGridSeconds();
  const double snapped =
      coordMapper->snapProjectTimeToTimelineGrid(timeSeconds, interval);
  return std::max(getTimelineStart(), snapped);
}

bool PianoRollComponent::isCanvasPoint(const juce::MouseEvent &e) const
{
  return e.x >= pianoKeysWidth && e.y >= headerHeight &&
         e.x < pianoKeysWidth + getVisibleContentWidth() &&
         e.y < headerHeight + getVisibleContentHeight();
}

bool PianoRollComponent::isModifierZoomDrag(const juce::MouseEvent &e) const
{
  // Zoom Tool: Cmd + Alt (Ctrl + Alt on Windows).
#if JUCE_MAC
  return e.mods.isCommandDown() && e.mods.isAltDown();
#else
  return e.mods.isCtrlDown() && e.mods.isAltDown();
#endif
}

void PianoRollComponent::applyModifierZoomDrag(const juce::MouseEvent &e)
{
  const float deltaX = e.position.x - modifierZoomLastPosition.x;
  const float deltaY = modifierZoomLastPosition.y - e.position.y;
  modifierZoomLastPosition = e.position;

  if (std::abs(deltaX) < 0.01f && std::abs(deltaY) < 0.01f)
    return;

  const float mouseX = modifierZoomAnchor.x - static_cast<float>(pianoKeysWidth);
  const float mouseY = modifierZoomAnchor.y - static_cast<float>(headerHeight);

  if (std::abs(deltaX) >= 0.01f)
  {
    const float zoomFactorX = std::pow(1.0065f, deltaX);
    const double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));
    const int visibleWidth = getVisibleContentWidth();
    const double totalTime = getTimelineLength();
    const float minPpsX =
        (visibleWidth > 0 && totalTime > 0.0)
            ? std::max(MIN_PIXELS_PER_SECOND,
                       static_cast<float>(visibleWidth / totalTime) * ZOOM_OUT_BEYOND_FIT)
            : MIN_PIXELS_PER_SECOND;
    const float newPpsX = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND,
                                       pixelsPerSecond * zoomFactorX);

    pixelsPerSecond = newPpsX;
    coordMapper->setPixelsPerSecond(newPpsX);

    const float newMouseX = static_cast<float>(timeAtMouse * newPpsX);
    scrollX = clampScrollX(static_cast<double>(newMouseX - mouseX));
    coordMapper->setScrollX(scrollX);
  }

  if (std::abs(deltaY) >= 0.01f)
  {
    const float zoomFactorY = std::pow(1.0065f, deltaY);
    setPixelsPerSemitone(pixelsPerSemitone * zoomFactorY, mouseY);
  }

  updateScrollBars();
  repaint();

  if (std::abs(deltaX) >= 0.01f && onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

bool PianoRollComponent::isModifierPanDrag(const juce::MouseEvent &e) const
{
  // Scroll Tool (the hand): Cmd + Shift (Ctrl + Shift on Windows).
  return (e.mods.isCommandDown() || e.mods.isCtrlDown()) && e.mods.isShiftDown() &&
         !e.mods.isAltDown();
}

void PianoRollComponent::applyModifierPanDrag(const juce::MouseEvent &e)
{
  const float deltaX = e.position.x - modifierPanLastPosition.x;
  const float deltaY = e.position.y - modifierPanLastPosition.y;
  modifierPanLastPosition = e.position;

  if (std::abs(deltaX) < 0.01f && std::abs(deltaY) < 0.01f)
    return;

  const double previousScrollX = scrollX;
  setScrollX(scrollX - static_cast<double>(deltaX));
  setScrollY(scrollY - static_cast<double>(deltaY));

  if (std::abs(scrollX - previousScrollX) >= 0.01 && onScrollChanged)
    onScrollChanged(scrollX);
}

std::vector<juce::Rectangle<float>> PianoRollComponent::trackNoteBounds(const Note& note, const Project& owner) const
{
  std::vector<juce::Rectangle<float>> result;
  if (!regionPreviews) return result;
  for (const auto& region : *regionPreviews) {
    if (region.pinned || region.sourceProject != &owner) continue;
    const double offset = region.projectHostOffsetSeconds - framesToSeconds(region.sourceFirstFrame);
    const double start = framesToSeconds(note.getVisualStartFrame()) + offset;
    const double end = framesToSeconds(note.getVisualEndFrame()) + offset;
    if (end <= region.startSeconds || start >= region.endSeconds) continue;
    const float left = pianoKeysWidth + timeToX(timelineToProject(std::max(start, region.startSeconds))) - scrollX;
    const float right = pianoKeysWidth + timeToX(timelineToProject(std::min(end, region.endSeconds))) - scrollX;
    const float y = headerHeight + coordMapper->midiToY(note.getAdjustedMidiNote()) - scrollY +
                    spreadOffsetFor(region);
    result.emplace_back(left, y, std::max(4.0f, right - left), pixelsPerSemitone);
  }
  return result;
}

void PianoRollComponent::syncTrackPreviewSelection()
{
  if (regionPreviews)
    for (const auto& region : *regionPreviews) {
      if (region.active || region.pinned || !region.project || !region.sourceProject) continue;
      auto target = std::find_if(trackGestureTargets.begin(), trackGestureTargets.end(),
          [&](const auto& item) { return item.project == region.sourceProject; });
      if (target == trackGestureTargets.end() || (target->isValid && !target->isValid())) continue;
      for (auto& copy : region.project->getNotes())
        for (const auto& note : target->project->getNotes())
          if (note.getStartFrame() == copy.getStartFrame() + region.sourceFirstFrame &&
              note.getEndFrame() == copy.getEndFrame() + region.sourceFirstFrame) {
            copy.setSelected(note.isSelected());
            break;
          }
    }
  inactiveLayer = {};
  repaint();
}

bool PianoRollComponent::beginTrackGesture(const juce::MouseEvent& e)
{
  if (!multiTrackEditing() || editMode != EditMode::Select || !getTrackEditTargets ||
      !e.mods.isLeftButtonDown() || !isCanvasPoint(e)) return false;
  trackGestureTargets = getTrackEditTargets();
  if (trackGestureTargets.empty()) return false;
  const float worldX = e.x - pianoKeysWidth + scrollX;
  const float worldY = e.y - headerHeight + scrollY;
  if ((hoveredNote && pitchToolHandles && pitchToolHandles->containsLayoutPoint(worldX, worldY)) ||
      findPreviewButtonNoteAt(worldX, worldY)) return false;
  Note* hit = nullptr;
  Project* hitOwner = nullptr;
  std::vector<Project*> selectedOwners;
  trackGestureNotes.clear();
  for (auto& target : trackGestureTargets) {
    for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames)) {
      for (const auto& bounds : trackNoteBounds(*note, *target.project))
        if (bounds.contains(e.position)) { hit = note; hitOwner = target.project; }
      if (coEditInClipMode && hitOwner != nullptr) affiliationOwner = hitOwner;
      if (note->isSelected()) {
        trackGestureNotes.push_back({note, target.project, note->getMidiNote(), note->getMidiNote()});
        if (std::find(selectedOwners.begin(), selectedOwners.end(), target.project) == selectedOwners.end())
          selectedOwners.push_back(target.project);
      }
    }
  }
  const bool additive = e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isShiftDown();
  if (hit && additive) {
    hit->setSelected(!hit->isSelected());
    syncTrackPreviewSelection();
    trackGestureNotes.clear();
  } else if (hit && hit->isSelected() && selectedOwners.size() > 1) {
    std::vector<Note*> activeNotes;
    for (const auto& item : trackGestureNotes)
      if (item.owner == project) activeNotes.push_back(item.note);
    beginMacroCurvePreview(activeNotes);
    beginInactiveMacroPreview(trackGestureTargets);
  } else if (!hit) {
    trackBoxSelecting = true;
    trackBoxInitialSelection.clear();
    if (additive)
      for (const auto& item : trackGestureNotes) trackBoxInitialSelection.push_back(item.note);
    if (!additive)
      for (auto& target : trackGestureTargets)
        for (auto& note : target.project->getNotes()) note.setSelected(false);
    trackGestureNotes.clear();
    syncTrackPreviewSelection();
  } else if (coEditInClipMode && hitOwner != project) {
    // A note of another track open for editing (hosts without ARA): select
    // it, as a click on this track's own note would, and drag the whole
    // selection across tracks straight away.
    if (!hit->isSelected())
      for (auto& target : trackGestureTargets)
        for (auto& note : target.project->getNotes()) note.setSelected(false);
    hit->setSelected(true);
    trackGestureNotes.clear();
    for (auto& target : trackGestureTargets)
      for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames))
        if (note->isSelected())
          trackGestureNotes.push_back({note, target.project, note->getMidiNote(), note->getMidiNote()});
    syncTrackPreviewSelection();
    std::vector<Note*> activeNotes;
    for (const auto& item : trackGestureNotes)
      if (item.owner == project) activeNotes.push_back(item.note);
    beginMacroCurvePreview(activeNotes);
    beginInactiveMacroPreview(trackGestureTargets);
  } else {
    if (!hit->isSelected())
      for (auto& target : trackGestureTargets)
        for (auto& note : target.project->getNotes()) note.setSelected(false);
    syncTrackPreviewSelection();
    return false;
  }
  trackGestureActive = true;
  trackGestureStart = trackGestureEnd = e.position;
  return true;
}

void PianoRollComponent::updateTrackGesture(const juce::MouseEvent& e)
{
  trackGestureEnd = e.position;
  if (trackBoxSelecting) {
    const auto rect = juce::Rectangle<float>(trackGestureStart, trackGestureEnd);
    for (auto& target : trackGestureTargets) {
      if (target.isValid && !target.isValid()) continue;
      for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames)) {
        bool selected = std::find(trackBoxInitialSelection.begin(), trackBoxInitialSelection.end(), note)
                        != trackBoxInitialSelection.end();
        for (const auto& bounds : trackNoteBounds(*note, *target.project))
          selected |= rect.intersects(bounds);
        note->setSelected(selected);
      }
    }
    syncTrackPreviewSelection();
  } else {
    float delta = (trackGestureStart.y - e.position.y) / pixelsPerSemitone;
    if (snapToSemitoneDrag != e.mods.isAltDown()) delta = std::round(delta);
    for (auto& item : trackGestureNotes) {
      auto target = std::find_if(trackGestureTargets.begin(), trackGestureTargets.end(),
          [&](const auto& target) { return target.project == item.owner; });
      if (target != trackGestureTargets.end() && (!target->isValid || target->isValid()))
        item.note->setMidiNote(item.midi + delta);
    }
    repaint();
  }
}

void PianoRollComponent::finishTrackGesture()
{
  trackGestureActive = false;
  trackBoxSelecting = false;
  endMacroCurvePreview();
  auto notes = trackGestureNotes;
  auto targets = trackGestureTargets;
  notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const auto& item) {
    auto target = std::find_if(targets.begin(), targets.end(), [&](const auto& target)
        { return target.project == item.owner; });
    return target == targets.end() || (target->isValid && !target->isValid());
  }), notes.end());
  for (auto& item : notes) item.finalMidi = item.note->getMidiNote();
  if (std::none_of(notes.begin(), notes.end(), [](const auto& item) {
      return std::abs(item.finalMidi - item.midi) > 0.0001f; })) { repaint(); return; }
  auto safe = juce::Component::SafePointer<PianoRollComponent>(this);
  auto apply = [safe, notes, targets](bool redo) {
    if (!safe) return;
    for (const auto& target : targets) {
      if (target.isValid && !target.isValid()) continue;
      int first = std::numeric_limits<int>::max(), last = -1;
      for (const auto& item : notes) {
        if (item.owner != target.project) continue;
        item.note->setMidiNote(redo ? item.finalMidi : item.midi);
        item.note->markDirty(); item.note->markSynthDirty();
        first = std::min(first, item.note->getStartFrame());
        last = std::max(last, item.note->getEndFrame());
      }
      if (last < first) continue;
      PitchCurveProcessor::rebuildBaseFromNotes(*target.project);
      target.project->markNoteEditDirtyRange(first, last);
      target.project->setModified(true);
      if (target.project == safe->project) {
        if (safe->onPitchEdited) safe->onPitchEdited();
        if (safe->onPitchEditFinished) safe->onPitchEditFinished();
      } else if (target.onChanged) target.onChanged(true);
    }
    safe->invalidateBasePitchCache(); safe->repaint();
  };
  class TrackDragAction final : public UndoableAction {
  public:
    explicit TrackDragAction(std::function<void(bool)> apply) : apply(std::move(apply)) {}
    void undo() override { apply(false); }
    void redo() override { apply(true); }
    juce::String getName() const override { return "Drag Track Notes"; }
    bool ownsProjectContext() const override { return true; }
    bool requiresAudioResynthesis() const override { return false; }
  private: std::function<void(bool)> apply;
  };
  apply(true);
  if (undoManager) undoManager->addAction(std::make_unique<TrackDragAction>(apply));
}

void PianoRollComponent::mouseDown(const juce::MouseEvent &e)
{
  swallowGestureUntilMouseUp = false;

  if (!project)
  {
    // The active region may still be analysing; the other regions stay
    // clickable so the user can move on to one that is ready.
    if (e.mods.isLeftButtonDown() && isCanvasPoint(e) && onInactiveRegionClicked)
    {
      if (const auto *region = findTrackRegionAtPointer(e.position))
      {
        const juce::String key = region->key;
        swallowGestureUntilMouseUp = true;
        onInactiveRegionClicked(key);
        repaint();
      }
    }
    return;
  }

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  if (e.mods.isPopupMenu() && e.y < timelineHeight && e.x >= pianoKeysWidth)
  {
    showReplaceRangesMenu(e);
    return;
  }

  if (replaceRangesActive() && e.mods.isLeftButtonDown() && e.y < timelineHeight &&
      e.x >= pianoKeysWidth)
  {
    bool isStart = false;
    const int edge = ReplaceRanges::edgeAt(project->getReplaceRanges(), xToTime(adjustedX),
                                           xToTime(5.0f), isStart);
    if (edge >= 0)
    {
      replaceDragIndex = edge;
      replaceDragStart = isStart;
      return;
    }
  }

  if (e.mods.isPopupMenu() && isCanvasPoint(e))
  {
    juce::PopupMenu menu;
    menu.setLookAndFeel(&canvasPopupMenu::getLookAndFeel());
    auto choice = std::make_shared<int>(0);
    menu.addCustomItem(1, std::make_unique<canvasPopupMenu::Content>(
        choice, undoManager && undoManager->canUndo() && bool(onUndoRequested),
        undoManager && undoManager->canRedo() && bool(onRedoRequested)));
    juce::Component::SafePointer<PianoRollComponent> safeThis(this);
    const auto clickScreen = e.getScreenPosition();
    const double clickFrame =
        xToTime(adjustedX) * static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
    Note *clickedNote = findNoteAt(adjustedX, adjustedY);
    menu.showMenuAsync(juce::PopupMenu::Options()
                          .withTargetScreenArea(canvasPopupMenu::Content::getTargetArea(
                              e.getScreenPosition()))
                          .withDeletionCheck(*this),
                      [safeThis, choice, clickScreen, clickFrame, clickedNote](int result)
    {
      if (safeThis == nullptr || !safeThis->project || result == 0)
        return;
      if (*choice == 1)
      {
        safeThis->rememberSelection();
        if (safeThis->ownTrackEditable)
          safeThis->project->selectAllNotes();
        safeThis->updatePitchToolHandlesFromSelection();
        safeThis->updatePreviewButtonBounds();
        safeThis->repaint();
      }
      else if (*choice == 6 && safeThis->onUndoRequested)
        safeThis->onUndoRequested();
      else if (*choice == 7 && safeThis->onRedoRequested)
        safeThis->onRedoRequested();
      else if (*choice >= 8 && *choice <= 10 && safeThis->onMacroRequested)
        safeThis->onMacroRequested(*choice - 8);
      else if (*choice == 11)
        safeThis->invertSelection();
      else if (*choice == 12)
        safeThis->restoreLastSelection();
      else if (*choice == 13)
        safeThis->selectSpecial(0);
      else if (*choice == 14)
        safeThis->selectSpecial(2);
      else if (*choice == 15 && safeThis->onMacroRequested)
        safeThis->onMacroRequested(3);
      else if (*choice == 16)
        safeThis->showSibilantMenu(clickScreen, clickFrame, clickedNote);
      else if (*choice >= 2 && *choice <= 5)
      {
        const EditMode modes[] = { EditMode::Select, EditMode::Split,
                                   EditMode::PitchDrawing, EditMode::Timing };
        const auto mode = modes[*choice - 2];
        if (safeThis->onEditModeRequested)
          safeThis->onEditModeRequested(mode);
        else
          safeThis->setEditMode(mode);
      }
    });
    return;
  }

  // Middle-mouse scrub: click-and-drag anywhere in the timeline/canvas
  // (ruler or note area) to move the playback cursor, mirroring Reaper's
  // own middle-button scrub. Takes priority over every other gesture so
  // held modifier keys never redirect it into zoom/pan.
  if (e.mods.isMiddleButtonDown() && e.x >= pianoKeysWidth)
  {
    middleButtonScrubActive = true;
    double time = std::max(getTimelineStart(), xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    return;
  }

  if (isCanvasPoint(e) && isModifierZoomDrag(e))
  {
    rememberZoom();
    modifierZoomDragActive = true;
    modifierZoomLastPosition = e.position;
    modifierZoomAnchor = e.position; // zoom around where the drag began
    return;
  }

  if (isCanvasPoint(e) && e.mods.isAltDown() && trackViewMode)
  {
    if (const auto* region = findTrackRegionAtPointer(e.position, true)) {
      const double offset = region->projectHostOffsetSeconds - framesToSeconds(region->sourceFirstFrame);
      const auto* boundWindow = findActiveRegionPreview();
      if (onInactiveRegionClicked && (region->sourceProject != project ||
          !juce::approximatelyEqual(offset, projectToTimeline(0.0)) ||
          !boundWindow || boundWindow->key != region->key)) {
        const auto key = region->key;
        onInactiveRegionClicked(key);
        adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
        adjustedY = e.y - headerHeight + static_cast<float>(scrollY);
      }
    }
  }
  // Alt is left to the tools (no grid while dragging a note).
  if (isCanvasPoint(e) && isModifierPanDrag(e))
  {
    modifierPanDragActive = true;
    modifierPanLastPosition = e.position;
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    return;
  }

  // Handle timeline clicks - seek to position. Then a drag
  // up or down zooms the time around that place, a drag sideways moves the
  // cursor along (the first movement decides).
  if (e.y < timelineHeight && e.x >= pianoKeysWidth)
  {
    double time = std::max(getTimelineStart(), xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    rulerDragActive = true;
    rulerDragAxis = 0;
    rulerDragStart = e.position;
    rulerDragStartPps = pixelsPerSecond;
    rulerDragAnchorTime = xToTime(adjustedX);
    return;
  }

  if (e.mods.isPopupMenu() && e.x < pianoKeysWidth && e.y >= headerHeight)
  {
    juce::PopupMenu menu;
    menu.addSectionHeader(TR("background.title"));
    menu.addItem(1, TR("background.keyboard"), true, !pitchLinesBackground);
    menu.addItem(2, TR("background.pitch_lines"), true, pitchLinesBackground);
    juce::Component::SafePointer<PianoRollComponent> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options()
                           .withTargetScreenArea({e.getScreenX(), e.getScreenY(), 1, 1})
                           .withDeletionCheck(*this),
                       [safeThis](int result) {
                         if (safeThis != nullptr && result > 0)
                           safeThis->setPitchLinesBackground(result == 2);
                       });
    return;
  }

  // Clicking a note name auditions its labelled pitch without changing the
  // current edit or transport state.
  if (e.x < pianoKeysWidth && e.y >= headerHeight &&
      e.y < getHeight() - (showHorizontalScrollBar ? 8 : 0))
  {
    const float referenceOffset =
        ScaleUtils::getReferenceOffsetSemitones(pitchReferenceHz);
    // yToMidi() returns the note at the row's top edge and descends across
    // the row. Ceiling keeps every pixel in this rendered row mapped to its
    // label; rounding would switch to the adjacent note halfway down.
    const int midiNote = static_cast<int>(
        std::ceil(yToMidi(adjustedY) - referenceOffset));
    if (midiNote >= MIN_MIDI_NOTE && midiNote <= MAX_MIDI_NOTE &&
        onPianoKeyAudition)
    {
      pianoKeyAuditionMouseDown = true;
      onPianoKeyAudition(midiNote);
    }
    // A key of the pitch ruler selects every note of its pitch.
    if (midiNote >= MIN_MIDI_NOTE && midiNote <= MAX_MIDI_NOTE && e.mods.isLeftButtonDown())
      selectNotesOfPitch(midiNote, e.getNumberOfClicks() >= 2,
                         e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown());
    return;
  }

  // Handle loop timeline drag (always active, priority over edit modes)
  if (loopDragHandler_->mouseDown(e, adjustedX, adjustedY))
    return;

  // Ignore clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  if (beginTrackGesture(e)) return;
  // The own track is switched off: clicks reach only the tracks being edited.
  if (!ownTrackEditable && isCanvasPoint(e)) return;

  // Bind the project owning this pointer hit internally, then deliver the
  // same event to its tool. Routing does not privilege the bound region.
  if (e.mods.isLeftButtonDown() && onInactiveRegionClicked && isCanvasPoint(e))
  {
    const auto* region = findTrackRegionAtPointer(e.position);
    const bool handleHit = hoveredNote && pitchToolHandles &&
        pitchToolHandles->containsLayoutPoint(adjustedX, adjustedY);
    const double regionOffset = region ? region->projectHostOffsetSeconds -
        framesToSeconds(region->sourceFirstFrame) : projectToTimeline(0.0);
    const auto* boundWindow = findActiveRegionPreview();
    if (region && !handleHit && (region->sourceProject != project ||
        !juce::approximatelyEqual(regionOffset, projectToTimeline(0.0)) ||
        !boundWindow || boundWindow->key != region->key))
    {
      const juce::String key = region->key; // previews may be replaced below
      const auto* requestedProject = region->sourceProject;
      const double previousHostScroll = getHostScrollSeconds();
      const double previousScrollY = scrollY;

      onInactiveRegionClicked(key);
      repaint();

      // The new region may belong to another modification (new project) or be
      // a sibling of this one (same project, new window and offset). Either
      // way the host view must not have moved for the click to carry on.
      const bool viewStill =
          std::abs(getHostScrollSeconds() - previousHostScroll) * pixelsPerSecond < 0.5 &&
          juce::approximatelyEqual(scrollY, previousScrollY);
      const bool continuesIntoRegion = project != nullptr && project == requestedProject &&
          viewStill && juce::approximatelyEqual(projectToTimeline(0.0), regionOffset);
      if (!continuesIntoRegion)
      {
        swallowGestureUntilMouseUp = true;
        return;
      }

      // Same screen point, now in the new region's time.
      adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
      adjustedY = e.y - headerHeight + static_cast<float>(scrollY);
      setHoveredNote(nullptr);
    }
    else if (trackViewMode && project != nullptr && !region && !handleHit)
    {
      // Outside every region in Track mode: the active take's audio there is
      // hidden, so nothing may be edited.
      swallowGestureUntilMouseUp = true;
      return;
    }
  }

  if (editMode == EditMode::Select)
  {
    if (auto *note = findPreviewButtonNoteAt(adjustedX, adjustedY))
    {
      triggerPreviewForNote(*note);
      return;
    }
  }

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseDown(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::mouseDrag(const juce::MouseEvent &e)
{
  if (trackGestureActive) { updateTrackGesture(e); return; }
  if (e.mods.isPopupMenu())
    return;
  if (swallowGestureUntilMouseUp)
    return;
  if (middleButtonScrubActive)
  {
    float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    double time = std::max(getTimelineStart(), xToTime(adjustedX));
    setCursorTime(time);
    if (onSeek)
      onSeek(time);
    return;
  }

  if (rulerDragActive)
  {
    const auto moved = e.position - rulerDragStart;
    if (rulerDragAxis == 0 && (std::abs(moved.x) > 3.0f || std::abs(moved.y) > 3.0f))
    {
      rulerDragAxis = std::abs(moved.y) > std::abs(moved.x) ? 2 : 1;
      if (rulerDragAxis == 2)
        rememberZoom();
    }
    if (rulerDragAxis == 2)
    {
      // Down zooms in, up zooms out, around the place grabbed.
      const int visibleWidth = getVisibleContentWidth();
      const double totalTime = getTimelineLength();
      const float minPpsX =
          (visibleWidth > 0 && totalTime > 0.0)
              ? std::max(MIN_PIXELS_PER_SECOND,
                         static_cast<float>(visibleWidth / totalTime) * ZOOM_OUT_BEYOND_FIT)
              : MIN_PIXELS_PER_SECOND;
      const float newPps = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND,
                                        rulerDragStartPps * std::pow(1.012f, moved.y));
      const float anchorX = rulerDragStart.x - static_cast<float>(pianoKeysWidth);
      pixelsPerSecond = newPps;
      coordMapper->setPixelsPerSecond(newPps);
      scrollX = clampScrollX(rulerDragAnchorTime * newPps - anchorX);
      coordMapper->setScrollX(scrollX);
      updateScrollBars();
      repaint();
      if (onZoomChanged)
        onZoomChanged(pixelsPerSecond);
    }
    else if (rulerDragAxis == 1)
    {
      const float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
      const double time = std::max(getTimelineStart(), xToTime(adjustedX));
      setCursorTime(time);
      if (onSeek)
        onSeek(time);
    }
    return;
  }

  if (replaceDragIndex >= 0 && project)
  {
    auto ranges = project->getReplaceRanges();
    if (replaceDragIndex < static_cast<int>(ranges.size()))
    {
      const double t = std::max(0.0, xToTime(e.x - pianoKeysWidth + static_cast<float>(scrollX)));
      auto &r = ranges[static_cast<size_t>(replaceDragIndex)];
      if (replaceDragStart)
        r.first = std::min(t, r.second - ReplaceRanges::kMinLengthSeconds);
      else
        r.second = std::max(t, r.first + ReplaceRanges::kMinLengthSeconds);
      project->setReplaceRanges(std::move(ranges));
      repaint();
    }
    return;
  }

  if (modifierZoomDragActive)
  {
    applyModifierZoomDrag(e);
    return;
  }

  if (modifierPanDragActive)
  {
    applyModifierPanDrag(e);
    return;
  }

  // Throttle repaints during drag to ~60fps max
  juce::int64 now = juce::Time::getMillisecondCounter();
  bool shouldRepaint = (now - lastDragRepaintTime) >= minDragRepaintInterval;
  juce::ignoreUnused(shouldRepaint);

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_ && currentHandler_->mouseDrag(e, adjustedX, adjustedY))
  {
    updatePreviewButtonBounds();
    if (shouldRepaint)
    {
      repaint();
      lastDragRepaintTime = now;
    }
  }
}

void PianoRollComponent::mouseUp(const juce::MouseEvent &e)
{
  if (affiliationOwner != nullptr)
  {
    affiliationOwner = nullptr;
    repaint();
  }
  if (trackGestureActive) { finishTrackGesture(); return; }
  if (e.mods.isPopupMenu())
    return;
  if (swallowGestureUntilMouseUp)
  {
    swallowGestureUntilMouseUp = false;
    return;
  }
  if (middleButtonScrubActive)
  {
    middleButtonScrubActive = false;
    return;
  }
  if (rulerDragActive)
  {
    rulerDragActive = false;
    return;
  }
  if (replaceDragIndex >= 0)
  {
    replaceDragIndex = -1;
    if (project)
      commitReplaceRanges(project->getReplaceRanges());
    return;
  }

  if (pianoKeyAuditionMouseDown)
  {
    pianoKeyAuditionMouseDown = false;
    if (onPianoKeyAuditionFinished)
      onPianoKeyAuditionFinished();
    return;
  }

  if (modifierZoomDragActive)
  {
    modifierZoomDragActive = false;
    return;
  }

  if (modifierPanDragActive)
  {
    modifierPanDragActive = false;
    if (!e.mods.isAltDown())
      updateMouseCursorForEditMode();
    return;
  }

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // Loop drag has priority (always active)
  if (loopDragHandler_->mouseUp(e, adjustedX, adjustedY))
    return;

  // Delegate to current mode handler
  if (currentHandler_)
  {
    currentHandler_->mouseUp(e, adjustedX, adjustedY);
    updatePreviewButtonBounds();
  }
}

void PianoRollComponent::mouseMove(const juce::MouseEvent &e)
{
  if (isCanvasPoint(e) && !isModifierZoomDrag(e)) prepareTrackPointer(e.position, true);
  const bool showZoomCursor = isCanvasPoint(e) && isModifierZoomDrag(e);
  const float adjustedX =
      e.x - pianoKeysWidth + static_cast<float>(scrollX);
  const float adjustedY =
      e.y - headerHeight + static_cast<float>(scrollY);

  if (replaceRangesActive() && e.y < timelineHeight && e.x >= pianoKeysWidth)
  {
    bool isStart = false;
    if (ReplaceRanges::edgeAt(project->getReplaceRanges(), xToTime(adjustedX),
                              xToTime(5.0f), isStart) >= 0)
    {
      setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
      return;
    }
  }

  Note *noteAtPointer = nullptr;
  if (e.y >= headerHeight && e.x >= pianoKeysWidth)
    noteAtPointer = findNoteAt(adjustedX, adjustedY);

  const bool altControlsNoteSnap =
      editMode == EditMode::Select && noteAtPointer != nullptr;
  if (!showZoomCursor && isCanvasPoint(e) && e.mods.isAltDown() &&
      !altControlsNoteSnap)
  {
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    return;
  }

  // Hide the bound take outside the track's playback windows, rather than
  // treating any one region as the interaction boundary.
  const bool overHoverControls = hoveredNote &&
      ((pitchToolHandles && pitchToolHandles->containsLayoutPoint(adjustedX, adjustedY)) ||
       findPreviewButtonNoteAt(adjustedX, adjustedY));
  if (!showZoomCursor && isCanvasPoint(e) && trackViewMode && !overHoverControls &&
      findTrackRegionAtPointer(e.position) == nullptr)
  {
    setHoveredNote(nullptr);
    loopDragHandler_->mouseMove(e, adjustedX, adjustedY);
    setMouseCursor(loopDragHandler_->isHoveringHandle()
                       ? juce::MouseCursor::LeftRightResizeCursor
                       : juce::MouseCursor::NormalCursor);
    return;
  }

  // Handle rows (including their gaps) keep their owner. Everywhere else,
  // a note directly under the pointer takes priority over the old layout.
  const bool overCurrentHandleArea = hoveredNote && pitchToolHandles &&
      pitchToolHandles->containsLayoutPoint(adjustedX, adjustedY);
  Note *noteUnderMouse = overCurrentHandleArea ? hoveredNote : noteAtPointer;
  if (!noteUnderMouse && hoveredNote)
  {
    auto hoverLayoutBounds = getPreviewHoverBounds(*hoveredNote);
    if (pitchToolHandles && !pitchToolHandles->isEmpty())
      hoverLayoutBounds = hoverLayoutBounds.getUnion(
          pitchToolHandles->getLayoutBounds());

    // Keep controls reachable across empty canvas without masking other notes.
    if (hoverLayoutBounds.expanded(4.0f).contains(adjustedX, adjustedY))
      noteUnderMouse = hoveredNote;
  }
  if (!noteUnderMouse)
    noteUnderMouse = findPreviewButtonNoteAt(adjustedX, adjustedY);
  if (!noteUnderMouse && hoveredNote && pitchToolHandles &&
      !pitchToolHandles->isEmpty() &&
      pitchToolHandles->hitTest(adjustedX, adjustedY) >= 0)
  {
    // Keep the note's hover controls visible while moving onto a handle.
    noteUnderMouse = hoveredNote;
  }
  if (!noteUnderMouse && hoveredNote &&
      getPreviewHoverBounds(*hoveredNote).contains(adjustedX, adjustedY))
  {
    noteUnderMouse = hoveredNote;
  }
  // Pitch Drawing mode uses its own point hover treatment. Split mode must wait until
  // its handler has resolved merge-boundary hover before changing hoveredNote.
  // Otherwise the preview/reset child components are shown and hidden again
  // during the same mouse event, which can stall the Windows mouse-message
  // loop while a custom cursor is active over a boundary.
  if (editMode != EditMode::Split)
    setHoveredNote(editMode == EditMode::PitchDrawing ? nullptr : noteUnderMouse);

  // Loop timeline cursor handling (always active)
  loopDragHandler_->mouseMove(e, adjustedX, adjustedY);

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseMove(e, adjustedX, adjustedY);

  // A merge boundary draws its own two-note hover treatment. Resolve the
  // split-mode hover once, after boundary hit-testing, so child controls never
  // enter a transient visible state over the boundary.
  if (editMode == EditMode::Split)
    setHoveredNote(splitHandler_ && splitHandler_->isHoveringMergeBoundary()
                       ? nullptr
                       : noteUnderMouse);

  // Pitch tool handle hover (uses raw event coordinates, not world-adjusted)
  if (editMode == EditMode::Select && pitchToolHandles && !pitchToolHandles->isEmpty() &&
      e.y >= headerHeight && e.x >= pianoKeysWidth)
  {
    int hitIndex = pitchToolHandles->hitTest(adjustedX, adjustedY);
    if (hitIndex != hoveredPitchToolHandle)
    {
      hoveredPitchToolHandle = hitIndex;
      pitchToolHandles->setHoveredHandleIndex(hitIndex);
      repaint();
    }
  }
  else if (hoveredPitchToolHandle != -1)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
    repaint();
  }

  // Cycle handles take priority over every edit-mode cursor. Once the pointer
  // leaves a handle, the active tool immediately restores its canvas cursor.
  if (loopDragHandler_ && loopDragHandler_->isHoveringHandle())
  {
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
  }
  else if (showZoomCursor)
  {
    setMouseCursor(zoomMouseCursor);
  }
  else if (editMode == EditMode::Select)
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Split)
  {
    if (!isCanvasPoint(e))
      setMouseCursor(juce::MouseCursor::NormalCursor);
    else
    {
      const bool hoveringMergeBoundary =
          splitHandler_ && splitHandler_->isHoveringMergeBoundary();
      const bool hoveringNote =
          splitHandler_ && splitHandler_->getSplitGuideNote() != nullptr;
      setMouseCursor(hoveringMergeBoundary
                         ? mergeMouseCursor
                         : (hoveringNote ? splitMouseCursor
                                         : juce::MouseCursor::NormalCursor));
    }
  }
  else if (editMode == EditMode::PitchDrawing)
  {
    if (!isCanvasPoint(e) || !pitchDrawingHandler_)
      setMouseCursor(juce::MouseCursor::NormalCursor);
    else if (pitchDrawingHandler_->isHoveringAnchor())
      setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    else
      setMouseCursor(pitchDrawingHandler_->isPointerOverPitchRegion()
                         ? juce::MouseCursor::CrosshairCursor
                         : juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Timing)
  {
    // TimingHandler owns edge-specific cursor arbitration.
  }
}

void PianoRollComponent::mouseEnter(const juce::MouseEvent &e)
{
  // The pointer may enter from a toolbar child whose pointing-hand cursor was
  // active when the edit mode changed. Re-run canvas cursor arbitration on
  // entry instead of waiting for a later move event.
  mouseMove(e);
}

void PianoRollComponent::modifierKeysChanged(
    const juce::ModifierKeys &modifiers)
{
  const auto mousePosition = getMouseXYRelative();
  const bool mouseOverCanvas =
      mousePosition.x >= pianoKeysWidth && mousePosition.y >= headerHeight &&
      mousePosition.x < pianoKeysWidth + getVisibleContentWidth() &&
      mousePosition.y < headerHeight + getVisibleContentHeight();
  const float worldX = mousePosition.x - pianoKeysWidth +
                       static_cast<float>(scrollX);
  const float worldY = mousePosition.y - headerHeight +
                       static_cast<float>(scrollY);
  const bool pointerOverNote =
      mouseOverCanvas && editMode == EditMode::Select &&
      findNoteAt(worldX, worldY) != nullptr;

#if JUCE_MAC
  const bool zoomModifierDown = modifiers.isCommandDown();
#else
  const bool zoomModifierDown = modifiers.isCtrlDown();
#endif

  if (zoomModifierDown && mouseOverCanvas)
    setMouseCursor(zoomMouseCursor);
  else if (modifierPanDragActive ||
           (modifiers.isAltDown() && mouseOverCanvas && !pointerOverNote))
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
  else
    updateMouseCursorForEditMode();
}

void PianoRollComponent::mouseExit(const juce::MouseEvent &)
{
  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_)
    pitchDrawingHandler_->clearHover();

  if (previewButton.isMouseOverOrDragging() ||
      resetButton.isMouseOverOrDragging())
    return;

  setHoveredNote(nullptr);
}

juce::String PianoRollComponent::getTooltip()
{
  if (!pitchToolHandles || pitchToolHandles->isEmpty())
    return {};

  const auto mousePosition = getMouseXYRelative();
  const float adjustedX = mousePosition.x - pianoKeysWidth +
                          static_cast<float>(scrollX);
  const float adjustedY = mousePosition.y - headerHeight +
                          static_cast<float>(scrollY);
  const int handleIndex = pitchToolHandles->hitTest(adjustedX, adjustedY);
  if (handleIndex < 0)
    return {};

  switch (pitchToolHandles->getHandle(handleIndex).type)
  {
    case PitchToolHandles::HandleType::TiltLeft: return TR("handle.tilt_left");
    case PitchToolHandles::HandleType::Vibrato: return TR("handle.vibrato");
    case PitchToolHandles::HandleType::PitchDrift: return TR("handle.pitch_drift");
    case PitchToolHandles::HandleType::Amplitude: return TR("handle.amplitude");
    case PitchToolHandles::HandleType::Formant: return TR("handle.formant");
    case PitchToolHandles::HandleType::TiltRight: return TR("handle.tilt_right");
    default: return {};
  }
}

void PianoRollComponent::mouseDoubleClick(const juce::MouseEvent &e)
{
  // JUCE dispatches double-clicks after mouseUp clears the scrub state.
  // Keep repeated middle clicks from triggering transport or edit actions.
  if (e.mods.isMiddleButtonDown() || e.mods.isPopupMenu())
    return;

  if (!project)
    return;

  // With the Zoom Tool keys (Ctrl + Alt) a double-click on the
  // background goes back to the previous zoom; with the Scroll Tool keys
  // (Ctrl + Shift) a double-click on a note zooms to it, or to the selection.
  if (isCanvasPoint(e) && (isModifierZoomDrag(e) || isModifierPanDrag(e)))
  {
    const float worldX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    const float worldY = e.y - headerHeight + static_cast<float>(scrollY);
    auto *hit = findNoteAt(worldX, worldY);
    if (isModifierPanDrag(e) && hit != nullptr)
      zoomToNotes(hit);
    else if (isModifierZoomDrag(e) && hit == nullptr)
      restorePreviousZoom();
    return;
  }

  // Double-clicking a reference (grey) note makes its track the edited one,
  // as in other pitch editors. This track's own notes are in front and win.
  if (onReferenceNoteDoubleClicked && regionPreviews && isCanvasPoint(e))
  {
    const float worldX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
    const float worldY = e.y - headerHeight + static_cast<float>(scrollY);
    if (findNoteAt(worldX, worldY) == nullptr)
      if (const auto key = pinnedNoteKeyAt(worldX, worldY); key.isNotEmpty())
      {
        onReferenceNoteDoubleClicked(key);
        return;
      }
  }

  if (e.y >= timelineHeight && e.y < headerHeight && e.x >= pianoKeysWidth)
  {
    auto loopRange = project->getLoopRange();
    if (loopRange.endSeconds > loopRange.startSeconds)
    {
      project->setLoopEnabled(false);
      if (onLoopRangeChanged)
        onLoopRangeChanged(project->getLoopRange());
      repaint();
    }
    return;
  }

  // Ignore double-clicks outside main area
  if (e.y < headerHeight || e.x < pianoKeysWidth)
    return;

  float adjustedX = e.x - pianoKeysWidth + static_cast<float>(scrollX);
  float adjustedY = e.y - headerHeight + static_cast<float>(scrollY);

  // An empty-space double click works uniformly across track regions.
  const auto* pointerRegion = findTrackRegionAtPointer(e.position);
  if (pointerRegion && pointerRegion->sourceProject != project) {
    bool overNote = false;
    if (getTrackEditTargets)
      for (const auto& target : getTrackEditTargets())
        for (auto* note : collectPitchMacroNotes(*target.project, true, target.frames))
          for (const auto& bounds : trackNoteBounds(*note, *target.project))
            overNote |= bounds.contains(e.position);
    if (!overNote && onCanvasEmptyDoubleClick)
      onCanvasEmptyDoubleClick(std::max(getTimelineStart(), xToTime(adjustedX)));
    return;
  }

  // Anchors may be placed vertically outside their owning note rectangle, so
  // give Pitch Drawing mode first chance to remove one before the empty-canvas
  // double-click transport behavior runs.
  if (editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
      pitchDrawingHandler_->removeAnchorAt(adjustedX, adjustedY))
    return;

  // Keep note-specific double-click interactions with their edit handlers.
  // Everywhere else on the canvas, a double-click controls transport at the
  // clicked time rather than changing the current edit selection.
  if (!findNoteAt(adjustedX, adjustedY))
  {
    if (onCanvasEmptyDoubleClick)
      onCanvasEmptyDoubleClick(std::max(getTimelineStart(), xToTime(adjustedX)));
    return;
  }

  // Delegate to current mode handler
  if (currentHandler_)
    currentHandler_->mouseDoubleClick(e, adjustedX, adjustedY);
}

void PianoRollComponent::mouseWheelMove(const juce::MouseEvent &e,
                                        const juce::MouseWheelDetails &wheel)
{
  float scrollMultiplier = wheel.isSmooth ? 200.0f : 80.0f;
  const int visibleHeight = getVisibleContentHeight();
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineDuration();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime) * ZOOM_OUT_BEYOND_FIT)
          : MIN_PIXELS_PER_SECOND;

  bool isOverPianoKeys = e.x < pianoKeysWidth;
  bool isOverTimeline = e.y < headerHeight;

  // Zoom around the pointer, as in FL Studio's piano roll.
  const auto zoomHorizontally = [&](float factor)
  {
    const float mouseX = static_cast<float>(e.x - pianoKeysWidth);
    const double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));
    const float newPps = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, pixelsPerSecond * factor);
    pixelsPerSecond = newPps;
    coordMapper->setPixelsPerSecond(newPps);
    scrollX = clampScrollX(timeAtMouse * newPps - mouseX);
    coordMapper->setScrollX(scrollX);
    updateScrollBars();
    repaint();
    if (onZoomChanged)
      onZoomChanged(pixelsPerSecond);
  };
  const auto zoomVertically = [&](float factor)
  {
    const float mouseY = static_cast<float>(e.y - headerHeight);
    if (factor < 1.0f)
    {
      // Elastic resistance near the smallest zoom.
      const float range = minPps * 0.35f;
      const float t = range > 0.0f ? juce::jlimit(0.0f, 1.0f, (pixelsPerSemitone - minPps) / range) : 0.0f;
      factor = 1.0f + (factor - 1.0f) * t;
    }
    setPixelsPerSemitone(juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, pixelsPerSemitone * factor), mouseY);
  };
  // One wheel notch is about 0.11 on Windows; trackpads send smaller steps.
  const float wheelAmount = std::abs(wheel.deltaY) > 0.0001f ? wheel.deltaY : wheel.deltaX;
  const float zoomFactor = 1.0f + wheelAmount * 0.3f;
  const bool ctrl = e.mods.isCommandDown() || e.mods.isCtrlDown();
  juce::ignoreUnused(isOverPianoKeys);

  // Ctrl(Cmd) + Alt + wheel zooms time and pitch at once, around
  // the pointer. A Windows trackpad pinch arrives as Ctrl + wheel: the same.
  if (ctrl)
  {
    rememberZoomThrottled();
    zoomHorizontally(zoomFactor);
    zoomVertically(zoomFactor);
    return;
  }
  // Over the time ruler the wheel zooms time.
  if (isOverTimeline)
  {
    rememberZoomThrottled();
    zoomHorizontally(zoomFactor);
    return;
  }

  // The wheel scrolls up / down, Shift + wheel left / right; a
  // trackpad's two-finger swipe moves both ways.
  float alongTime = wheel.deltaX;
  float alongPitch = wheel.deltaY;
  if (e.mods.isShiftDown() && std::abs(alongTime) < 0.001f)
  {
    alongTime = wheel.deltaY;
    alongPitch = 0.0f;
  }

  if (std::abs(alongTime) > 0.001f)
    horizontalScrollBar.setCurrentRangeStart(
        clampScrollX(scrollX - alongTime * scrollMultiplier));
  if (std::abs(alongPitch) > 0.001f)
    verticalScrollBar.setCurrentRangeStart(scrollY - alongPitch * scrollMultiplier);
}

void PianoRollComponent::mouseMagnify(const juce::MouseEvent &e,
                                      float scaleFactor)
{
  // Pinch-to-zoom on trackpad: time and pitch at once, around
  // the pointer.
  rememberZoomThrottled();
  setPixelsPerSemitone(pixelsPerSemitone * scaleFactor, static_cast<float>(e.y - headerHeight));
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineLength();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime) * ZOOM_OUT_BEYOND_FIT)
          : MIN_PIXELS_PER_SECOND;
  float mouseX = static_cast<float>(e.x - pianoKeysWidth);
  double timeAtMouse = xToTime(mouseX + static_cast<float>(scrollX));

  float newPps = pixelsPerSecond * scaleFactor;
  newPps = juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, newPps);

  // Adjust scroll to keep mouse position stable
  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  float newMouseX = static_cast<float>(timeAtMouse * newPps);
  scrollX = clampScrollX(static_cast<double>(newMouseX - mouseX));
  coordMapper->setScrollX(scrollX);

  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  repaint();

  if (onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

void PianoRollComponent::scrollBarMoved(juce::ScrollBar *scrollBar,
                                        double newRangeStart)
{
  if (scrollBar == &horizontalScrollBar)
  {
    scrollX = clampScrollX(newRangeStart);
    coordMapper->setScrollX(scrollX);

    // Notify scroll changed for synchronization
    if (onScrollChanged)
      onScrollChanged(scrollX);
  }
  else if (scrollBar == &verticalScrollBar)
  {
    scrollY = newRangeStart;
    coordMapper->setScrollY(newRangeStart);
  }
  updatePreviewButtonBounds();
  repaint();
}

// The key the editor shows: rows in the key light, the rest dark, like a
// scale highlight in a DAW piano roll. Only an explicit "None" turns it off.
ScaleMode PianoRollComponent::keyModeOf(const Project *p)
{
  if (p == nullptr)
    return ScaleMode::Chromatic;
  return p->getScaleMode() == ScaleMode::None ? ScaleMode::None : p->getPreferredScaleMode();
}

void PianoRollComponent::setProject(Project *proj)
{
  if (pitchDrawingHandler_)
    pitchDrawingHandler_->cancel();
  liveTimelineEndSeconds = 0.0;
  liveRecordingSampleRate = 0.0;
  project = proj;
  hoveredNote = nullptr;
  setPreviewPlaybackState(false, 0, 0);
  selectedScaleMode = keyModeOf(project);
  selectedScaleRootNote = project != nullptr ? project->getScaleRootNote() : 0;
  pitchReferenceHz = project != nullptr ? project->getPitchReferenceHz() : 440;
  snapToSemitoneDrag = project != nullptr ? project->getSnapToSemitones() : false;
  dragSnapMode = project != nullptr
                     ? project->getDragSnapMode()
                     : DragSnapMode::Chromatic;
  timelineDisplayMode = project != nullptr
                            ? project->getTimelineDisplayMode()
                            : TimelineDisplayMode::Beats;
  timelineBeatNumerator = project != nullptr ? project->getTimelineBeatNumerator() : 4;
  timelineBeatDenominator =
      project != nullptr ? project->getTimelineBeatDenominator() : 4;
  timelineTempoBpm = project != nullptr ? project->getTimelineTempoBpm() : 120.0;
  timelineGridDivision = project != nullptr
                             ? project->getTimelineGridDivision()
                             : TimelineGridDivision::Quarter;
  timelineSnapCycle = project != nullptr ? project->getTimelineSnapCycle() : false;
  previewScaleRootNote.reset();
  previewScaleMode.reset();

  // Update modular components
  gridRenderer->setProject(proj);
  timelineRenderer->setProject(proj);
  waveformBackgroundRenderer->setProject(proj); // also clears its cache
  noteRenderer->setProject(proj);
  pitchCurveRenderer->setProject(proj); // also clears its cache
  scrollZoomController->setProject(proj);
  pitchEditor->setProject(proj);
  pitchEditor->setSnapToSemitoneDragEnabled(snapToSemitoneDrag);
  pitchEditor->setDragSnapMode(dragSnapMode);
  pitchEditor->setPitchReferenceHz(pitchReferenceHz);
  noteSplitter->setProject(proj);
  pitchToolController->setProject(proj);

  // Note: waveform and base-pitch caches are cleared by their renderers'
  // setProject calls above.

  updatePitchToolHandlesFromSelection();

  updateScrollBars();
  repaint();
}

void PianoRollComponent::beginLiveRecordingWaveform(
    double sampleRate, double timelineOffsetSeconds)
{
  waveformBackgroundRenderer->beginLiveWaveform(sampleRate,
                                                timelineOffsetSeconds);
  liveRecordingSampleRate = sampleRate;
  liveTimelineEndSeconds = std::max(0.0, timelineOffsetSeconds);
  updateScrollBars();
  repaint();
}

void PianoRollComponent::appendLiveRecordingWaveform(
    const juce::AudioBuffer<float> &buffer)
{
  waveformBackgroundRenderer->appendLiveWaveform(buffer);
  if (liveRecordingSampleRate > 0.0)
    liveTimelineEndSeconds +=
        static_cast<double>(buffer.getNumSamples()) / liveRecordingSampleRate;
  updateScrollBars();
  repaint();
}

bool PianoRollComponent::extendTimelineTo(double endSeconds)
{
  const double newEndSeconds = std::max(0.0, endSeconds);
  if (newEndSeconds <= hostTimelineEndSeconds)
    return false;

  hostTimelineEndSeconds = newEndSeconds;
  updateScrollBars();
  repaint();
  return true;
}

void PianoRollComponent::refreshScaleFromProject()
{
  selectedScaleRootNote = project ? project->getScaleRootNote() : 0;
  selectedScaleMode = keyModeOf(project);
  previewScaleRootNote.reset();
  previewScaleMode.reset();
  repaint();
}

void PianoRollComponent::setScaleMode(ScaleMode mode)
{
  if (selectedScaleMode == mode && !previewScaleMode.has_value())
    return;

  selectedScaleMode = mode;
  if (project != nullptr)
    project->setScaleMode(mode);
  previewScaleMode.reset();
  repaint();
}

void PianoRollComponent::setScaleRootNote(int noteInOctave)
{
  const int normalized = juce::jlimit(-1, 11, noteInOctave);
  const bool changed = selectedScaleRootNote != normalized;
  if (!changed && !previewScaleRootNote.has_value())
    return;

  selectedScaleRootNote = normalized;
  if (project != nullptr && changed)
    project->setScaleRootNote(normalized);
  previewScaleRootNote.reset();
  repaint();
}

void PianoRollComponent::setScaleRootPreview(std::optional<int> noteInOctave)
{
  std::optional<int> normalizedPreview;
  if (noteInOctave.has_value())
    normalizedPreview = juce::jlimit(-1, 11, *noteInOctave);

  if (previewScaleRootNote == normalizedPreview)
    return;

  previewScaleRootNote = normalizedPreview;
  repaint();
}

void PianoRollComponent::setScaleModePreview(std::optional<ScaleMode> mode)
{
  if (mode.has_value() &&
      (selectedScaleMode == ScaleMode::Chromatic ||
       selectedScaleMode == ScaleMode::None) &&
      !(snapToSemitoneDrag && dragSnapMode == DragSnapMode::Scale))
    mode.reset();

  if (previewScaleMode == mode)
    return;

  previewScaleMode = mode;
  repaint();
}

void PianoRollComponent::setSnapToSemitoneDrag(bool enabled)
{
  if (snapToSemitoneDrag == enabled)
    return;

  snapToSemitoneDrag = enabled;
  if (project != nullptr)
    project->setSnapToSemitones(enabled);
  pitchEditor->setSnapToSemitoneDragEnabled(enabled);
  repaint();
}

void PianoRollComponent::setDragSnapMode(DragSnapMode mode)
{
  if (dragSnapMode == mode)
    return;

  dragSnapMode = mode;
  if (project != nullptr)
    project->setDragSnapMode(mode);
  pitchEditor->setDragSnapMode(mode);
  repaint();
}

void PianoRollComponent::setPitchReferenceHz(int hz)
{
  const int normalized = juce::jlimit(400, 480, hz);
  if (pitchReferenceHz == normalized)
    return;

  pitchReferenceHz = normalized;
  if (project != nullptr)
    project->setPitchReferenceHz(normalized);
  pitchEditor->setPitchReferenceHz(normalized);
  repaint();
}

void PianoRollComponent::setTimelineDisplayMode(TimelineDisplayMode mode)
{
  if (timelineDisplayMode == mode)
    return;

  timelineDisplayMode = mode;
  if (project != nullptr)
    project->setTimelineDisplayMode(mode);
  repaint();
}

void PianoRollComponent::setTimelineBeatSignature(int numerator, int denominator)
{
  const int normalizedNumerator = juce::jlimit(1, 32, numerator);
  const int normalizedDenominator = normalizeTimelineBeatDenominator(denominator);
  if (timelineBeatNumerator == normalizedNumerator &&
      timelineBeatDenominator == normalizedDenominator)
    return;

  timelineBeatNumerator = normalizedNumerator;
  timelineBeatDenominator = normalizedDenominator;
  if (project != nullptr)
    project->setTimelineBeatSignature(normalizedNumerator, normalizedDenominator);
  repaint();
}

void PianoRollComponent::setTimelineTempoBpm(double bpm)
{
  const double normalized = juce::jlimit(20.0, 300.0, bpm);
  if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
    return;

  timelineTempoBpm = normalized;
  if (project != nullptr)
    project->setTimelineTempoBpm(normalized);
  repaint();
}

void PianoRollComponent::setTimelineGridDivision(TimelineGridDivision division)
{
  if (timelineGridDivision == division)
    return;

  timelineGridDivision = division;
  if (project != nullptr)
    project->setTimelineGridDivision(division);
  repaint();
}

void PianoRollComponent::setTimelineSnapCycle(bool enabled)
{
  if (timelineSnapCycle == enabled)
    return;

  timelineSnapCycle = enabled;
  if (project != nullptr)
    project->setTimelineSnapCycle(enabled);
}

void PianoRollComponent::setCycleEditEnabled(bool enabled)
{
  if (cycleEditEnabled == enabled)
    return;

  cycleEditEnabled = enabled;
  if (!enabled && loopDragHandler_)
    loopDragHandler_->cancel();
  setMouseCursor(juce::MouseCursor::NormalCursor);
  repaint();
}

void PianoRollComponent::setUndoManager(PitchUndoManager *manager)
{
  undoManager = manager;
  pitchEditor->setUndoManager(manager);
  noteSplitter->setUndoManager(manager);
}

bool PianoRollComponent::nudgeSelectedNotesBySemitones(int semitoneDelta)
{
  if (project == nullptr || semitoneDelta == 0)
    return false;

  // Other tracks open for editing: their selected notes move too, each
  // track rendered by its own instance (one undo step per track).
  bool movedOthers = false;
  if (multiTrackEditing() && getTrackEditTargets)
    for (auto &target : getTrackEditTargets())
    {
      if (target.project == nullptr || target.project == project ||
          (target.isValid && !target.isValid()))
        continue;
      std::vector<Note *> notes, moved;
      std::vector<float> oldMidis, newMidis;
      for (auto &note : target.project->getNotes())
        if (!note.isRest() && note.isSelected())
          notes.push_back(&note);
      int first = std::numeric_limits<int>::max(), last = std::numeric_limits<int>::min();
      for (auto *note : notes)
      {
        if (note == nullptr || note->isRest())
          continue;
        const float offset = note->getPitchOffset();
        const float to = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE), static_cast<float>(MAX_MIDI_NOTE),
                                      note->getMidiNote() + offset + static_cast<float>(semitoneDelta)) - offset;
        if (std::abs(to - note->getMidiNote()) <= 1.0e-6f)
          continue;
        moved.push_back(note);
        oldMidis.push_back(note->getMidiNote());
        newMidis.push_back(to);
        first = std::min(first, note->getStartFrame());
        last = std::max(last, note->getEndFrame());
      }
      if (moved.empty())
        continue;
      auto *targetProject = target.project;
      auto refresh = [targetProject, first, last, onChanged = target.onChanged](const std::vector<Note *> &changed)
      {
        PitchCurveProcessor::rebuildBaseFromNotes(*targetProject);
        for (auto *note : changed)
          if (note)
          {
            note->markDirty();
            note->markSynthDirty();
          }
        targetProject->markNoteEditDirtyRange(first, last);
        if (onChanged)
          onChanged(true);
      };
      if (undoManager)
        undoManager->addAction(std::make_unique<MultiNoteMidiNudgeAction>(moved, oldMidis, newMidis, refresh));
      for (size_t i = 0; i < moved.size(); ++i)
        moved[i]->setMidiNote(newMidis[i]);
      refresh(moved);
      movedOthers = true;
    }

  auto selectedNotes = project->getSelectedNotes();
  if (selectedNotes.empty())
  {
    if (movedOthers)
      repaint();
    return movedOthers;
  }

  constexpr float minMidi = static_cast<float>(MIN_MIDI_NOTE);
  constexpr float maxMidi = static_cast<float>(MAX_MIDI_NOTE);

  std::vector<Note *> notesToMove;
  std::vector<float> oldMidis;
  std::vector<float> newMidis;
  notesToMove.reserve(selectedNotes.size());
  oldMidis.reserve(selectedNotes.size());
  newMidis.reserve(selectedNotes.size());

  int dirtyStartFrame = std::numeric_limits<int>::max();
  int dirtyEndFrame = std::numeric_limits<int>::min();

  for (auto *note : selectedNotes)
  {
    if (!note || note->isRest())
      continue;

    const float oldMidi = note->getMidiNote();
    const float offset = note->getPitchOffset();
    const float oldAdjustedMidi = oldMidi + offset;
    const float movedAdjustedMidi =
        juce::jlimit(minMidi, maxMidi,
                     oldAdjustedMidi + static_cast<float>(semitoneDelta));
    const float movedMidi = movedAdjustedMidi - offset;

    if (std::abs(movedMidi - oldMidi) <= 1.0e-6f)
      continue;

    notesToMove.push_back(note);
    oldMidis.push_back(oldMidi);
    newMidis.push_back(movedMidi);
    dirtyStartFrame = std::min(dirtyStartFrame, note->getStartFrame());
    dirtyEndFrame = std::max(dirtyEndFrame, note->getEndFrame());
  }

  if (notesToMove.empty())
    return false;

  auto rebuildAndNotify =
      [this, dirtyStartFrame, dirtyEndFrame](const std::vector<Note *> &)
  {
    if (project == nullptr)
      return;

    PitchCurveProcessor::rebuildBaseFromNotes(*project);
    invalidateBasePitchCache();

    project->markNoteEditDirtyRange(dirtyStartFrame, dirtyEndFrame);

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();

    repaint();
  };

  if (undoManager)
  {
    auto *projectPtr = project;
    auto action = std::make_unique<MultiNoteMidiNudgeAction>(
        notesToMove, oldMidis, newMidis,
        [projectPtr, dirtyStartFrame,
         dirtyEndFrame](const std::vector<Note *> &notes)
        {
          if (!projectPtr)
            return;
          PitchCurveProcessor::rebuildBaseFromNotes(*projectPtr);
          for (auto *note : notes)
            if (note)
              note->markSynthDirty();
          projectPtr->markNoteEditDirtyRange(dirtyStartFrame, dirtyEndFrame);
        });
    undoManager->addAction(std::move(action));
  }

  for (size_t i = 0; i < notesToMove.size(); ++i)
  {
    notesToMove[i]->setMidiNote(newMidis[i]);
    notesToMove[i]->markDirty();
    notesToMove[i]->markSynthDirty();
  }

  rebuildAndNotify(notesToMove);
  return true;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key)
{
  const auto mods = key.getModifiers();
  if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
    return false;

  const int keyCode = key.getKeyCode();
  if (keyCode == juce::KeyPress::upKey || keyCode == juce::KeyPress::downKey)
  {
    const int direction = keyCode == juce::KeyPress::upKey ? 1 : -1;
    const int step = mods.isShiftDown() ? 12 : 1;
    return nudgeSelectedNotesBySemitones(direction * step);
  }

  if (keyCode == juce::KeyPress::deleteKey ||
      keyCode == juce::KeyPress::backspaceKey)
    return deleteSelectedNotes();

  return false;
}

std::vector<TrackPitchMacroTarget> PianoRollComponent::editableTracks()
{
  if (multiTrackEditing() && getTrackEditTargets)
  {
    auto targets = getTrackEditTargets();
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [this](const auto &t) {
                                   return t.project == nullptr ||
                                          (t.isValid && !t.isValid()) ||
                                          (!ownTrackEditable && t.project == project);
                                 }),
                  targets.end());
    if (!targets.empty())
      return targets;
  }
  if (!project || !ownTrackEditable)
    return {};
  return {{project, {}, {}, {}}};
}

bool PianoRollComponent::copySelectedNotes(bool cut)
{
  // Copy takes the selected notes of every track being edited,
  // each with its audio; Cut then removes them.
  std::vector<std::shared_ptr<NoteClipboard::Clip>> clips;
  for (const auto &target : editableTracks())
    if (auto clip = NoteClipboard::copySelection(
            *target.project, juce::String::toHexString(static_cast<juce::int64>(
                                 reinterpret_cast<std::uintptr_t>(target.project)))))
      clips.push_back(std::move(clip));
  if (clips.empty())
    return false;
  NoteClipboard::contents() = clips;

  // The cursor moves to the quarter note nearest the first copied note.
  double first = clips.front()->firstNoteSeconds;
  for (const auto &clip : clips)
    first = std::min(first, clip->firstNoteSeconds);
  const double bpm = project ? project->getTimelineTempoBpm() : 120.0;
  const double quarter = 60.0 / std::max(1.0, bpm);
  setCursorTime(std::round(first / quarter) * quarter);

  if (cut)
    deleteSelectedNotes();
  return true;
}

bool PianoRollComponent::pasteNotes()
{
  const auto clips = NoteClipboard::contents();
  auto dests = editableTracks();
  if (clips.empty() || dests.empty())
    return false;

  // Which clip goes to which track: as many tracks as clips ->
  // in order; one clip -> into every track being edited; several clips but
  // one track -> all into that track; otherwise in order as far as both go.
  std::vector<std::pair<size_t, size_t>> pairs; // clip index, dest index
  if (clips.size() == dests.size())
    for (size_t i = 0; i < clips.size(); ++i) pairs.push_back({i, i});
  else if (clips.size() == 1)
    for (size_t d = 0; d < dests.size(); ++d) pairs.push_back({0, d});
  else if (dests.size() == 1)
    for (size_t i = 0; i < clips.size(); ++i) pairs.push_back({i, 0});
  else
    for (size_t i = 0; i < std::min(clips.size(), dests.size()); ++i) pairs.push_back({i, i});

  // Where: selected notes in a destination are replaced from their start;
  // otherwise the cursor, keeping the copy's offset from its beat.
  double base = clips.front()->firstNoteSeconds;
  for (const auto &clip : clips)
    base = std::min(base, clip->firstNoteSeconds);
  const double bpm = project ? project->getTimelineTempoBpm() : 120.0;
  const double quarter = 60.0 / std::max(1.0, bpm);
  const double offsetFromBeat = base - std::round(base / quarter) * quarter;
  const double cursorStart = cursorTime + offsetFromBeat;

  struct Applied
  {
    TrackPitchMacroTarget target;
    std::shared_ptr<NoteClipboard::Slice> slice;
  };
  auto applied = std::make_shared<std::vector<Applied>>();
  double lastEnd = cursorStart;
  for (const auto &[clipIndex, destIndex] : pairs)
  {
    const auto &clip = *clips[clipIndex];
    auto &target = dests[destIndex];
    int destFrame = -1;
    for (const auto &note : target.project->getNotes())
      if (!note.isRest() && note.isSelected())
        destFrame = destFrame < 0 ? note.getStartFrame()
                                  : std::min(destFrame, note.getStartFrame());
    if (destFrame < 0)
      destFrame = secondsToFrames(static_cast<float>(
          std::max(0.0, cursorStart + (clip.firstNoteSeconds - base))));
    auto slice = std::make_shared<NoteClipboard::Slice>(
        NoteClipboard::paste(*target.project, clip, destFrame));
    if (slice->lengthFrames <= 0)
      continue;
    lastEnd = std::max(lastEnd, static_cast<double>(
                                    framesToSeconds(slice->startFrame + slice->lengthFrames)));
    applied->push_back({target, slice});
  }
  if (applied->empty())
    return false;

  auto safe = juce::Component::SafePointer<PianoRollComponent>(this);
  const auto refresh = [safe, applied] {
    if (!safe)
      return;
    for (auto &item : *applied)
    {
      if (item.target.isValid && !item.target.isValid())
        continue;
      auto &p = *item.target.project;
      PitchCurveProcessor::rebuildBaseFromNotes(p);
      p.markNoteEditDirtyRange(item.slice->startFrame,
                               item.slice->startFrame + item.slice->lengthFrames);
      p.setModified(true);
      if (item.target.project == safe->project)
      {
        if (safe->onPitchEdited) safe->onPitchEdited();
        if (safe->onPitchEditFinished) safe->onPitchEditFinished();
      }
      else if (item.target.onChanged)
        item.target.onChanged(true);
    }
    safe->invalidateBasePitchCache();
    safe->repaint();
  };
  refresh();

  class PasteAction final : public UndoableAction
  {
  public:
    PasteAction(std::shared_ptr<std::vector<Applied>> items, std::function<void()> after)
        : items(std::move(items)), after(std::move(after)) {}
    void undo() override { swap(); }
    void redo() override { swap(); }
    juce::String getName() const override { return "Paste Notes"; }
    bool ownsProjectContext() const override { return true; }
    bool requiresAudioResynthesis() const override { return false; }
  private:
    void swap()
    {
      for (auto &item : *items)
        if (!item.target.isValid || item.target.isValid())
          NoteClipboard::swapSlice(*item.target.project, *item.slice);
      if (after) after();
    }
    std::shared_ptr<std::vector<Applied>> items;
    std::function<void()> after;
  };
  if (undoManager)
    undoManager->addAction(std::make_unique<PasteAction>(applied, refresh));

  // The cursor moves on to the first quarter after the paste, so pasting
  // again strings copies together on the beat.
  setCursorTime(std::ceil(lastEnd / quarter) * quarter);
  return true;
}

bool PianoRollComponent::deleteSelectedNotes()
{
  if (!project || !pitchToolController)
    return false;

  const auto selectedLiveNotes = [](Project &owner, std::vector<Note *> &notes,
                                    std::vector<float> &before) {
    for (auto &note : owner.getNotes())
      if (!note.isRest() && note.isSelected() &&
          note.getVolumeDb() > NoteGainCurve::kDeletedNoteDb)
      {
        notes.push_back(&note);
        before.push_back(note.getVolumeDb());
      }
  };

  bool deleted = false;
  std::vector<Note *> targets;
  std::vector<float> before;
  selectedLiveNotes(*project, targets, before);
  if (!targets.empty())
  {
    // A gain edit, like dragging the amplitude handle to the floor: it
    // changes the composite in place, so it needs no resynthesis and undoes
    // exactly. "Restore Amplitude" on a note brings it back too.
    std::vector<float> after(targets.size(), NoteGainCurve::kDeletedNoteDb);
    applyRestoreAction(std::make_unique<AmplitudeAction>(
        *project, targets, before, after, pitchToolController->onAmplitudeEdited,
        "Delete Notes"));
    deleted = true;
  }

  // Other tracks open for editing in this canvas (Ctrl+Edit):
  // the same edit on their projects, which their own instances then render.
  if (multiTrackEditing() && getTrackEditTargets)
    for (auto &target : getTrackEditTargets())
    {
      if (target.project == nullptr || target.project == project ||
          (target.isValid && !target.isValid()))
        continue;
      std::vector<Note *> notes;
      std::vector<float> levels;
      selectedLiveNotes(*target.project, notes, levels);
      if (notes.empty())
        continue;
      std::vector<float> after(notes.size(), NoteGainCurve::kDeletedNoteDb);
      auto changed = [onChanged = target.onChanged] {
        if (onChanged)
          onChanged(true);
      };
      applyRestoreAction(std::make_unique<AmplitudeAction>(
          *target.project, notes, levels, after, changed, "Delete Notes"));
      deleted = true;
    }
  return deleted;
}

bool PianoRollComponent::keyPressed(const juce::KeyPress &key,
                                    juce::Component *)
{
  return keyPressed(key);
}

void PianoRollComponent::focusLost(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Don't automatically re-grab focus - let the host manage focus normally
  // Focus will be re-acquired when user clicks on the piano roll
}

void PianoRollComponent::focusGained(FocusChangeType cause)
{
  juce::ignoreUnused(cause);
  // Focus gained - nothing special needed
}

void PianoRollComponent::setTimelineDisplayOffset(double seconds)
{
  if (!coordMapper ||
      juce::approximatelyEqual(coordMapper->getTimelineDisplayOffset(), seconds))
    return;

  const double previous = coordMapper->getTimelineDisplayOffset();
  coordMapper->setTimelineDisplayOffset(seconds);

  if (trackViewMode)
  {
    // The canvas is in the active modification's time, so a new offset (a
    // different region became active, or it moved) shifts where the host
    // timeline sits on it. Move the scroll with it so the track itself stays
    // still on screen.
    scrollX -= (seconds - previous) * pixelsPerSecond;
    coordMapper->setViewStartSeconds(getTimelineStart());
    coordMapper->setScrollX(scrollX);
    updateScrollBars();
  }

  // Region previews and the Track-mode clip are placed through the offset.
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  invalidateWaveformCache();
  updatePreviewButtonBounds();
  repaint();
}

double PianoRollComponent::getHostScrollSeconds() const
{
  return projectToTimeline(scrollX / std::max(1.0e-6, (double)pixelsPerSecond));
}

double PianoRollComponent::projectToTimeline(double projectSeconds) const
{
  return coordMapper ? coordMapper->projectToTimeline(projectSeconds)
                     : projectSeconds;
}

double PianoRollComponent::timelineToProject(double timelineSeconds) const
{
  return coordMapper ? coordMapper->timelineToProject(timelineSeconds)
                     : timelineSeconds;
}

void PianoRollComponent::setCursorTime(double time)
{
  if (std::abs(cursorTime - time) < 0.0001)
    return; // Skip if no change

  // Calculate dirty rectangle for cursor position
  // Include full height so the old and new cursor lines are fully repainted.
  auto getCursorRect = [this](double t) -> juce::Rectangle<int>
  {
    float x =
        static_cast<float>(t * pixelsPerSecond - scrollX) + pianoKeysWidth;
    constexpr int cursorHalfWidth = 2;
    int rectX = static_cast<int>(x) - cursorHalfWidth;
    int rectWidth = cursorHalfWidth * 2 + 1;
    return juce::Rectangle<int>(rectX, 0, rectWidth, getHeight());
  };

  const auto oldCursorRect = getCursorRect(cursorTime);
  cursorTime = time;
  const auto cursorDirtyArea =
      oldCursorRect.getUnion(getCursorRect(cursorTime))
          .getIntersection(getLocalBounds());

  // Erase the old playhead and draw the new one in a single paint operation.
  // Separate invalidations can be presented independently when the positions
  // no longer overlap at high horizontal zoom, producing a visible blink.
  if (!cursorDirtyArea.isEmpty())
    repaint(cursorDirtyArea);

  if (onCursorMoved)
    onCursorMoved();
}

void PianoRollComponent::setPlaybackActive(bool active)
{
  if (playbackActive == active)
    return;

  playbackActive = active;
  repaint();
}

void PianoRollComponent::setPixelsPerSecond(float pps, bool centerOnCursor)
{
  float oldPps = pixelsPerSecond;
  const int visibleWidth = getVisibleContentWidth();
  const double totalTime = getTimelineLength();
  const float minPpsX =
      (visibleWidth > 0 && totalTime > 0.0)
          ? std::max(MIN_PIXELS_PER_SECOND,
                     static_cast<float>(visibleWidth / totalTime) * ZOOM_OUT_BEYOND_FIT)
          : MIN_PIXELS_PER_SECOND;
  float newPps =
      juce::jlimit(minPpsX, MAX_PIXELS_PER_SECOND, pps);

  if (std::abs(oldPps - newPps) < 0.01f)
    return; // No significant change

  if (centerOnCursor)
  {
    // Calculate cursor position relative to view
    float cursorX = static_cast<float>(cursorTime * oldPps);
    float cursorRelativeX = cursorX - static_cast<float>(scrollX);

    // Calculate new scroll position to keep cursor at same relative position
    float newCursorX = static_cast<float>(cursorTime * newPps);
    pixelsPerSecond = newPps;
    coordMapper->setPixelsPerSecond(newPps);
    scrollX = clampScrollX(static_cast<double>(newCursorX - cursorRelativeX));
    coordMapper->setScrollX(scrollX);
  }

  pixelsPerSecond = newPps;
  coordMapper->setPixelsPerSecond(newPps);
  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();

  // Don't call onZoomChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components
}

void PianoRollComponent::setPixelsPerSemitone(float pps, float anchorContentY)
{
  const float oldPps = pixelsPerSemitone;
  const int visibleHeight = getVisibleContentHeight();
  const float minPpsForFill =
      visibleHeight > 0
          ? static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1)
          : MIN_PIXELS_PER_SEMITONE;
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);

  const float newPps = juce::jlimit(minPps, MAX_PIXELS_PER_SEMITONE, pps);
  if (std::abs(oldPps - newPps) < 0.01f)
    return;

  float effectiveAnchorY = anchorContentY;
  if (effectiveAnchorY < 0.0f)
    effectiveAnchorY = static_cast<float>(visibleHeight) * 0.5f;
  effectiveAnchorY = juce::jlimit(0.0f, static_cast<float>(visibleHeight),
                                  effectiveAnchorY);

  const float midiAtAnchor =
      MAX_MIDI_NOTE -
      (effectiveAnchorY + static_cast<float>(scrollY)) / oldPps;

  pixelsPerSemitone = newPps;
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double anchoredScrollY =
      (MAX_MIDI_NOTE - midiAtAnchor) * pixelsPerSemitone - effectiveAnchorY;
  scrollY = juce::jlimit(0.0, maxScrollY, anchoredScrollY);
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setScrollX(double x)
{
  const double clampedX = clampScrollX(x);

  if (std::abs(scrollX - clampedX) < 0.01)
    return; // No significant change

  scrollX = clampedX;
  coordMapper->setScrollX(clampedX);
  horizontalScrollBar.setCurrentRangeStart(clampedX);

  // Don't call onScrollChanged here to avoid infinite recursion
  // The caller is responsible for synchronizing other components

  repaint();
  updatePreviewButtonBounds();
}

void PianoRollComponent::setScrollY(double y)
{
  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY = std::max(
      0.0, totalHeight - static_cast<double>(getVisibleContentHeight()));
  const double clampedY = juce::jlimit(0.0, maxScrollY, y);

  if (std::abs(scrollY - clampedY) < 0.01)
    return;

  scrollY = clampedY;
  coordMapper->setScrollY(clampedY);
  verticalScrollBar.setCurrentRangeStart(clampedY);
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::centerOnPitchRange(float minMidi, float maxMidi)
{
  // Calculate center MIDI note
  float centerMidi = (minMidi + maxMidi) / 2.0f;

  // Calculate Y position for center
  float centerY = midiToY(centerMidi);

  // Get visible height
  int visibleHeight = getHeight() - 8; // scrollbar height

  // Calculate scroll position to center the pitch range
  double newScrollY = centerY - visibleHeight / 2.0;

  // Clamp to valid range
  double totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  newScrollY =
      juce::jlimit(0.0, std::max(0.0, totalHeight - visibleHeight), newScrollY);

  scrollY = newScrollY;
  coordMapper->setScrollY(newScrollY);
  verticalScrollBar.setCurrentRangeStart(newScrollY);
  updatePreviewButtonBounds();
  repaint();
}

bool PianoRollComponent::centerOnCurrentPitchRange()
{
  if (project == nullptr)
    return false;

  float minMidi = std::numeric_limits<float>::max();
  float maxMidi = std::numeric_limits<float>::lowest();
  bool hasNotePitch = false;

  for (const auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;

    const float midi = note.getAdjustedMidiNote();
    minMidi = std::min(minMidi, midi);
    maxMidi = std::max(maxMidi, midi);
    hasNotePitch = true;

    for (const float delta : note.getActiveDeltaPitch())
    {
      minMidi = std::min(minMidi, midi + delta);
      maxMidi = std::max(maxMidi, midi + delta);
    }
  }

  if (!hasNotePitch)
    return false;

  minMidi -= 1.0f;
  maxMidi += 1.0f;
  if (maxMidi - minMidi < 2.0f)
  {
    const float centerMidi = (minMidi + maxMidi) * 0.5f;
    minMidi = centerMidi - 1.0f;
    maxMidi = centerMidi + 1.0f;
  }

  centerOnPitchRange(minMidi, maxMidi);
  return true;
}

void PianoRollComponent::fitPitchRangeToView(float minMidi, float maxMidi)
{
  if (!std::isfinite(minMidi) || !std::isfinite(maxMidi))
    return;

  if (minMidi > maxMidi)
    std::swap(minMidi, maxMidi);

  minMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), minMidi);
  maxMidi = juce::jlimit(static_cast<float>(MIN_MIDI_NOTE),
                         static_cast<float>(MAX_MIDI_NOTE), maxMidi);

  const int visibleHeight = getVisibleContentHeight();
  if (visibleHeight <= 0)
    return;

  const float rangeSemitones = std::max(1.0f, maxMidi - minMidi + 1.0f);
  const float minPpsForFill =
      static_cast<float>(visibleHeight) / (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1);
  const float minPps = std::max(MIN_PIXELS_PER_SEMITONE, minPpsForFill);
  pixelsPerSemitone = juce::jlimit(
      minPps, MAX_PIXELS_PER_SEMITONE,
      static_cast<float>(visibleHeight) / rangeSemitones);
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);

  const double totalHeight =
      (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;
  const double maxScrollY =
      std::max(0.0, totalHeight - static_cast<double>(visibleHeight));
  const double lowestNoteBottomY =
      (static_cast<double>(MAX_MIDI_NOTE) - static_cast<double>(minMidi) +
       1.0) *
      static_cast<double>(pixelsPerSemitone);

  scrollY = juce::jlimit(
      0.0, maxScrollY,
      lowestNoteBottomY - static_cast<double>(visibleHeight));
  coordMapper->setScrollY(scrollY);

  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::setEditMode(EditMode mode)
{
  const EditMode previousMode = editMode;
  if (previousMode == EditMode::PitchDrawing && mode != EditMode::PitchDrawing &&
      pitchDrawingHandler_)
  {
    pitchDrawingHandler_->cancel();
  }
  if (previousMode == EditMode::Timing && mode != EditMode::Timing &&
      timingHandler_)
    timingHandler_->cancel();
  editMode = mode;

  if (mode == EditMode::PitchDrawing)
    setHoveredNote(nullptr);

  // Clear split guide when leaving split mode
  if (mode != EditMode::Split && splitHandler_)
  {
    splitHandler_->clearGuide();
  }

  updateMouseCursorForEditMode();

  // Update currentHandler_ based on the new mode
  switch (mode)
  {
  case EditMode::Select:
    currentHandler_ = selectHandler_.get();
    break;
  case EditMode::Split:
    currentHandler_ = splitHandler_.get();
    break;
  case EditMode::PitchDrawing:
    currentHandler_ = pitchDrawingHandler_.get();
    break;
  case EditMode::Timing:
    currentHandler_ = timingHandler_.get();
    break;
  }

  if (mode != EditMode::Select)
  {
    hoveredPitchToolHandle = -1;
    if (pitchToolHandles)
      pitchToolHandles->setHoveredHandleIndex(-1);
  }
  updatePitchToolHandlesFromSelection();
  updatePitchDrawingConfirmationPopup();

  repaint();
}

void PianoRollComponent::updateMouseCursorForEditMode()
{
  if (editMode == EditMode::Split)
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::PitchDrawing)
  {
    const auto pointer = getMouseXYRelative();
    const bool overCanvas = pointer.x >= pianoKeysWidth &&
                            pointer.y >= headerHeight &&
                            pointer.x < pianoKeysWidth + getVisibleContentWidth() &&
                            pointer.y < headerHeight + getVisibleContentHeight();
    const float worldX = pointer.x - pianoKeysWidth +
                         static_cast<float>(scrollX);
    if (overCanvas && pitchDrawingHandler_ &&
        pitchDrawingHandler_->isPitchRegionAtWorldX(worldX))
      setMouseCursor(pitchDrawingHandler_->isHoveringAnchor()
                         ? juce::MouseCursor::DraggingHandCursor
                         : juce::MouseCursor::CrosshairCursor);
    else
      setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else if (editMode == EditMode::Timing)
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
  else
  {
    setMouseCursor(juce::MouseCursor::NormalCursor);
  }
}

std::vector<Note *> PianoRollComponent::getSelectedNotes() const
{
  if (!project || !ownTrackEditable)
    return {};

  std::vector<Note *> selected;
  for (auto &note : project->getNotes())
  {
    if (note.isSelected())
      selected.push_back(&note);
  }
  return selected;
}

void PianoRollComponent::updatePitchToolHandlesFromSelection()
{
  if (!pitchToolHandles || !coordMapper)
    return;

  const bool isChangingPitch =
      (selectHandler_ && (selectHandler_->isSingleNoteDragging() ||
                          selectHandler_->getIsDeltaScaleDragging() ||
                          selectHandler_->getIsDeltaOffsetDragging())) ||
      (pitchToolController && pitchToolController->isDragging()) ||
      (pitchEditor && (pitchEditor->isDraggingNote() ||
                       pitchEditor->isDraggingMultiNotes()));

  if (!project || editMode != EditMode::Select)
  {
    pitchToolHandles->clear();
    hoveredPitchToolHandle = -1;
    pitchToolHandles->setHoveredHandleIndex(-1);
    return;
  }

  if (isChangingPitch)
  {
    pitchToolHandles->clear();
    hoveredPitchToolHandle = -1;
    pitchToolHandles->setHoveredHandleIndex(-1);
    return;
  }

  // Vibrato is a hover control: no hovered note means no visible handle.
  std::vector<Note *> targetNotes;
  if (hoveredNote)
    targetNotes.push_back(hoveredNote);

  const auto hoverBounds = hoveredNote ? getNoteHoverShadowBounds(*hoveredNote)
                                       : juce::Rectangle<float>();
  pitchToolHandles->updateHandles(targetNotes, *coordMapper, hoverBounds);
  if (hoveredPitchToolHandle >=
      static_cast<int>(pitchToolHandles->getHandles().size()))
  {
    hoveredPitchToolHandle = -1;
    pitchToolHandles->setHoveredHandleIndex(-1);
  }
}

Note *PianoRollComponent::findNoteAt(float x, float y)
{
  if (!project || !ownTrackEditable)
    return nullptr;

  for (auto &note : project->getNotes())
  {
    // Skip rest notes
    if (note.isRest())
      continue;

    float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
    float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
    float noteY = midiToY(note.getAdjustedMidiNote());
    float noteH = pixelsPerSemitone;

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
    {
      return &note;
    }
  }

  return nullptr;
}

juce::Rectangle<float>
PianoRollComponent::getNoteHoverShadowBounds(const Note &note) const
{
  if (!project)
    return {};

  const auto &audioData = project->getAudioData();
  const float noteStartTime = static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime = static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float x = noteStartTime * pixelsPerSecond;
  const float w = (noteEndTime - noteStartTime) * pixelsPerSecond;
  const float renderedWidth = std::max(w, 4.0f);
  const float h = pixelsPerSemitone;
  const float y = midiToY(note.getAdjustedMidiNote());

  juce::Rectangle<float> shadowBounds(x, y, renderedWidth, h);
  const float *samples = nullptr;
  int totalSamples = 0;
  int startSample = 0;
  int endSample = 0;
  if (audioData.waveform.getNumSamples() > 0)
  {
    samples = audioData.waveform.getReadPointer(0);
    totalSamples = audioData.waveform.getNumSamples();
    startSample = static_cast<int>(noteStartTime * audioData.sampleRate);
    endSample = static_cast<int>(noteEndTime * audioData.sampleRate);
    startSample = std::max(0, std::min(startSample, totalSamples - 1));
    endSample = std::max(startSample + 1, std::min(endSample, totalSamples));
  }

  if (samples && totalSamples > 0 && w > 2.0f && endSample > startSample)
  {
    const int pointCount =
        std::max(2, std::min(512, static_cast<int>(std::ceil(w)) + 1));
    const auto envelope = VisualWaveformEnvelope::build(
        samples, totalSamples, startSample, endSample, pointCount,
        renderedWidth, audioData.sampleRate, pixelsPerSecond, true,
        pitchToolController ? pitchToolController->getAmplitudePreviewGain(note) : 1.0f);
    const float maxSample =
        envelope.empty()
            ? 0.0f
            : *std::max_element(envelope.begin(), envelope.end());

    const float centreY = y + h * 0.5f;
    const float waveHeight = h * 3.0f;
    shadowBounds = juce::Rectangle<float>(x, centreY - maxSample * waveHeight * 0.5f,
                                          renderedWidth, maxSample * waveHeight);
  }

  shadowBounds = shadowBounds.expanded(4.0f, 4.0f);
  if (shadowBounds.getWidth() < PitchToolHandles::buttonGroupWidth)
  {
    const float centreX = shadowBounds.getCentreX();
    shadowBounds.setX(centreX - PitchToolHandles::buttonGroupWidth * 0.5f);
    shadowBounds.setWidth(PitchToolHandles::buttonGroupWidth);
  }

  return shadowBounds;
}

juce::Rectangle<float>
PianoRollComponent::getPreviewButtonBounds(const Note &note) const
{
  const auto shadowBounds = getNoteHoverShadowBounds(note);

  constexpr float buttonGap = 4.0f;
  const float buttonWidth = static_cast<float>(previewButtonWidth);
  const float buttonHeight = static_cast<float>(previewButtonHeight);
  const float buttonGroupWidth =
      buttonWidth + buttonGap + static_cast<float>(resetButtonWidth);
  const float buttonX = shadowBounds.getCentreX() - buttonGroupWidth * 0.5f;
  const float buttonY = shadowBounds.getBottom() + 7.0f +
      (editMode == EditMode::Select ? PitchToolHandles::buttonHeight + 10.0f : 0.0f);
  return {buttonX, buttonY, buttonWidth, buttonHeight};
}

juce::Rectangle<float>
PianoRollComponent::getPreviewHoverBounds(const Note &note) const
{
  const float noteStartTime =
      static_cast<float>(framesToSeconds(note.getStartFrame()));
  const float noteEndTime =
      static_cast<float>(framesToSeconds(note.getEndFrame()));
  const float noteX = noteStartTime * pixelsPerSecond;
  const float noteW = std::max((noteEndTime - noteStartTime) * pixelsPerSecond,
                               4.0f);
  const float noteY = midiToY(note.getAdjustedMidiNote());
  const juce::Rectangle<float> noteBounds(noteX, noteY, noteW,
                                          pixelsPerSemitone);

  return noteBounds.getUnion(getPreviewButtonBounds(note))
      .getUnion(getResetButtonBounds(note))
      .expanded(4.0f);
}

juce::Rectangle<float>
PianoRollComponent::getResetButtonBounds(const Note &note) const
{
  constexpr float buttonGap = 4.0f;
  auto bounds = getPreviewButtonBounds(note);
  return {bounds.getRight() + buttonGap, bounds.getY(),
          static_cast<float>(resetButtonWidth),
          static_cast<float>(resetButtonHeight)};
}

juce::Rectangle<int>
PianoRollComponent::getPreviewButtonLocalBounds(const Note &note) const
{
  const auto bounds =
      getPreviewButtonBounds(note)
          .translated(static_cast<float>(pianoKeysWidth) -
                          static_cast<float>(scrollX),
                      static_cast<float>(headerHeight) -
                          static_cast<float>(scrollY));
  return {static_cast<int>(std::round(bounds.getX())),
          static_cast<int>(std::round(bounds.getY())), previewButtonWidth,
          previewButtonHeight};
}

juce::Rectangle<int>
PianoRollComponent::getResetButtonLocalBounds(const Note &note) const
{
  const auto bounds =
      getResetButtonBounds(note)
          .translated(static_cast<float>(pianoKeysWidth) -
                          static_cast<float>(scrollX),
                      static_cast<float>(headerHeight) -
                          static_cast<float>(scrollY));
  return {static_cast<int>(std::round(bounds.getX())),
          static_cast<int>(std::round(bounds.getY())), resetButtonWidth,
          resetButtonHeight};
}

void PianoRollComponent::updatePreviewButtonBounds()
{
  const bool isChangingPitch =
      (selectHandler_ && (selectHandler_->isSingleNoteDragging() ||
                          selectHandler_->getIsDeltaScaleDragging() ||
                          selectHandler_->getIsDeltaOffsetDragging())) ||
      (pitchToolController && pitchToolController->isDragging()) ||
      (pitchEditor && (pitchEditor->isDraggingNote() ||
                       pitchEditor->isDraggingMultiNotes()));

  const bool supportsNoteHoverControls =
      editMode == EditMode::Select || editMode == EditMode::Split ||
      editMode == EditMode::Timing;
  if (!hoveredNote || !project || !supportsNoteHoverControls ||
      isChangingPitch)
  {
    previewButton.setVisible(false);
    resetButton.setVisible(false);
    return;
  }

  const auto previewBounds = getPreviewButtonLocalBounds(*hoveredNote);
  const auto resetBounds = getResetButtonLocalBounds(*hoveredNote);
  const int horizontalScrollBarSize =
      showHorizontalScrollBar ? APP_SCROLLBAR_THICKNESS : 0;
  constexpr int verticalScrollBarSize = APP_SCROLLBAR_THICKNESS;
  const auto mainArea = getLocalBounds()
                            .withTrimmedLeft(pianoKeysWidth)
                            .withTrimmedTop(headerHeight)
                            .withTrimmedBottom(horizontalScrollBarSize)
                            .withTrimmedRight(verticalScrollBarSize);

  previewButton.setBounds(previewBounds);
  previewButton.setVisible(previewBounds.intersects(mainArea));
  resetButton.setBounds(resetBounds);
  resetButton.setVisible(resetBounds.intersects(mainArea));
}

Note *PianoRollComponent::findPreviewButtonNoteAt(float x, float y) const
{
  if (!hoveredNote || !project)
    return nullptr;

  return (getPreviewButtonBounds(*hoveredNote).contains(x, y) ||
          getResetButtonBounds(*hoveredNote).contains(x, y))
             ? hoveredNote
             : nullptr;
}

std::vector<Note *> PianoRollComponent::getResetTargetNotes(Note &note) const
{
  std::vector<Note *> targets{&note};
  if (!project || editMode != EditMode::Select)
    return targets;

  std::vector<Note *> selected;
  for (auto &candidate : project->getNotes())
    if (!candidate.isRest() && candidate.isSelected())
      selected.push_back(&candidate);
  return note.isSelected() && selected.size() > 1 ? selected : targets;
}

void PianoRollComponent::resetNoteEdits(Note &note, NoteRestoreMode mode)
{
  if (!project)
    return;

  const auto targets = getResetTargetNotes(note);
  if (mode == NoteRestoreMode::Amplitude)
  {
    std::vector<float> before, after(targets.size(), 0.0f);
    for (const auto* target : targets) before.push_back(target->getVolumeDb());
    if (before == after) return;
    applyRestoreAction(std::make_unique<AmplitudeAction>(
        *project, targets, before, after, pitchToolController->onAmplitudeEdited,
        "Restore Amplitude"));
    return;
  }

  const bool all = mode == NoteRestoreMode::All;
  applyRestoreAction(std::make_unique<ResetNoteEditsAction>(
      *project, targets, all || mode == NoteRestoreMode::Pitch,
      all || mode == NoteRestoreMode::Formant, all,
      all ? "Restore All Edits" : mode == NoteRestoreMode::Formant
          ? "Restore Formant" : "Restore Pitch",
      all ? createResetTimingAction(note) : nullptr));
}

void PianoRollComponent::applyRestoreAction(std::unique_ptr<UndoableAction> action)
{
  if (!action)
    return;

  // Same order as MainComponent::undo(): drop any transient drawing preview,
  // change the data, record history, then let the shared post-history path
  // refresh the UI and trigger the render.
  cancelPitchDrawingPreview();
  const bool requiresResynthesis = action->requiresAudioResynthesis();
  LOG("history: apply " + action->getName());
  action->redo();
  if (undoManager)
    undoManager->addAction(std::move(action));

  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();

  if (onHistoryActionApplied)
  {
    onHistoryActionApplied(requiresResynthesis);
    return;
  }

  // No host wiring (e.g. a bare component): keep the old behaviour.
  invalidateBasePitchCache();
  if (onPitchEdited)
    onPitchEdited();
  if (requiresResynthesis && onPitchEditFinished)
    onPitchEditFinished();
  repaint();
}

void PianoRollComponent::refreshLocalisedText()
{
  previewButton.setTooltip(TR("tooltip.context_audition"));
  resetButton.setTooltip(TR("tooltip.restore_original"));
}

void PianoRollComponent::showSibilantMenu(juce::Point<int> screenPosition,
                                          double clickFrame, Note *clicked)
{
  if (!project)
    return;
  // The selection, else the note clicked.
  std::vector<Note *> targets;
  if (ownTrackEditable)
    for (auto &note : project->getNotes())
      if (!note.isRest() && note.isSelected())
        targets.push_back(&note);
  if (targets.empty() && clicked != nullptr && ownTrackEditable)
    targets.push_back(clicked);

  juce::PopupMenu menu;
  menu.setLookAndFeel(&pitchPopupMenu::getLookAndFeel());
  const auto addItem = [&menu](int id, const juce::String &label, bool enabled, bool ticked)
  {
    juce::PopupMenu::Item item(label);
    item.itemID = id;
    item.isEnabled = enabled;
    item.customComponent =
        std::make_unique<pitchPopupMenu::MenuItemComponent>(label, ticked).release();
    menu.addItem(std::move(item));
  };
  const bool any = !targets.empty();
  const bool inClicked = clicked != nullptr && std::find(targets.begin(), targets.end(), clicked) != targets.end();
  addItem(1, "Show Sibilants", true, ViewPrefs::showSibilants());
  menu.addSeparator();
  addItem(2, "Find Sibilants Again", any, false);
  addItem(3, "Sibilant from Note Start to Here", inClicked, false);
  addItem(4, "Sibilant from Here to Note End", inClicked, false);
  addItem(5, "Remove Sibilants (pitch-shift them too)", any, false);
  menu.addSeparator();
  addItem(10, "Sibilant Balance: Mute Sibilants (-100%)", any, false);
  addItem(11, "Sibilant Balance: Softer Sibilants (-50%)", any, false);
  addItem(12, "Sibilant Balance: As Sung (0%)", any, false);
  addItem(13, "Sibilant Balance: Softer Pitched Part (+50%)", any, false);
  addItem(14, "Sibilant Balance: Sibilants Only (+100%)", any, false);

  juce::Component::SafePointer<PianoRollComponent> safeThis(this);
  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetScreenArea({screenPosition.x, screenPosition.y, 1, 1}),
      [safeThis, targets, clicked, clickFrame](int result)
  {
    if (safeThis == nullptr || !safeThis->project || result == 0)
      return;
    auto &self = *safeThis;
    if (result == 1)
    {
      ViewPrefs::setShowSibilants(!ViewPrefs::showSibilants());
      self.repaint();
      return;
    }
    // The notes must still exist (the menu was open meanwhile).
    auto &notes = self.project->getNotes();
    auto alive = [&notes](const Note *n)
    {
      return std::any_of(notes.begin(), notes.end(),
                         [n](const Note &candidate) { return &candidate == n; });
    };
    std::vector<Note *> live;
    for (auto *n : targets)
      if (alive(n))
        live.push_back(n);
    if (live.empty())
      return;

    if (result >= 10 && result <= 14)
    {
      const float values[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
      std::vector<float> before, after;
      for (auto *n : live)
      {
        before.push_back(n->getSibilantBalance());
        after.push_back(values[result - 10]);
      }
      self.applyRestoreAction(std::make_unique<SibilantBalanceAction>(
          *self.project, live, before, after,
          self.pitchToolController ? self.pitchToolController->onAmplitudeEdited
                                   : std::function<void()>{}));
      self.repaint();
      return;
    }

    std::vector<SibilantState> before, after;
    juce::String name = "Sibilant Range";
    const auto &audio = self.project->getAudioData();
    for (auto *n : live)
    {
      before.push_back(SibilantState::of(*n));
      SibilantState state = SibilantState::of(*n);
      if (result == 2)
      {
        const auto range = SibilantDetector::detect(audio, n->getSrcStartFrame(), n->getSrcEndFrame());
        state = {range.head, range.tail, false, n->getSibilantBalance()};
        name = "Find Sibilants";
      }
      else if (result == 5)
      {
        state = {0, 0, true, n->getSibilantBalance()};
        name = "Remove Sibilants";
      }
      else if ((result == 3 || result == 4) && n == clicked)
      {
        // The clicked point, in the note's source frames.
        const double src =
            NoteTimeMap::sourceAt(NoteTimeMap::knots(*n), clickFrame);
        const int length = n->getSrcDurationFrames();
        const int at = juce::jlimit(0, length,
                                    static_cast<int>(std::lround(src - n->getSrcStartFrame())));
        const int head = std::max(0, n->getSibilantHead());
        const int tail = std::max(0, n->getSibilantTail());
        if (result == 3)
          state = {at, std::min(tail, length - at), true, n->getSibilantBalance()};
        else
          state = {std::min(head, at), length - at, true, n->getSibilantBalance()};
      }
      after.push_back(state);
    }
    self.applyRestoreAction(std::make_unique<SibilantRangeAction>(
        *self.project, live, before, after, name));
    self.repaint();
  });
}

void PianoRollComponent::showResetMenu(Note &note)
{
  juce::PopupMenu menu;
  menu.setLookAndFeel(&pitchPopupMenu::getLookAndFeel());
  const auto targets = getResetTargetNotes(note);
  const auto anyTarget = [&targets](bool (*predicate)(const Note&))
  {
    return std::any_of(targets.begin(), targets.end(),
                       [predicate](const Note* n) { return n && predicate(*n); });
  };
  const bool pitchChanged = anyTarget(hasPitchEdits);
  const bool formantChanged = anyTarget(hasFormantEdits);
  const bool amplitudeChanged = anyTarget(hasAmplitudeEdits);
  const bool timingChanged = anyTarget(hasTimingEdits);
  const bool anyChanged =
      pitchChanged || formantChanged || amplitudeChanged || timingChanged;

  const auto addItem = [&menu](int id, const juce::String& label, bool enabled)
  {
    juce::PopupMenu::Item item(label);
    item.itemID = id;
    item.isEnabled = enabled;
    item.customComponent = std::make_unique<pitchPopupMenu::MenuItemComponent>(
        label, false, std::function<void()>{}, false).release();
    menu.addItem(std::move(item));
  };
  addItem(1, TR("restore.pitch"), pitchChanged);
  addItem(3, TR("restore.formant"), formantChanged);
  addItem(4, TR("restore.amplitude"), amplitudeChanged);
  addItem(2, TR("restore.timing"), timingChanged);
  menu.addSeparator();
  addItem(5, TR("restore.all"), anyChanged);
  // "Undo All Changes" for the whole track, not only the selection.
  addItem(6, TR("restore.whole_track"), true);

  juce::Component::SafePointer<PianoRollComponent> safeThis(this);
  Note* notePtr = &note;
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&resetButton),
                     [safeThis, notePtr](int result)
  {
    if (safeThis == nullptr || !safeThis->project || result == 0)
      return;
    auto& notes = safeThis->project->getNotes();
    if (std::none_of(notes.begin(), notes.end(),
                     [notePtr](const Note& candidate)
                     { return &candidate == notePtr; }))
      return;
    if (result == 1)
      safeThis->resetNoteEdits(*notePtr);
    else if (result == 2)
      safeThis->resetNoteTiming(*notePtr);
    else if (result == 3)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::Formant);
    else if (result == 4)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::Amplitude);
    else if (result == 5)
      safeThis->resetNoteEdits(*notePtr, NoteRestoreMode::All);
    else if (result == 6)
      safeThis->restoreWholeTrack();
  });
}

std::unique_ptr<UndoableAction> PianoRollComponent::createResetTimingAction(Note &note)
{
  if (!project)
    return {};

  auto& notes = project->getNotes();
  const auto targets = getResetTargetNotes(note);
  std::unordered_set<Note*> targetSet(targets.begin(), targets.end());
  std::unordered_map<Note*, NoteTimingState> desired;
  auto ensureDesired = [&](Note* candidate) -> NoteTimingState&
  {
    auto [it, inserted] = desired.emplace(
        candidate, NoteTimingState::capture(*candidate));
    juce::ignoreUnused(inserted);
    return it->second;
  };

  // Selected notes restore their own immutable ranges. Shared-boundary
  // companions outside the selection follow the restored selected edge.
  for (auto* target : targets)
  {
    if (!target)
      continue;

    Note* previous = nullptr;
    Note* next = nullptr;
    for (auto& candidate : notes)
    {
      if (&candidate == target || candidate.isRest())
        continue;
      if (candidate.getEndFrame() <= target->getStartFrame() &&
          (!previous || candidate.getEndFrame() > previous->getEndFrame()))
        previous = &candidate;
      if (candidate.getStartFrame() >= target->getEndFrame() &&
          (!next || candidate.getStartFrame() < next->getStartFrame()))
        next = &candidate;
    }

    const bool sharesLeftBoundary =
        previous && previous->getEndFrame() == target->getStartFrame();
    const bool sharesRightBoundary =
        next && next->getStartFrame() == target->getEndFrame();

    auto& targetState = ensureDesired(target);
    targetState.startFrame = target->getSrcStartFrame();
    targetState.endFrame = target->getSrcEndFrame();
    if (sharesLeftBoundary && targetSet.count(previous) == 0)
      ensureDesired(previous).endFrame = targetState.startFrame;
    if (sharesRightBoundary && targetSet.count(next) == 0)
      ensureDesired(next).startFrame = targetState.endFrame;
  }

  std::vector<NoteTimingState> before;
  std::vector<NoteTimingState> after;
  before.reserve(desired.size());
  after.reserve(desired.size());
  for (const auto& [affectedNote, targetState] : desired)
  {
    before.push_back(NoteTimingState::capture(*affectedNote));
    after.push_back(targetState);
  }

  return std::make_unique<TimingAction>(
      *project, std::move(before), std::move(after), "Restore Timing");
}

void PianoRollComponent::resetNoteTiming(Note &note)
{
  applyRestoreAction(createResetTimingAction(note));
}

void PianoRollComponent::triggerPreviewForNote(Note &note)
{
  if (!project)
    return;

  auto &notes = project->getNotes();
  auto it = std::find_if(notes.begin(), notes.end(),
                         [&note](const Note &candidate)
                         { return &candidate == &note; });
  if (it == notes.end())
    return;

  const auto sourceRegion = timingRegions::getSourceRegion(*project, note);

  auto isInSameRegion = [&](const Note &candidate)
  {
    return timingRegions::belongsTo(candidate, sourceRegion);
  };

  Note *startNote = &note;
  Note *endNote = &note;

  for (auto prev = it; prev != notes.begin();)
  {
    --prev;
    if (!prev->isRest() && isInSameRegion(*prev))
    {
      startNote = &(*prev);
      break;
    }
  }

  for (auto next = std::next(it); next != notes.end(); ++next)
  {
    if (!next->isRest() && isInSameRegion(*next))
    {
      endNote = &(*next);
      break;
    }
  }

  const int startFrame = std::max(0, startNote->getStartFrame());
  const int endFrame = std::max(startFrame, endNote->getEndFrame());
  setPreviewPlaybackState(endFrame > startFrame, startFrame, endFrame);

  if (endFrame > startFrame && onPreviewRegionRequested)
    onPreviewRegionRequested(startFrame, endFrame);
}

void PianoRollComponent::setPreviewPlaybackState(bool active, int startFrame,
                                                 int endFrame)
{
  previewPlaybackActive = active && endFrame > startFrame;
  previewPlaybackStartFrame = previewPlaybackActive ? startFrame : 0;
  previewPlaybackEndFrame = previewPlaybackActive ? endFrame : 0;
  previewPlaybackCurrentTime =
      previewPlaybackActive ? framesToSeconds(startFrame) : 0.0;
  repaint();
}

void PianoRollComponent::setPreviewPlaybackPosition(double timeSeconds)
{
  if (!previewPlaybackActive)
    return;

  if (std::abs(previewPlaybackCurrentTime - timeSeconds) < 0.0001)
    return;

  previewPlaybackCurrentTime = timeSeconds;
  repaint();
}

void PianoRollComponent::setHoveredNote(Note *note)
{
  if (hoveredNote == note)
    return;

  hoveredNote = note;
  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::updateScrollBars()
{
  if (project)
  {
    float totalHeight = (MAX_MIDI_NOTE - MIN_MIDI_NOTE + 1) * pixelsPerSemitone;

    int visibleWidth = getVisibleContentWidth();
    int visibleHeight = getVisibleContentHeight();
    scrollX = clampScrollX(scrollX);
    coordMapper->setScrollX(scrollX);

    horizontalScrollBar.setRangeLimits(
        getTimelineStart() * pixelsPerSecond,
        std::max(getTimelineStart() * pixelsPerSecond +
                     static_cast<double>(visibleWidth),
                 getTimelineDuration() * pixelsPerSecond));
    horizontalScrollBar.setCurrentRange(scrollX, visibleWidth);

    verticalScrollBar.setRangeLimits(0, totalHeight);
    verticalScrollBar.setCurrentRange(scrollY, visibleHeight);
  }
}

double PianoRollComponent::getTimelineStart() const
{
  // Clip mode: the canvas starts at the modification's own time zero.
  // Track mode: it starts at the host's time zero, which in the active
  // modification's time can be negative (a region placed later in the song
  // than its offset in the take) or positive.
  return trackViewMode ? timelineToProject(0.0) : 0.0;
}

double PianoRollComponent::getTimelineDuration() const
{
  if (trackViewMode)
  {
    // The track's extent: every region on it plus the active region. The
    // rest of the active take is hidden in Track mode, so it does not extend
    // the timeline.
    double end = std::max(liveTimelineEndSeconds, hostTimelineEndSeconds);
    if (regionPreviewsEndSeconds > 0.0)
      end = std::max(end, timelineToProject(regionPreviewsEndSeconds));
    if (const auto span = getTrackModeActiveSpan())
      end = std::max(end, span->second);
    else if (project != nullptr)
      end = std::max(end, static_cast<double>(
                              project->getAudioData().getDuration()));
    end += getTimelineBarSeconds();
    return std::max(end, getTimelineStart() + getTimelineBarSeconds());
  }

  const double projectDuration =
      project ? static_cast<double>(project->getAudioData().getDuration()) : 0.0;
  double timelineDuration =
      std::max({projectDuration, liveTimelineEndSeconds, hostTimelineEndSeconds});

  // ARA region projects contain a playback range; give the editor one full bar
  // of writable-looking canvas after the real project end without extending
  // the underlying audio or playback range.
  if (project != nullptr &&
      !project->getAudioData().playbackRegionRanges.empty())
    timelineDuration += getTimelineBarSeconds();

  return timelineDuration;
}

double PianoRollComponent::getTimelineLength() const
{
  return std::max(0.0, getTimelineDuration() - getTimelineStart());
}

double PianoRollComponent::clampScrollX(double x) const
{
  const double minScrollX = getTimelineStart() * pixelsPerSecond;
  const double maxScrollX =
      std::max(minScrollX, getTimelineDuration() * pixelsPerSecond -
                               static_cast<double>(getVisibleContentWidth()));
  return juce::jlimit(minScrollX, maxScrollX, x);
}

std::optional<std::pair<double, double>>
PianoRollComponent::getTrackModeActiveSpan() const
{
  if (!trackViewMode || project == nullptr)
    return std::nullopt;
  const auto &ranges = project->getAudioData().playbackRegionRanges;
  if (ranges.empty() || ranges.front().second <= ranges.front().first)
    return std::nullopt;
  return ranges.front();
}

void PianoRollComponent::setTrackViewMode(bool shouldShowTrack)
{
  if (trackViewMode == shouldShowTrack)
    return;

  trackViewMode = shouldShowTrack;
  if (coordMapper)
    coordMapper->setViewStartSeconds(getTimelineStart());
  waveformBackgroundRenderer->setProjectDrawRange(getTrackModeActiveSpan());
  invalidateWaveformCache();
  setHoveredNote(nullptr);
  updateScrollBars();
  updatePreviewButtonBounds();
  repaint();
}

void PianoRollComponent::reapplyBasePitchForNote(Note *note)
{
  if (!note || !project)
    return;

  auto &audioData = project->getAudioData();
  int startFrame = note->getStartFrame();
  int endFrame = note->getEndFrame();
  int f0Size = static_cast<int>(audioData.f0.size());

  // Reapply base + delta from dense curves
  for (int i = startFrame; i < endFrame && i < f0Size; ++i)
  {
    float base = (i < static_cast<int>(audioData.basePitch.size()))
                     ? audioData.basePitch[static_cast<size_t>(i)]
                     : 0.0f;
    float delta = (i < static_cast<int>(audioData.deltaPitch.size()))
                      ? audioData.deltaPitch[static_cast<size_t>(i)]
                      : 0.0f;
    audioData.f0[i] = midiToFreq(base + delta);
  }

  // Always set F0 dirty range for synthesis (needed for undo/redo to trigger
  // resynthesis)
  project->markNoteEditDirtyRange(startFrame, endFrame);

  // Trigger repaint
  repaint();
}

void PianoRollComponent::cancelPitchDrawingPreview()
{
  if (pitchDrawingHandler_ && pitchDrawingHandler_->isActive())
    pitchDrawingHandler_->cancel();
}

void PianoRollComponent::updatePitchDrawingConfirmationPopup()
{
  if (!pitchDrawingConfirmationPanel)
    return;

  const bool shouldShow = editMode == EditMode::PitchDrawing && pitchDrawingHandler_ &&
                          pitchDrawingHandler_->hasAnchors();
  pitchDrawingConfirmationPanel->setVisible(shouldShow);
  if (!shouldShow)
    return;

  constexpr int popupWidth = 213;
  constexpr int popupHeight = 30;
  const int canvasWidth = getVisibleContentWidth();
  const int popupX = pianoKeysWidth + std::max(0, (canvasWidth - popupWidth) / 2);
  pitchDrawingConfirmationPanel->setBounds(popupX, headerHeight + 10,
                                     std::min(popupWidth, canvasWidth),
                                     popupHeight);
  pitchDrawingConfirmationPanel->toFront(false);
}

void PianoRollComponent::drawSelectionRect(juce::Graphics &g)
{
  if (!boxSelector || !boxSelector->isSelecting())
    return;

  auto rect = boxSelector->getSelectionRect();

  // Draw semi-transparent fill
  g.setColour(juce::Colours::white.withAlpha(0.05f));
  g.fillRect(rect);

  // Draw border
  g.setColour(juce::Colours::white.withAlpha(0.25f));
  g.drawRect(rect, 1.0f);
}

bool PianoRollComponent::setSelectedNotesProperty(
    NoteProperty property, const std::function<float(const Note &)> &valueFor)
{
  if (!project)
    return false;
  std::vector<Note *> notes;
  for (auto *note : getSelectedNotes())
    if (note != nullptr && !note->isRest())
      notes.push_back(note);
  if (notes.empty())
    return false;

  std::vector<float> before, after;
  for (auto *note : notes)
  {
    const float value = valueFor(*note);
    switch (property)
    {
    case NoteProperty::Pitch: before.push_back(note->getMidiNote()); break;
    case NoteProperty::Modulation: before.push_back(note->getVibrato()); break;
    case NoteProperty::Drift: before.push_back(note->getPitchDrift()); break;
    case NoteProperty::Formant: before.push_back(note->getFormantShift()); break;
    case NoteProperty::Amplitude: before.push_back(note->getVolumeDb()); break;
    }
    after.push_back(std::isfinite(value) ? value : before.back());
  }
  if (property == NoteProperty::Pitch)
    for (size_t i = 0; i < notes.size(); ++i)
      before[i] = notes[i]->getMidiNote();

  bool changed = false;
  for (size_t i = 0; i < notes.size(); ++i)
    changed = changed || std::abs(after[i] - (property == NoteProperty::Pitch
                                                  ? before[i] + notes[i]->getPitchOffset()
                                                  : before[i])) > 1.0e-4f;
  if (!changed)
    return false;

  // Level is a gain edit on the composite, like the amplitude handle.
  if (property == NoteProperty::Amplitude)
  {
    applyRestoreAction(std::make_unique<AmplitudeAction>(
        *project, notes, before, after,
        pitchToolController ? pitchToolController->onAmplitudeEdited : std::function<void()>{},
        "Note Inspector: Amplitude"));
    repaint();
    return true;
  }

  auto *owner = project;
  const auto rebuild = [owner](const std::vector<Note *> &edited) {
    PitchCurveProcessor::rebuildBaseFromNotes(*owner);
    int minFrame = std::numeric_limits<int>::max(), maxFrame = std::numeric_limits<int>::min();
    for (auto *n : edited)
    {
      n->markSynthDirty();
      minFrame = std::min(minFrame, n->getStartFrame());
      maxFrame = std::max(maxFrame, n->getEndFrame());
    }
    owner->markNoteEditDirtyRange(minFrame, maxFrame);
  };

  if (property == NoteProperty::Pitch)
  {
    std::vector<float> offsets;
    for (auto *n : notes)
      offsets.push_back(n->getPitchOffset());
    if (undoManager)
      undoManager->addAction(std::make_unique<MultiNoteSnapToSemitoneAction>(
          notes, before, offsets, after, rebuild));
    for (size_t i = 0; i < notes.size(); ++i)
    {
      notes[i]->setMidiNote(after[i]);
      notes[i]->setPitchOffset(0.0f);
      notes[i]->markDirty();
    }
  }
  else
  {
    const auto setter = property == NoteProperty::Modulation ? &Note::setVibrato
                        : property == NoteProperty::Drift    ? &Note::setPitchDrift
                                                             : &Note::setFormantShift;
    const juce::String name = property == NoteProperty::Modulation ? "Note Inspector: Modulation"
                              : property == NoteProperty::Drift    ? "Note Inspector: Drift"
                                                                   : "Note Inspector: Formant";
    if (undoManager)
      undoManager->addAction(std::make_unique<MultiNoteFloatPropertyAction>(
          notes, before, after, setter, name, [rebuild, notes] { rebuild(notes); }));
    for (size_t i = 0; i < notes.size(); ++i)
    {
      (notes[i]->*setter)(after[i]);
      notes[i]->markDirty();
    }
  }
  rebuild(notes);
  invalidateBasePitchCache();
  updatePitchToolHandlesFromSelection();
  if (onPitchEdited)
    onPitchEdited();
  if (onPitchEditFinished)
    onPitchEditFinished();
  repaint();
  return true;
}

bool PianoRollComponent::toggleMuteSelectedNotes()
{
  bool allMuted = true, any = false;
  for (auto *note : getSelectedNotes())
    if (note != nullptr && !note->isRest())
    {
      any = true;
      allMuted = allMuted && note->getVolumeDb() <= NoteGainCurve::kDeletedNoteDb + 0.5f;
    }
  if (!any)
    return false;
  return setSelectedNotesProperty(NoteProperty::Amplitude, [allMuted](const Note &) {
    return allMuted ? 0.0f : NoteGainCurve::kDeletedNoteDb;
  });
}

namespace {
std::pair<int, int> selectionKey(const Note &note)
{
  return {note.getSrcStartFrame(), note.getSrcEndFrame()};
}
} // namespace

void PianoRollComponent::rememberSelection()
{
  if (!project)
    return;
  lastSelectionKeys.clear();
  for (const auto &note : project->getNotes())
    if (note.isSelected())
      lastSelectionKeys.push_back(selectionKey(note));
}

void PianoRollComponent::restoreLastSelection()
{
  if (!project)
    return;
  std::vector<std::pair<int, int>> current;
  for (const auto &note : project->getNotes())
    if (note.isSelected())
      current.push_back(selectionKey(note));
  for (auto &note : project->getNotes())
    note.setSelected(!note.isRest() &&
                     std::find(lastSelectionKeys.begin(), lastSelectionKeys.end(),
                               selectionKey(note)) != lastSelectionKeys.end());
  lastSelectionKeys = std::move(current); // pressing again swaps back
  updatePitchToolHandlesFromSelection();
  repaint();
}

void PianoRollComponent::invertSelection()
{
  if (!project)
    return;
  rememberSelection();
  for (auto &note : project->getNotes())
    note.setSelected(!note.isRest() && !note.isSelected());
  updatePitchToolHandlesFromSelection();
  repaint();
}

void PianoRollComponent::selectSpecial(int kind)
{
  if (!project)
    return;
  std::vector<int> pitches;
  int firstStart = std::numeric_limits<int>::max();
  for (const auto &note : project->getNotes())
    if (note.isSelected() && !note.isRest())
    {
      pitches.push_back(static_cast<int>(std::lround(note.getAdjustedMidiNote())));
      firstStart = std::min(firstStart, note.getStartFrame());
    }
  if (pitches.empty())
    return;
  rememberSelection();
  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    const int pitch = static_cast<int>(std::lround(note.getAdjustedMidiNote()));
    bool match = false;
    if (kind == 2)
      match = note.getStartFrame() >= firstStart;
    else
      for (int p : pitches)
        match = match || (kind == 1 ? ((pitch - p) % 12 + 12) % 12 == 0 : pitch == p);
    if (match)
      note.setSelected(true);
  }
  updatePitchToolHandlesFromSelection();
  repaint();
}

void PianoRollComponent::selectNotesOfPitch(int midiNote, bool allOctaves, bool additive)
{
  if (!project)
    return;
  rememberSelection();
  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    const int pitch = static_cast<int>(std::lround(note.getAdjustedMidiNote()));
    const bool match = allOctaves ? ((pitch - midiNote) % 12 + 12) % 12 == 0 : pitch == midiNote;
    if (match)
      note.setSelected(true);
    else if (!additive)
      note.setSelected(false);
  }
  updatePitchToolHandlesFromSelection();
  repaint();
}

void PianoRollComponent::restoreWholeTrack()
{
  if (!project)
    return;
  Note *first = nullptr;
  rememberSelection();
  for (auto &note : project->getNotes())
    if (!note.isRest())
    {
      note.setSelected(true);
      if (first == nullptr)
        first = &note;
    }
  if (first != nullptr)
  {
    resetNoteEdits(*first, NoteRestoreMode::All);
    resetNoteEdits(*first, NoteRestoreMode::Amplitude);
  }
  restoreLastSelection();
}

int PianoRollComponent::keyMarkerAt(int componentX) const
{
  if (!project)
    return -1;
  const auto &markers = project->getMacroParameters()->keyMarkers;
  for (int i = 0; i < static_cast<int>(markers.size()); ++i)
  {
    const float x = static_cast<float>(pianoKeysWidth) +
                    timeToX(timelineToProject(markers[static_cast<size_t>(i)].seconds)) -
                    static_cast<float>(scrollX);
    if (std::abs(x - static_cast<float>(componentX)) <= 6.0f)
      return i;
  }
  return -1;
}

void PianoRollComponent::addKeyMarker(double hostSeconds, int root, ScaleMode mode)
{
  if (!project)
    return;
  auto &markers = project->getMacroParameters()->keyMarkers;
  // One key change per place: a new one there replaces the old.
  markers.erase(std::remove_if(markers.begin(), markers.end(),
                               [hostSeconds](const KeyMarker &m) {
                                 return std::abs(m.seconds - hostSeconds) < 0.01;
                               }),
                markers.end());
  markers.push_back({std::max(0.0, hostSeconds), juce::jlimit(0, 11, root), mode});
  std::sort(markers.begin(), markers.end(),
            [](const KeyMarker &a, const KeyMarker &b) { return a.seconds < b.seconds; });
  keyMarkersChanged();
}

void PianoRollComponent::removeKeyMarker(int index)
{
  if (!project)
    return;
  auto &markers = project->getMacroParameters()->keyMarkers;
  if (index < 0 || index >= static_cast<int>(markers.size()))
    return;
  markers.erase(markers.begin() + index);
  keyMarkersChanged();
}

void PianoRollComponent::keyMarkersChanged()
{
  if (project)
    project->setModified(true);
  inactiveLayer = {};
  repaint();
  if (onKeyMarkersChanged)
    onKeyMarkersChanged();
}

void PianoRollComponent::drawKeyMarkers(juce::Graphics &g)
{
  if (!project)
    return;
  const auto &markers = project->getMacroParameters()->keyMarkers;
  if (markers.empty())
    return;
  juce::Graphics::ScopedSaveState state(g);
  g.reduceClipRegion(pianoKeysWidth, 0, getWidth() - pianoKeysWidth, timelineHeight);
  g.setFont(AppFont::getFont(11.0f));
  const auto colour = juce::Colour(0xFFE8A33Cu);
  for (const auto &marker : markers)
  {
    const float x = static_cast<float>(pianoKeysWidth) +
                    timeToX(timelineToProject(marker.seconds)) - static_cast<float>(scrollX);
    juce::Path flag;
    flag.addTriangle(x, 0.0f, x + 7.0f, 0.0f, x, 7.0f);
    g.setColour(colour);
    g.fillPath(flag);
    g.fillRect(x, 0.0f, 1.0f, static_cast<float>(timelineHeight));
    g.drawText(ScaleNames::keyName(marker.root, marker.mode),
               juce::Rectangle<float>(x + 4.0f, 2.0f, 140.0f,
                                      static_cast<float>(timelineHeight - 3)),
               juce::Justification::centredLeft, false);
  }
}

void PianoRollComponent::setOwnTrackEditable(bool editable)
{
  if (ownTrackEditable == editable)
    return;
  ownTrackEditable = editable;
  if (!editable && project)
    project->deselectAllNotes(); // nothing of it stays selected or editable
  setHoveredNote(nullptr);
  updatePitchToolHandlesFromSelection();
  updatePreviewButtonBounds();
  repaint();
}

// ---------------------------------------------------------------------------
// Zoom history and "fit" zooms (navigation)
// ---------------------------------------------------------------------------

void PianoRollComponent::rememberZoom()
{
  zoomHistory.push_back({pixelsPerSecond, pixelsPerSemitone, scrollX, scrollY});
  if (zoomHistory.size() > 30)
    zoomHistory.erase(zoomHistory.begin());
  lastZoomRememberMs = juce::Time::getMillisecondCounterHiRes();
}

void PianoRollComponent::rememberZoomThrottled()
{
  // One step of history per burst of wheel or pinch zooming.
  if (juce::Time::getMillisecondCounterHiRes() - lastZoomRememberMs > 700.0)
    rememberZoom();
  else
    lastZoomRememberMs = juce::Time::getMillisecondCounterHiRes();
}

void PianoRollComponent::applyZoomState(float pps, float ppsSemitone, double newScrollX,
                                        double newScrollY)
{
  pixelsPerSecond = juce::jlimit(MIN_PIXELS_PER_SECOND, MAX_PIXELS_PER_SECOND, pps);
  coordMapper->setPixelsPerSecond(pixelsPerSecond);
  pixelsPerSemitone = juce::jlimit(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, ppsSemitone);
  coordMapper->setPixelsPerSemitone(pixelsPerSemitone);
  // Set the positions directly: a scroll bar reports its move later
  // (asynchronously), which would put the old position back.
  scrollX = clampScrollX(newScrollX);
  coordMapper->setScrollX(scrollX);
  updateScrollBars();
  setScrollY(newScrollY);
  updateScrollBars();
  repaint();
  if (onZoomChanged)
    onZoomChanged(pixelsPerSecond);
}

void PianoRollComponent::restorePreviousZoom()
{
  if (zoomHistory.empty())
    return;
  const auto state = zoomHistory.back();
  zoomHistory.pop_back();
  applyZoomState(state.pps, state.ppsSemitone, state.scrollX, state.scrollY);
}

void PianoRollComponent::zoomToSpan(double startSeconds, double endSeconds, float lowMidi,
                                    float highMidi)
{
  const int width = getVisibleContentWidth();
  const int height = getVisibleContentHeight();
  if (width <= 0 || height <= 0 || endSeconds <= startSeconds)
    return;
  rememberZoom();
  const float pps = static_cast<float>(width / ((endSeconds - startSeconds) * 1.10));
  const float rows = std::max(4.0f, highMidi - lowMidi + 1.0f) * 1.2f + 2.0f;
  const float ppsSemitone = static_cast<float>(height) / rows;
  const float clampedSemitone = juce::jlimit(MIN_PIXELS_PER_SEMITONE, MAX_PIXELS_PER_SEMITONE, ppsSemitone);
  const double centreMidi = (lowMidi + highMidi) * 0.5;
  const double newScrollX = startSeconds * pps - width * 0.05;
  const double newScrollY =
      (MAX_MIDI_NOTE - centreMidi + 0.5) * clampedSemitone - height * 0.5;
  applyZoomState(pps, clampedSemitone, newScrollX, newScrollY);
}

void PianoRollComponent::zoomToNotes(Note *clicked)
{
  if (!project)
    return;
  std::vector<const Note *> notes;
  if (clicked != nullptr && clicked->isSelected())
    for (const auto &note : project->getNotes())
      if (note.isSelected() && !note.isRest())
        notes.push_back(&note);
  if (notes.empty() && clicked != nullptr)
    notes.push_back(clicked);
  if (notes.empty())
    return;
  double start = 1.0e9, end = -1.0e9;
  float low = 1000.0f, high = -1000.0f;
  for (const auto *note : notes)
  {
    start = std::min(start, static_cast<double>(framesToSeconds(note->getStartFrame())));
    end = std::max(end, static_cast<double>(framesToSeconds(note->getEndFrame())));
    low = std::min(low, note->getAdjustedMidiNote());
    high = std::max(high, note->getAdjustedMidiNote());
  }
  zoomToSpan(start, end, low, high);
}

void PianoRollComponent::zoomToFitAll()
{
  // Double-click a scroll bar to see every note; with the cycle on,
  // the whole cycle range.
  if (!project)
    return;
  const auto &loop = project->getLoopRange();
  const bool useLoop = loop.isValid();
  double start = 1.0e9, end = -1.0e9;
  float low = 1000.0f, high = -1000.0f;
  for (const auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;
    const double s = framesToSeconds(note.getStartFrame());
    const double t = framesToSeconds(note.getEndFrame());
    if (useLoop && (t <= loop.startSeconds || s >= loop.endSeconds))
      continue;
    start = std::min(start, s);
    end = std::max(end, t);
    low = std::min(low, note.getAdjustedMidiNote());
    high = std::max(high, note.getAdjustedMidiNote());
  }
  if (useLoop)
  {
    start = loop.startSeconds;
    end = loop.endSeconds;
  }
  if (end <= start)
  {
    start = 0.0;
    end = std::max(1.0, static_cast<double>(project->getAudioData().getDuration()));
  }
  if (high < low)
  {
    low = 48.0f;
    high = 72.0f;
  }
  zoomToSpan(start, end, low, high);
}
