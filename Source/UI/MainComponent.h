#pragma once

#include "../Audio/EditorController.h"
#include "../Utils/UI/Skin.h"
#include "../Audio/ResampledLoopAudition.h"
#include "IMainView.h"
#include "Dialogs/CreditsDialog.h"
#include "../Audio/IO/AudioFileManager.h"
#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Undo/UndoActions.h"
#include "CustomMenuBarLookAndFeel.h"
#include "CustomTitleBar.h"
#include "Commands.h"
#include "Components/AppFont.h"
#include "Main/MenuHandler.h"
#include "Main/SettingsManager.h"
#include "ParameterPanel.h"
#include "TrackListPanel.h"
#include "PianoRollComponent.h"
#include "Dialogs/MacroDialog.h"
#include "Dialogs/QuantizePitchDialog.h"
#include "PianoRollWorkspaceView.h"
#include "SettingsOverlay.h"
#include "ToolbarComponent.h"
#include "Workspace/WorkspaceComponent.h"

#include <atomic>
#include <cstdint>
#include <cmath>
#include <thread>
#include <vector>

class UiBrightnessEffect;

class AnalysisBackdrop : public juce::Component {
public:
  AnalysisBackdrop() {
    setInterceptsMouseClicks(true, true);
  }

  void paint(juce::Graphics &g) override {
    g.fillAll(juce::Colours::black.withAlpha(0.40f));
  }
};

class AnalysisProgressPopup : public juce::Component {
public:
  void setProgress(double newProgress) {
    progress = juce::jlimit(0.0, 1.0, newProgress);
    repaint();
  }

  void paint(juce::Graphics &g) override {
    auto bounds = getLocalBounds().toFloat();
    g.setColour(Skin::colour(0xFF333333u));
    g.fillRoundedRectangle(bounds, 7.0f);

    g.setColour(Skin::colour(0xFFEFEFEFu));
    g.setFont(AppFont::getFont(16.0f));
    g.drawText(TR("progress.analyzing_title"),
               getLocalBounds().withHeight(44).translated(0, 4),
               juce::Justification::centred, false);

    auto bar = juce::Rectangle<float>(29.0f, 53.0f, bounds.getWidth() - 58.0f, 20.0f);
    const auto barRadius = bar.getHeight() * 0.5f;
    g.setColour(Skin::colour(0xFF191717u));
    g.fillRoundedRectangle(bar, barRadius);

    auto fillBounds = bar.reduced(2.0f);
    auto fill = fillBounds.withWidth(fillBounds.getWidth() * static_cast<float>(progress));
    if (fill.getWidth() > 0.0f) {
      g.setColour(Skin::colour(0xFF9A9A9Au));
      g.fillRoundedRectangle(fill, fillBounds.getHeight() * 0.5f);
    }

    g.setColour(Skin::colour(0xFFEFEFEFu));
    g.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    g.drawText(juce::String(static_cast<int>(std::round(progress * 100.0))) + "%",
               bar.toNearestInt(), juce::Justification::centred, false);
  }

private:
  double progress = 0.0;
};

class MainComponent : public juce::Component,
                      public juce::Timer,
                      public juce::KeyListener,
                      public juce::FileDragAndDropTarget,
                      public juce::ApplicationCommandTarget,
                      public IMainView {
public:
  using juce::Component::keyPressed;
  explicit MainComponent(bool enableAudioDevice = true);
  ~MainComponent() override;

  void paint(juce::Graphics &g) override;
  void paintOverChildren(juce::Graphics &g) override;
  void resized() override;

  void timerCallback() override;

  // KeyListener
  bool keyPressed(const juce::KeyPress &key,
                  juce::Component *originatingComponent) override;

  // ApplicationCommandTarget interface
  juce::ApplicationCommandTarget* getNextCommandTarget() override;
  void getAllCommands(juce::Array<juce::CommandID>& commands) override;
  void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
  bool perform(const ApplicationCommandTarget::InvocationInfo& info) override;

  // Mouse handling for window dragging on macOS
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;

  // FileDragAndDropTarget
  bool isInterestedInFileDrag(const juce::StringArray &files) override;
  void filesDropped(const juce::StringArray &files, int x, int y) override;

  // Opens a project or audio file chosen outside the UI (command line, file
  // manager). Ignored in plugin mode and for files that no longer exist.
  void openFileFromPath(const juce::File &file);

  // Plugin mode
  bool isPluginMode() const { return !enableAudioDeviceFlag; }
  juce::Component *getComponent() override { return this; }
  Project *getProject() const override {
    return editorController ? editorController->getProject() : nullptr;
  }
  std::unique_ptr<Project>
  exchangeProject(std::unique_ptr<Project> project) override;
  Vocoder *getVocoder() const override {
    return editorController ? editorController->getVocoder() : nullptr;
  }
  void bindBackendController(EditorController *controller) override;
  void bindUndoManager(PitchUndoManager *manager) override;
  MainViewViewportState getViewportState() const override;
  void restoreViewportState(const MainViewViewportState &state) override;
  bool hasAnalyzedProject() const override;
  void bindRealtimeProcessor(RealtimePitchProcessor &processor) override;
  juce::String serializeProjectJson() const override;
  bool restoreProjectJson(const juce::String &json) override;
  bool restoreProjectSnapshot(const Project &project) override;
  void setStatusMessage(const juce::String &message) override {
    toolbar.setStatusMessage(message);
  }
  void showAnalysisProgress(double progress) override;
  void hideAnalysisProgress() override;
  void finishBackendRender(bool success) override;
  void setTrackPitchMacroTargets(
      std::function<std::vector<TrackPitchMacroTarget>()> callback) override {
    pianoRoll.getTrackEditTargets = callback;
    pianoRollView.getTrackPitchMacroTargets = std::move(callback);
  }
  void setOnProjectDataChanged(std::function<void()> callback) override {
    onProjectDataChanged = std::move(callback);
  }
  void setOnPitchEditFinished(std::function<void()> callback) override {
    onPitchEditFinished = std::move(callback);
  }
  void setOnRequestBackendRender(
      std::function<void(const Project &)> callback) override {
    onRequestBackendRender = std::move(callback);
  }
  void setOnRequestBackendPreview(
      std::function<void(const Project &, int, int)> callback) override {
    onRequestBackendPreview = std::move(callback);
  }
  void setOnStopBackendPreview(std::function<void()> callback) override {
    onStopBackendPreview = std::move(callback);
  }
  void setOnRequestDragAudition(
      std::function<void(const juce::AudioBuffer<float> &, double)> callback) override {
    onRequestDragAudition = std::move(callback);
  }
  void setOnStopDragAudition(std::function<void()> callback) override {
    onStopDragAudition = std::move(callback);
  }
  void setOnRequestHostPlayState(
      std::function<void(bool)> callback) override {
    onRequestHostPlayState = std::move(callback);
  }
  void setOnRequestHostStop(std::function<void()> callback) override {
    onRequestHostStop = std::move(callback);
  }
  void setOnRequestHostSeek(
      std::function<void(double)> callback) override {
    onRequestHostSeek = std::move(callback);
  }
  void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) override {
    onRequestHostLoopRange = std::move(callback);
  }
  void setOnRecordArmChanged(std::function<void(bool)> callback) override {
    onRecordArmChanged = std::move(callback);
  }
  void setRecordControlVisible(bool visible) override {
    toolbar.setRecordControlVisible(visible);
  }
  void updateTrackList(const std::vector<MainViewTrackEntry> &tracks,
                       const juce::String &activeKey) override;
  void setOnTrackSelected(
      std::function<void(const juce::String &)> callback) override {
    trackListPanel.onTrackSelected = std::move(callback);
  }
  void setOnTrackPinChanged(
      std::function<void(const juce::String &, bool)> callback) override {
    trackListPanel.onTrackPinChanged = std::move(callback);
  }
  void setOnTrackTransferToggled(
      std::function<void(const juce::String &, bool)> callback) override {
    trackListPanel.onTrackTransferToggled = std::move(callback);
  }
  void setOnReferenceNoteDoubleClicked(
      std::function<void(const juce::String &)> callback) override {
    pianoRoll.onReferenceNoteDoubleClicked = std::move(callback);
  }
  void setOnTrackEditAdded(
      std::function<void(const juce::String &)> callback) override {
    trackListPanel.onTrackEditAdded = std::move(callback);
  }
  void setTrackGroupCallbacks(TrackGroupCallbacks callbacks) override {
    trackListPanel.groupCallbacks = std::move(callbacks);
  }
  void setCoEditActive(bool active) override {
    pianoRoll.setCoEditInClipMode(active);
  }
  void setEditingMixAvailable(bool available) override {
    trackListPanel.setEditingMixAvailable(available);
  }
  void setOnEditingMixChanged(std::function<void(double)> callback) override {
    trackListPanel.onEditingMixChanged = std::move(callback);
  }
  double getEditingMix() const override { return trackListPanel.getEditingMix(); }
  void notifyProjectEditedExternally(bool render) override {
    pianoRoll.invalidateBasePitchCache();
    pianoRoll.repaint();
    pianoRollView.refreshOverview();
    if (render && pianoRoll.onPitchEditFinished)
      pianoRoll.onPitchEditFinished();
  }
  void setReplaceRangesAvailable(bool available) override {
    pianoRoll.setReplaceRangesAvailable(available);
  }
  void setOnReplaceRangesEdited(
      std::function<void(const std::vector<std::pair<double, double>> &)> callback) override {
    pianoRoll.onReplaceRangesEdited = std::move(callback);
  }
  void setMidiExportTracksProvider(std::function<std::vector<MidiExportTrack>()> provider) override {
    midiExportTracksProvider = std::move(provider);
  }
  bool runEditCommand(const juce::String &command) override {
    if (command == "copy") return pianoRoll.copySelectedNotes(false);
    if (command == "cut") return pianoRoll.copySelectedNotes(true);
    if (command == "paste") return pianoRoll.pasteNotes();
    if (command == "deselect") {
      for (auto &target : pianoRoll.editableTracks())
        for (auto &note : target.project->getNotes()) note.setSelected(false);
      pianoRoll.repaint();
      return true;
    }
    if (command.startsWith("macro:")) {
      if (pianoRoll.onMacroRequested)
        pianoRoll.onMacroRequested(command.fromFirstOccurrenceOf(":", false, false).getIntValue());
      return true;
    }
    if (command == "macrook") { MacroDialog::acceptPopup(); return true; }
    if (command == "replace:notes") { pianoRoll.setReplaceRangesToNotes(); return true; }
    if (command == "invertsel") { pianoRoll.invertSelection(); return true; }
    if (command == "restoresel") { pianoRoll.restoreLastSelection(); return true; }
    if (command.startsWith("selspecial:")) {
      pianoRoll.selectSpecial(command.fromFirstOccurrenceOf(":", false, false).getIntValue());
      return true;
    }
    if (command == "mute") { return pianoRoll.toggleMuteSelectedNotes(); }
    if (command == "restoreall") { pianoRoll.restoreWholeTrack(); return true; }
    // "keymarker:<seconds>:<root 0-11>:<mode int>" adds a key change.
    if (command.startsWith("keymarker:")) {
      const auto parts = juce::StringArray::fromTokens(command, ":", "");
      if (parts.size() == 4)
        pianoRoll.addKeyMarker(parts[1].getDoubleValue(), parts[2].getIntValue(),
                               static_cast<ScaleMode>(parts[3].getIntValue()));
      return true;
    }
    // "customscale": a test scale (C major, E and B 14 cents flat) in use.
    if (command == "customscale") {
      auto &shared = *sharedMacroParameters();
      shared.customScale.name = "Test just";
      shared.customScale.cents[4] = -14.0f;
      shared.customScale.cents[11] = -12.0f;
      shared.projectKeyMode = ScaleMode::Custom;
      refreshSharedKey();
      return true;
    }
    if (command == "credits") { CreditsDialog::showPopup(this); return true; }
    // "track:<row>:<row|edit|ghost|rec>[:ctrl|shift]": a click in the track list.
    if (command.startsWith("track:")) {
      const auto parts = juce::StringArray::fromTokens(command, ":", "");
      if (parts.size() >= 3) {
        const auto mods = parts[3] == "ctrl" ? juce::ModifierKeys(juce::ModifierKeys::commandModifier)
                        : parts[3] == "shift" ? juce::ModifierKeys(juce::ModifierKeys::shiftModifier)
                                              : juce::ModifierKeys();
        const auto point = trackListPanel.pointFor(parts[1].getIntValue(), parts[2]);
        if (point.x >= 0)
          trackListPanel.clickAt(point, mods);
      }
      return true;
    }
    if (command.startsWith("transpose:")) {
      pianoRoll.transposeSelected(command.fromFirstOccurrenceOf(":", false, false).getIntValue());
      return true;
    }
    // "zoomto:<start s>:<end s>:<low midi>:<high midi>"
    if (command.startsWith("zoomto:")) {
      const auto p = juce::StringArray::fromTokens(command, ":", "");
      if (p.size() == 5)
        pianoRoll.zoomToRange(p[1].getDoubleValue(), p[2].getDoubleValue(),
                              p[3].getFloatValue(), p[4].getFloatValue());
      return true;
    }
    // "correctpitch:<percent>:<snap 0|1>": Correct Pitch on the selection.
    if (command.startsWith("correctpitch:")) {
      const auto p = juce::StringArray::fromTokens(command, ":", "");
      pianoRollView.showPitchCenterPopup();
      QuantizePitchDialog::setAndAccept(p[1].getFloatValue(), p[2].getIntValue() != 0);
      return true;
    }
    if (command == "fitall") { pianoRoll.zoomToFitAll(); return true; }
    if (command == "zoomprev") { pianoRoll.restorePreviousZoom(); return true; }
    if (command == "pitchlines") { pianoRoll.setPitchLinesBackground(!pianoRoll.getPitchLinesBackground()); return true; }
    if (command == "replace:reset") { pianoRoll.resetReplaceRanges(); return true; }
    if (command.startsWith("macroset:")) {
      const auto rest = command.fromFirstOccurrenceOf(":", false, false);
      MacroDialog::setValue(rest.upToFirstOccurrenceOf("=", false, false),
                            rest.fromFirstOccurrenceOf("=", false, false).getDoubleValue());
      return true;
    }
    if (command.startsWith("cursor:")) {
      pianoRoll.setCursorTime(command.fromFirstOccurrenceOf(":", false, false).getDoubleValue());
      return true;
    }
    return false;
  }
  bool getUseDawTrackColour() const override {
    return trackListPanel.getUseDawTrackColour();
  }
  void setOnTrackColourModeChanged(std::function<void()> callback) override {
    onTrackColourModeChanged = std::move(callback);
  }
  void updateRegionPreviews(MainViewRegionPreviewList previews) override;
  void setTrackViewModeAvailable(bool available) override;
  void setTrackListAvailable(bool available) override;
  void refreshSharedKey() override;
  // A skin change (from this window or another) re-draws cached layers.
  void lookAndFeelChanged() override { pianoRoll.refreshSkin(); }
  void setOwnTrackEditable(bool editable) override { pianoRoll.setOwnTrackEditable(editable); }
  bool trackListShownByDefault = false;
  // Auto-scroll: paused while notes are selected or after you
  // scroll away; reset when playback stops.
  bool autoScrollPaused = false;
  double autoScrollLastX = -1.0;
  void showTrackList(bool show) override;
  bool isTrackViewMode() const override {
    return pianoRollView.isTrackViewMode();
  }
  void requestTrackViewMode(bool track) override {
    if (pianoRollView.isTrackViewMode() != track && toolbar.onTrackViewModeChanged)
      toolbar.onTrackViewModeChanged(track);
  }
  void setOnTrackViewModeChanged(std::function<void(bool)> callback) override {
    onTrackViewModeChanged = std::move(callback);
  }
  void setOnRegionActivationRequested(
      std::function<void(const juce::String &)> callback) override {
    onRegionActivationRequested = std::move(callback);
  }
  void setHostTransportControlAvailable(bool available) override;
  bool isHostTransportControlAvailable() const {
    return hostTransportControlAvailable;
  }
  void updateRecordArmState(bool armed) override {
    toolbar.setRecordArmed(armed);
  }
  void beginLiveRecording(double sampleRate,
                          double timelineOffsetSeconds) override;
  void appendLiveRecordingAudio(
      const juce::AudioBuffer<float> &buffer) override;
  juce::Point<int> getSavedWindowSize() const;

  // Check if ARA mode is active.
  bool isARAModeActive() const;

  // Plugin mode - host audio handling
  void setHostAudio(const juce::AudioBuffer<float> &buffer,
                    double sampleRate,
                    double timelineOffsetSeconds = 0.0) override;
  void updateHostAudioTimelineOffset(double timelineOffsetSeconds) override;
  void clearHostAudio() override;
  void focusTimelineRange(double startSeconds, double endSeconds) override;

  // Plugin mode callbacks
  std::function<void()>
      onProjectDataChanged; // Called when project data is ready or changed
  std::function<void()>
      onPitchEditFinished; // Called when pitch editing is finished
                           // (triggers real-time update)
  std::function<void(const Project &)> onRequestBackendRender;
  std::function<void(const Project &, int, int)> onRequestBackendPreview;
  std::function<void()> onStopBackendPreview;
  std::function<void(const juce::AudioBuffer<float> &, double)> onRequestDragAudition;
  std::function<void()> onStopDragAudition;

  // Plugin mode - request host transport control (optional; only works if host
  // supports it)
  std::function<void(bool shouldPlay)> onRequestHostPlayState;
  std::function<void()> onRequestHostStop;
  std::function<void(double timeInSeconds)> onRequestHostSeek;
  std::function<void(double startSeconds, double endSeconds, bool enabled,
                     bool hasRange)>
      onRequestHostLoopRange;
  std::function<void(bool armed)> onRecordArmChanged;

  // Plugin mode - update playback position from host
  void updatePlaybackPosition(double timeSeconds) override;
  void setTimelineDisplayOffset(double seconds) override;
  void updateHostPlaybackState(bool isPlaying) override;
  void updateHostTimelineState(double bpm, int numerator,
                               int denominator) override;
  void updateHostLoopRange(double startSeconds, double endSeconds,
                           bool enabled, bool hasRange) override;
  void notifyHostStopped() override; // Called when host stops playback
  void triggerResynthesis() override; // Triggered by DAW parameter automation

private:
  void openFile();
  void exportFile();
  void exportMidiFile();
  bool writeMidiExport(const juce::File &file, const Project &project,
                       const MidiExportOptions &options);
  std::function<std::vector<MidiExportTrack>()> midiExportTracksProvider;
  void play();
  void pause();
  void stop();
  // scrollToCursor=false leaves the view where it is (used when stop/finish
  // rewinds the cursor while Auto Scroll is off).
  void seek(double time, bool scrollToCursor = true);
  void previewNoteRegion(int startFrame, int endFrame);
  void finishPreviewRegion(bool restorePosition);
  void auditionDraggedNote(const Note &note);
  void finishDraggedNoteAudition();
  void dispatchPendingDragAudition();
  void auditionPianoKey(int midiNote);
  void finishPianoKeyAudition();
  void jumpTransport(bool forward);
  void resynthesizeIncremental(); // Incremental synthesis on edit
  void showSettings();
  void openRecentFile(const juce::File &file);
  void addRecentFile(const juce::File &file);
  void refreshRecentFilesMenu();

  void onNoteSelected(Note *note);
  void onPitchEdited();
  void onZoomChanged(float pixelsPerSecond);
  void reinterpolateUV(int startFrame,
                       int endFrame); // Re-infer UV regions using FCPE
  void notifyProjectDataChanged();
  void fitAnalyzedPitchRangeToView(Project &project);
  void applyUiBrightness(double brightnessPercent);

  void reloadInferenceModels(bool async = false);
  bool isInferenceBusy() const;
  // Keeps the Rendering card's device row in step with the execution provider
  // chosen in Settings - the list is per-provider, and so is the default.
  void refreshRenderDeviceOptions();
  void applyCachedHostLoopRange();
  void checkForUpdatesOnLaunch();
  void showUpdateAvailablePopup(const juce::String &latestVersion,
                                const juce::String &releaseNotes);
  void skipUpdateVersion(const juce::String &version);
  void showAboutPopup();

  void loadAudioFile(const juce::File &file);
  void clearProjectForNewLoad();
  void openProjectFile(const juce::File &file);
  void analyzeAudio();
  void analyzeAudio(
      Project &targetProject,
      const std::function<void(double, const juce::String &)> &onProgress,
      std::function<void()> onComplete = nullptr);
  void segmentIntoNotes();
  void segmentIntoNotes(Project &targetProject);

  void saveProject(bool saveAs = false);
  void restoreProjectPresentation();

  void undo();
  void redo();
  // Shared by undo, redo and the note Restore menu, so all three process a
  // history change identically.
  void afterHistoryChange(bool requiresResynthesis);
  void setEditMode(EditMode mode);
  void setToolGroupEnabled(bool enabled);

  std::unique_ptr<EditorController> ownedEditorController;
  EditorController *editorController = nullptr;
  std::unique_ptr<PitchUndoManager> ownedUndoManager;
  PitchUndoManager *undoManager = nullptr;
  std::unique_ptr<juce::ApplicationCommandManager> commandManager;
  bool toolGroupEnabled = true;
  bool previewRegionActive = false;
  double previewRegionEndTime = 0.0;
  double previewRegionReturnTime = 0.0;
  double previewRegionStartTime = 0.0;
  double previewRegionWallClockStartMs = 0.0;
  bool previewRegionWasProjectPlaying = false;
  std::unique_ptr<ResampledLoopAudition> resampledLoopAudition;
  bool dragAuditionEnabled = false;
  bool noteDragAuditionActive = false;
  bool noteDragAuditionBackendPreviewStarted = false;
  bool noteDragAuditionPending = false;
  bool noteDragAuditionWasPlaying = false;
  double noteDragAuditionReturnTime = 0.0;
  int noteDragAuditionStartFrame = 0;
  int noteDragAuditionEndFrame = 0;
  juce::int64 noteDragAuditionLastInputMs = 0;
  float pendingNoteDragAuditionShift = 0.0f;
  float pendingNoteDragAuditionMidiNote = 60.0f;
  std::vector<float> pendingNoteDragAuditionSource;
  static constexpr juce::int64 noteDragAuditionDebounceMs = 20;

  // New modular components
  std::unique_ptr<AudioFileManager> fileManager;
  std::unique_ptr<MenuHandler> menuHandler;
  std::unique_ptr<SettingsManager> settingsManager;

  const bool enableAudioDeviceFlag;

  CustomMenuBarLookAndFeel menuBarLookAndFeel;
  juce::MenuBarComponent menuBar;
#if JUCE_MAC
  juce::PopupMenu macExtraAppleMenuItems;
#endif
  ToolbarComponent toolbar;
  WorkspaceComponent workspace;
  PianoRollComponent pianoRoll;
  PianoRollWorkspaceView pianoRollView;
  ParameterPanel parameterPanel;
  TrackListPanel trackListPanel; // content of the left side panel
  std::function<void()> onTrackColourModeChanged;
  std::unique_ptr<UiBrightnessEffect> uiBrightnessEffect;
  AnalysisBackdrop analysisBackdrop;
  AnalysisProgressPopup analysisProgressPopup;

  // Shared across editors so tooltips can extend beyond their native windows.
  std::shared_ptr<juce::TooltipWindow> tooltipWindow;

  std::unique_ptr<SettingsOverlay> settingsOverlay;

  std::unique_ptr<juce::FileChooser> fileChooser;
  juce::StringArray recentFiles;

  bool isPlaying = false;
  bool hostTransportControlAvailable = true;
  std::function<void(const juce::String &)> onRegionActivationRequested;
  std::function<void(bool)> onTrackViewModeChanged;
  // True while a canvas click is switching the active region. The clicked
  // region is already on screen, so its project must not refit the view.
  bool regionActivationFromCanvas = false;
  bool liveRecordingActive = false;
  bool pianoKeyAuditionActive = false;
  bool pianoKeyAuditionWasPlaying = false;
  double pianoKeyAuditionReturnTime = 0.0;

  // Sync flag to prevent infinite loops
  bool isSyncingZoom = false;

  // The update worker must finish before networking shuts down.
  std::unique_ptr<juce::WebInputStream> updateRequest;
  std::thread updateCheckThread;
  std::atomic<bool> stoppingUpdateCheck{false};

  // Async load state
  std::atomic<bool> isLoadingAudio{false};
  std::atomic<double> loadingProgress{0.0};
  juce::CriticalSection loadingMessageLock;
  juce::String loadingMessage;
  juce::String lastLoadingMessage;

  // Async render state (plugin mode) is managed by EditorController
  // Incremental synthesis coalescing
  std::atomic<bool> pendingIncrementalResynth{false};

  // Cursor update throttling
  std::atomic<double> pendingCursorTime{0.0};
  std::atomic<bool> hasPendingCursorUpdate{false};

  bool hasCachedHostTimelineState = false;
  double cachedHostTempoBpm = 120.0;
  int cachedHostBeatNumerator = 4;
  int cachedHostBeatDenominator = 4;

  bool hasCachedHostLoopRange = false;
  double cachedHostLoopStartSeconds = 0.0;
  double cachedHostLoopEndSeconds = 0.0;
  bool cachedHostLoopEnabled = false;
  bool cachedHostLoopHasRange = false;
  double suppressPlaybackFollowUntilMs = 0.0;
  juce::int64 lastCursorUpdateTime = 0;

#if JUCE_MAC
  juce::ComponentDragger dragger;
#endif

  // Rebuilds the menu bar and command names after a language change; the
  // child components refresh themselves through their own watchers.
  void refreshLocalisedText();

  // Declared last so it is torn down before what it refreshes.
  LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
