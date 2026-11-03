#include "PluginEditor.h"
#include "UnisonCoordinator.h"
#include "../Utils/TimingAligner.h"
#include "../Utils/LeadProjection.h"
#include "../Utils/DoubleTuning.h"
#include "HostCompatibility.h"
#include "InstanceRegistry.h"
#include "../Utils/AudioResampler.h"
#include "../Models/Project.h"
#include "../Utils/AppLogger.h"
#include "../Utils/Constants.h"
#include "../Undo/ContextUndoAction.h"
#include "../Undo/PitchUndoManager.h"

#include <algorithm>
#include <cmath>

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"
#include "PitchNetAudioModification.h"
#endif

namespace
{
#if JucePlugin_Enable_ARA
// How often the Regions card re-reads the host's region list.
constexpr int kRegionListRefreshHz = 8;
juce::String araTrackKey(juce::ARARegionSequence*);
#endif

namespace {
// Every open PitchNet window in this process (message thread only), so a
// window can find the one showing a track it wants to switch to.
std::vector<PitchNetAudioProcessorEditor *> &openEditors() {
  static std::vector<PitchNetAudioProcessorEditor *> editors;
  return editors;
}
} // namespace

#if JucePlugin_Enable_ARA
// A copy of the part of `source` between windowStart and windowEnd
// (modification seconds), rebased so its time zero is the window's first
// analysis frame, and holding only what the note and pitch-curve renderers read:
// notes, pitch arrays and the window's audio (for the notes' inline waveform).
// No mel spectrogram or original audio, so its size follows the region, not
// the take. windowZeroSeconds receives the window zero in modification time.
std::shared_ptr<Project> makeRegionWindowProject(const Project &source,
                                                 double windowStart,
                                                 double windowEnd,
                                                 double &windowZeroSeconds)
{
  windowZeroSeconds = 0.0;
  const auto &src = source.getAudioData();
  const int frameCount = static_cast<int>(src.f0.size());
  if (frameCount <= 0 || windowEnd <= windowStart)
    return nullptr;

  const double framesPerSecond =
      static_cast<double>(SAMPLE_RATE) / static_cast<double>(HOP_SIZE);
  int firstFrame = juce::jlimit(
      0, frameCount, static_cast<int>(std::floor(windowStart * framesPerSecond)));
  int lastFrame = juce::jlimit(
      firstFrame, frameCount,
      static_cast<int>(std::ceil(windowEnd * framesPerSecond)) + 1);
  if (lastFrame <= firstFrame)
    return nullptr;
  // Keep notes whole: widen the window to every note it touches, so no note
  // starts before the copy's frame zero (the canvas clips to the region).
  {
    const int windowFirst = firstFrame;
    const int windowLast = lastFrame;
    for (const auto &note : source.getNotes())
      if (note.getEndFrame() > windowFirst && note.getStartFrame() < windowLast)
      {
        firstFrame = std::min(firstFrame, note.getStartFrame());
        lastFrame = std::max(lastFrame, note.getEndFrame());
      }
    firstFrame = juce::jlimit(0, frameCount, firstFrame);
    lastFrame = juce::jlimit(firstFrame, frameCount, lastFrame);
  }
  windowZeroSeconds = framesToSeconds(firstFrame);

  auto window = std::make_shared<Project>();
  window->setPitchReferenceHz(source.getPitchReferenceHz());
  auto &dst = window->getAudioData();
  dst.sampleRate = src.sampleRate;

  const auto sliceFloats = [&](const std::vector<float> &from,
                               std::vector<float> &to) {
    const int end = std::min(lastFrame, static_cast<int>(from.size()));
    if (end > firstFrame)
      to.assign(from.begin() + firstFrame, from.begin() + end);
  };
  const auto sliceBools = [&](const std::vector<bool> &from,
                              std::vector<bool> &to) {
    const int end = std::min(lastFrame, static_cast<int>(from.size()));
    if (end > firstFrame)
      to.assign(from.begin() + firstFrame, from.begin() + end);
  };
  sliceFloats(src.rawF0, dst.rawF0);
  sliceFloats(src.cleanedF0, dst.cleanedF0);
  sliceFloats(src.denseF0, dst.denseF0);
  sliceFloats(src.f0, dst.f0);
  sliceFloats(src.baseF0, dst.baseF0);
  sliceFloats(src.basePitch, dst.basePitch);
  sliceFloats(src.deltaPitch, dst.deltaPitch);
  sliceBools(src.voicedMask, dst.voicedMask);
  sliceBools(src.vadMask, dst.vadMask);

  const int numSamples = src.waveform.getNumSamples();
  if (numSamples > 0 && src.sampleRate > 0 && src.waveform.getNumChannels() > 0)
  {
    const int firstSample = juce::jlimit(
        0, numSamples,
        static_cast<int>(std::llround(windowZeroSeconds * src.sampleRate)));
    const int lastSample = juce::jlimit(
        firstSample, numSamples,
        static_cast<int>(std::ceil(framesToSeconds(lastFrame) * src.sampleRate)));
    if (lastSample > firstSample)
    {
      dst.waveform.setSize(1, lastSample - firstSample);
      dst.waveform.copyFrom(0, 0, src.waveform, 0, firstSample,
                            lastSample - firstSample);
    }
  }

  auto &notes = window->getNotes();
  for (const auto &note : source.getNotes())
  {
    // Every note touching the region was covered by widening the window; a
    // note starting before it lies wholly outside the region and is skipped.
    if (note.getStartFrame() < firstFrame || note.getStartFrame() >= lastFrame)
      continue;
    Note copy = note;
    copy.setStartFrame(copy.getStartFrame() - firstFrame);
    copy.setEndFrame(copy.getEndFrame() - firstFrame);
    copy.setSrcStartFrame(copy.getSrcStartFrame() - firstFrame);
    copy.setSrcEndFrame(copy.getSrcEndFrame() - firstFrame);
    copy.setSelected(note.isSelected()); // Track selection spans all projects
    notes.push_back(std::move(copy));
  }
  return window;
}

// Snapshot of one playback region's window onto its modification's Project,
// as plain drawing data placed at the region's host-timeline position.
// Projects are stored in MODIFICATION time (PR #8), so the window starts at
// the region's start in the modification and is shifted by
// (host start - modification start) onto the timeline.
MainViewRegionPreview buildRegionPreview(const Project &project,
                                         double regionStart, double regionEnd,
                                         double startInModificationSeconds)
{
  MainViewRegionPreview preview;
  preview.hasContent = true;

  const auto &audioData = project.getAudioData();
  const double shift = regionStart - std::max(0.0, startInModificationSeconds);
  const double localStart = regionStart - shift;
  const double localEnd = regionEnd - shift;

  for (const auto &note : project.getNotes())
  {
    if (note.isRest())
      continue;
    const double start =
        std::max(regionStart, framesToSeconds(note.getVisualStartFrame()) + shift);
    const double end =
        std::min(regionEnd, framesToSeconds(note.getVisualEndFrame()) + shift);
    if (end <= start)
      continue;
    preview.notes.push_back({start, end, note.getAdjustedMidiNote()});
  }

  preview.project = makeRegionWindowProject(
      project, localStart, localEnd, preview.projectHostOffsetSeconds);
  preview.sourceProject = &project;
  preview.sourceFirstFrame = static_cast<int>(std::llround(
      preview.projectHostOffsetSeconds * SAMPLE_RATE / HOP_SIZE));
  if (preview.project != nullptr)
    preview.projectHostOffsetSeconds += shift;

  const auto &waveform = audioData.waveform;
  const int numSamples = waveform.getNumSamples();
  const double sampleRate = static_cast<double>(audioData.sampleRate);
  if (numSamples > 0 && sampleRate > 0.0 && waveform.getNumChannels() > 0)
  {
    const int firstSample = juce::jlimit(
        0, numSamples, static_cast<int>(std::floor(localStart * sampleRate)));
    const int lastSample = juce::jlimit(
        firstSample, numSamples,
        static_cast<int>(std::ceil(localEnd * sampleRate)));
    if (lastSample > firstSample)
    {
      const float *data = waveform.getReadPointer(0);
      preview.waveformSamples = std::make_shared<const std::vector<float>>(
          data + firstSample, data + lastSample);
      preview.waveformSampleRate = sampleRate;
      preview.waveformStartSeconds = firstSample / sampleRate + shift;
    }
  }

  return preview;
}
#endif

bool isLunaHostProcess()
{
#if JUCE_WINDOWS
  const auto host = juce::File::getSpecialLocation(
      juce::File::hostApplicationPath);
  return host.getFileNameWithoutExtension().equalsIgnoreCase("LUNA");
#else
  return false;
#endif
}
} // namespace

PitchNetAudioProcessorEditor::PitchNetAudioProcessorEditor(
    PitchNetAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p)
#if JucePlugin_Enable_ARA
      ,
      AudioProcessorEditorARAExtension(&p)
#endif
{
  targetProcessor = &audioProcessor;
  targetId = audioProcessor.getInstanceRegistryId();
  openEditors().push_back(this);

  // Fonts must exist before MainComponent constructs labels that retain them.
  initializeUiResources();
  mainView = createMainView(false);

  // Enable keyboard focus for the editor and the main view. This is required so
  // that when the host forwards a key event (see the IPlugView::onKeyDown patch
  // in juce_audio_plugin_client_VST3.cpp), MainComponent is the focused
  // component and its command-key mappings sit in the dispatch chain.
  setWantsKeyboardFocus(true);
  setMouseClickGrabsKeyboardFocus(true);

  addAndMakeVisible(*mainView->getComponent());
  mainView->getComponent()->setWantsKeyboardFocus(true);
  mainView->getComponent()->setMouseClickGrabsKeyboardFocus(true);
  audioProcessor.setMainComponent(mainView.get());
  addMouseListener(this, true);

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto size = audioProcessor.getLastEditorSize();
  if (auto *project = mainView->getProject();
      project && project->uiWidth > 0 && project->uiHeight > 0)
    size = {project->uiWidth, project->uiHeight};
  if (size.x <= 0 || size.y <= 0)
    size = getDefaultMainViewSize(this);
  setSize(size.x, size.y);
  setResizable(true, true);

  // Grab keyboard focus once the host has actually shown the editor.
  requestMainViewKeyboardFocusAsync();
}

#if JucePlugin_Enable_ARA
juce::AudioProcessorEditorARAExtension *
PitchNetAudioProcessorEditor::getARAClientExtensions() {
  return this;
}
#endif

PitchNetAudioProcessorEditor::~PitchNetAudioProcessorEditor() {
  stopTimer();
#if JucePlugin_Enable_ARA
  flushAraPersist(true);
#endif
  InstanceRegistry::get().removeChangeListener(this);
  auto &editors = openEditors();
  editors.erase(std::remove(editors.begin(), editors.end(), this), editors.end());
  // Other windows editing this window's track borrow its project; it goes
  // away with this window.
  releaseLiveView(mainView.get());
  while (!coEditTracks.empty())
    closeCoEdit(coEditTracks.begin()->first);

  // Some ARA hosts briefly overlap editor instances while rebuilding the plug-
  // in view.  Only the editor that still owns the processor binding may tear
  // down shared callbacks; an older editor's destructor must not detach the
  // newer visible editor. Without ARA this window may be editing another
  // instance's track (targetProcessor); that one is let go the same way.
  const bool ownsProcessorMainView =
      targetIsAlive() && target().getMainComponent() == mainView.get();
#if JucePlugin_Enable_ARA
  if (auto *araEditorView = getARAEditorView()) {
    araEditorView->removeListener(this);
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController)) {
        // Do NOT clear the realtime-processor binding or persistence callbacks
        // here: both are owned by the processor and must stay live so ARA
        // playback and project restore keep working headlessly after the editor
        // closes. The processor detaches them in its destructor.
        if (pitchDocController->getMainComponent() == mainView.get()) {
          pitchDocController->releaseEditorProcessor(&audioProcessor);
          pitchDocController->setMainComponent(nullptr);
        }
      }
    }
  }
#endif

  removeMouseListener(this);
  if (ownsProcessorMainView) {
    // Preview/audition is editor-owned in non-ARA mode. Do not leave its
    // per-sample rendering path active after the controls that stop it are gone.
    target().stopPluginAudition();
    target().getTransportController().clearCallbacks();
    target().setMainComponent(nullptr);
  }

  // Destroy all components (and their retained Font/Typeface objects) before
  // releasing the final shared UI resources. This is especially important for
  // DirectWrite memory fonts when a Windows host unloads the plugin DLL.
  mainView.reset();
  shutdownUiResources();
}

void PitchNetAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  auto *editorView = getARAEditorView();
  if (!editorView) {
    setupNonARAMode();
    return;
  }

  auto *docController = editorView->getDocumentController();
  if (!docController) {
    setupNonARAMode();
    return;
  }

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  if (!pitchDocController) {
    setupNonARAMode();
    return;
  }

  mainView->setRecordControlVisible(false);
  mainView->setOnRecordArmChanged(nullptr);
  // ARA gives us a host link: transport controls and cycle editing are live.
  mainView->setHostTransportControlAvailable(true);

  // Listen for host selection changes to switch the canvas to the clicked
  // region (per-region Projects).
  editorView->addListener(this);

  auto safeEditorForHistory = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this);
  audioProcessor.onAraUndoManagerRequested = [safeEditorForHistory](const juce::String& key) -> PitchUndoManager* {
    if (!safeEditorForHistory) return nullptr;
    auto* editor = safeEditorForHistory.getComponent();
    for (auto* region : editor->collectAraPlaybackRegions()) {
      if (pitchnetRegionKey(*region) != key ||
          pitchnetRegionSelector(*region) != editor->audioProcessor.getActiveAraRegionSelector()) continue;
      auto* sequence = region->getRegionSequence();
      if (!sequence) return nullptr;
      auto* manager = editor->audioProcessor.getTrackUndoManager(araTrackKey(sequence));
      const juce::String regionName(region->getName());
      const juce::String label = regionName.isNotEmpty() ? regionName : "Clip";
      const auto trackKey = araTrackKey(sequence);
      manager->decorateAction = [safeEditorForHistory, key, label, trackKey](std::unique_ptr<UndoableAction> action) {
        if (action->ownsProjectContext()) return action;
        auto* editor = safeEditorForHistory.getComponent();
        if (!editor) return std::unique_ptr<UndoableAction>{};
        Project* expected = editor->mainView->getProject();
        auto prepare = [safeEditorForHistory, key, expected, trackKey] {
          if (!safeEditorForHistory) return false;
          auto* editor = safeEditorForHistory.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions()) {
            if (pitchnetRegionKey(*live) != key || araTrackKey(live->getRegionSequence()) != trackKey) continue;
            std::uint64_t revision = 0; bool canvas = false;
            if (editor->audioProcessor.getAraProjectForPreview(
                live->getAudioModification<PitchNetAudioModification>(), revision, canvas) != expected)
              return false;
            editor->activateAraRegion(live, false);
            return true;
          }
          return false;
        };
        return std::unique_ptr<UndoableAction>(new ContextUndoAction(
            std::move(action), key, label, std::move(prepare)));
      };
      return manager;
    }
    return nullptr;
  };

  mainView->setTrackPitchMacroTargets([this, pitchDocController] {
    std::vector<TrackPitchMacroTarget> targets;
    const auto regions = collectAraPlaybackRegions();
    juce::ARARegionSequence* sequence = nullptr;
    for (auto* region : regions)
      if (pitchnetRegionSelector(*region) == audioProcessor.getActiveAraRegionSelector())
        sequence = region->getRegionSequence();
    if (!sequence) return targets;
    std::map<juce::String, size_t> indices;
    for (auto* region : regions) {
      auto* regionSequence = region->getRegionSequence();
      const bool edited =
          regionSequence != nullptr &&
          (regionSequence == sequence ? araOwnTrackEditable
                                      : araCoEditTrackKeys.count(araTrackKey(regionSequence)) != 0);
      if (!edited) continue;
      auto* modification = region->getAudioModification<PitchNetAudioModification>();
      if (!modification) continue;
      const auto key = pitchnetModificationKey(*modification);
      std::uint64_t revision = 0;
      bool fromCanvas = false;
      auto* project = const_cast<Project*>(audioProcessor.getAraProjectForPreview(
          modification, revision, fromCanvas));
      if (project && audioProcessor.araRegionProjectNeedsSourceHydration(key)) {
        pitchDocController->requestRegionCanvasAnalysis(region);
        project = const_cast<Project*>(audioProcessor.getAraProjectForPreview(
            modification, revision, fromCanvas));
      }
      if (!project || audioProcessor.araRegionProjectNeedsSourceHydration(key)) {
        mainView->setStatusMessage("Wait for all regions in this track to finish analysis.");
        return std::vector<TrackPitchMacroTarget>{};
      }
      auto found = indices.find(key);
      if (found == indices.end()) {
        const size_t index = targets.size();
        indices.emplace(key, index);
        TrackPitchMacroTarget target;
        target.project = project;
        // Resolve the current project by key rather than retaining a host object
        // in undo history. A split/replacement invalidates this action safely.
        auto safeEditor = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this);
        target.isValid = [safeEditor, key, project] {
          if (!safeEditor) return false;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              std::uint64_t rev = 0; bool canvas = false;
              return editor->audioProcessor.getAraProjectForPreview(
                  live->getAudioModification<PitchNetAudioModification>(), rev, canvas) == project;
            }
          return false;
        };
        target.onChanged = [safeEditor, key, project](bool render) {
          if (!safeEditor) return;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              auto* mod = live->getAudioModification<PitchNetAudioModification>();
              if (render) {
                juce::MemoryBlock archive;
                if (editor->audioProcessor.serializeAraRegionProject(key, archive))
                  mod->setProjectArchive(archive.getData(), archive.getSize());
                editor->audioProcessor.requestPluginProjectRender(*project, mod);
              }
              editor->regionPreviewCache.clear();
              editor->lastPublishedPreviewSignature.clear();
              editor->refreshAraRegionList();
              return;
            }
        };
        target.onCommit = [safeEditor, key] {
          if (!safeEditor) return;
          auto* editor = safeEditor.getComponent();
          for (auto* live : editor->collectAraPlaybackRegions())
            if (pitchnetRegionKey(*live) == key) {
              juce::MemoryBlock archive;
              if (editor->audioProcessor.serializeAraRegionProject(key, archive))
                live->getAudioModification<PitchNetAudioModification>()->setProjectArchive(
                    archive.getData(), archive.getSize());
              return;
            }
        };
        targets.push_back(std::move(target));
        found = indices.find(key);
      }
      const auto* source = modification->getAudioSource();
      const double rate = source ? source->getSampleRate() : 0.0;
      if (rate > 0.0) {
        const double framesPerSample = project->getAudioData().sampleRate / (rate * HOP_SIZE);
        const int first = static_cast<int>(std::floor(
            region->getStartInAudioModificationSamples() * framesPerSample));
        const int last = static_cast<int>(std::ceil(
            region->getEndInAudioModificationSamples() * framesPerSample));
        targets[found->second].frames.emplace_back(first, last);
      }
    }
    return targets;
  });

  // Clicking inside an inactive region on the canvas makes it active in place.
  mainView->setOnRegionActivationRequested(
      [this](const juce::String &regionKey) {
        activateAraRegionByKey(regionKey, false);
      });
  // Clicking an inactive track in the left side panel switches to it.
  mainView->setOnTrackSelected([this](const juce::String &trackKey) {
    // Edit on a reference track swaps the two tracks' roles - the
    // edited track becomes a reference.
    if (pinnedTrackKeys.count(trackKey) > 0 && araActiveTrackKey.isNotEmpty() &&
        araActiveTrackKey != trackKey)
      pinnedTrackKeys.insert(araActiveTrackKey);
    activateAraTrackByKey(trackKey);
  });
  // Double-clicking a grey (reference) note: its region and track become
  // the edited ones, and the previously edited track a reference.
  mainView->setOnReferenceNoteDoubleClicked([this](const juce::String &regionKey) {
    for (auto *region : collectAraPlaybackRegions())
      if (pitchnetRegionSelector(*region) == regionKey) {
        if (araActiveTrackKey.isNotEmpty())
          pinnedTrackKeys.insert(araActiveTrackKey);
        pinnedTrackKeys.erase(araTrackKey(region->getRegionSequence()));
        break;
      }
    lastPublishedTrackSignature.clear();
    lastPublishedPreviewSignature.clear();
    activateAraRegionByKey(regionKey);
  });
  // Edit: any number of tracks edited at once, as in hosts without ARA.
  // Their clips become editable windows next to the active track's (Track
  // mode, where clips sit on the timeline). The active track is always on.
  mainView->setOnTrackEditAdded([this](const juce::String &trackKey) {
    if (trackKey.isEmpty())
      return;
    if (trackKey == araActiveTrackKey) {
      setAraOwnTrackEditable(!araOwnTrackEditable);
      return;
    }
    if (!araCoEditTrackKeys.erase(trackKey)) {
      araCoEditTrackKeys.insert(trackKey);
      pinnedTrackKeys.erase(trackKey);
      if (mainView != nullptr && !mainView->isTrackViewMode())
        mainView->requestTrackViewMode(true);
    }
    lastPublishedTrackSignature.clear();
    lastPublishedPreviewSignature.clear();
    refreshAraRegionList();
  });
  // Pinning a track draws its notes on the canvas (Track mode).
  mainView->setOnTrackPinChanged([this](const juce::String &trackKey,
                                        bool pinned) {
    if (pinned) {
      pinnedTrackKeys.insert(trackKey);
      araCoEditTrackKeys.erase(trackKey); // Ghost and Edit exclude each other
      if (trackKey == araActiveTrackKey) {
        // Ghost notes are drawn in Track mode, with the other tracks.
        if (mainView != nullptr && !mainView->isTrackViewMode())
          mainView->requestTrackViewMode(true);
        setAraOwnTrackEditable(false);
        return;
      }
    } else {
      pinnedTrackKeys.erase(trackKey);
    }
    lastPublishedTrackSignature.clear();
    lastPublishedPreviewSignature.clear();
    refreshAraRegionList();
  });
  // Pinned notes are outlined in the track colour, which this switch changes.
  mainView->setOnTrackColourModeChanged([this] {
    lastPublishedPreviewSignature.clear();
    refreshAraRegionList();
  });
  // Clip / Track toggle. Track mode needs the other regions' previews; Clip
  // mode drops them. Republish right away rather than on the next poll.
  mainView->setOnTrackViewModeChanged([this](bool) {
    lastPublishedPreviewSignature.clear();
    lastPublishedRegionSignature.clear();
    refreshAraRegionList();
  });
  mainView->setTrackViewModeAvailable(true);

  // Connect ARA controller to UI
  pitchDocController->setMainComponent(mainView.get());
  // Bind via the processor so the realtime-processor link outlives the editor:
  // ARA playback/bounce must keep working after the UI is closed.
  audioProcessor.setAraDocumentController(pitchDocController);
  pitchDocController->setEditorProcessor(&audioProcessor);
  audioProcessor.bindAraUndoManager(nullptr);
  // Persistence callbacks are owned by the processor (set in didBindToARA) so
  // that saved-project restore works with the UI closed; the editor must not
  // install editor-capturing callbacks that would dangle on close.
  mainView->setOnRequestBackendRender([this](const Project &project) {
    audioProcessor.requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [pitchDocController](const Project &project, int startFrame,
                           int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        pitchDocController->startPreviewRange(
            static_cast<double>(startFrame) * HOP_SIZE / sampleRate,
            static_cast<double>(endFrame) * HOP_SIZE / sampleRate);
      });
  mainView->setOnStopBackendPreview(
      [pitchDocController]() { pitchDocController->stopPreview(); });
  mainView->setOnRequestDragAudition(
      [pitchDocController](const juce::AudioBuffer<float> &buffer,
                           double sampleRate) {
        pitchDocController->startPreviewAudio(buffer, sampleRate);
      });
  mainView->setOnStopDragAudition(
      [pitchDocController]() { pitchDocController->stopPreview(); });

  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          if (shouldPlay)
            playbackController->requestStartPlayback();
          else
            playbackController->requestStopPlayback();
          return;
        }
      }
    }

    audioProcessor.requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestStopPlayback();
          playbackController->requestSetPlaybackPosition(0.0);
          return;
        }
      }
    }

    audioProcessor.requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *playbackController =
                araDocController->getHostPlaybackController()) {
          playbackController->requestSetPlaybackPosition(timeInSeconds);
          return;
        }
      }
    }

    audioProcessor.requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(
      [this](double startSeconds, double endSeconds, bool enabled,
             bool hasRange) {
        if (auto *araEditorView = getARAEditorView()) {
          if (auto *araDocController = araEditorView->getDocumentController()) {
            if (auto *playbackController =
                    araDocController->getHostPlaybackController()) {
              if (hasRange && endSeconds > startSeconds)
                playbackController->requestSetCycleRange(
                    startSeconds, endSeconds - startSeconds);

              playbackController->requestEnableCycle(enabled);
              return;
            }
          }
        }

        juce::ignoreUnused(startSeconds, endSeconds, enabled, hasRange);
      });

  setupHostTransportUiSync(true);
  // VocalNet's working Studio One Event FX path reads the editor view's
  // current selection directly instead of waiting for a future listener
  // callback. Studio One can open the editor with a region already selected
  // without subsequently announcing a selection change.
  if (audioProcessor.wrapperType == juce::AudioProcessor::wrapperType_AAX)
    startTimerHz(60);
  else
    // Hosts announce region add/remove through the document controller, not to
    // the editor, so the Regions card polls for its list. Slow enough to be
    // free, fast enough that dropping a region in feels immediate.
    startTimerHz(kRegionListRefreshHz);

  setupAraGroupCallbacks();
  refreshAraRegionList();

  // Test hook (unset in normal use): a script of steps run once after the
  // window opens, for checking groups in an ARA host. "sleep:N" waits;
  // "aragroup" makes the first track lead the others; "araunison:on:pct"
  // and "aratiming:on:pct" set the group's switches; "aradump:name" writes
  // every track's notes on the timeline to PITCHNET_PROBE_DUMP_name.json;
  // anything else goes to the main view's test commands.
  static bool araProbeScriptDone = false;
  const auto araScript = juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_ACTIONS", {});
  if (!araProbeScriptDone && araScript.isNotEmpty()) {
    araProbeScriptDone = true;
    const auto steps = juce::StringArray::fromTokens(araScript, ";", "");
    int delayMs = 800;
    for (int s = 0; s < steps.size(); ++s, delayMs += 500) {
      if (steps[s].startsWith("sleep:")) {
        delayMs += steps[s].fromFirstOccurrenceOf(":", false, false).getIntValue();
        continue;
      }
      juce::Timer::callAfterDelay(delayMs, [safe = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this),
                                            step = steps[s]] {
        if (safe == nullptr || safe->mainView == nullptr)
          return;
        PerfTrace::mark("probe action " + step);
        const auto parts = juce::StringArray::fromTokens(step, ":", "");
        const auto action = parts[0];
        const auto tracks = safe->collectAraClips();
        if (action == "araactivate") {
          const int index = parts[1].getIntValue();
          if (index >= 0 && index < static_cast<int>(tracks.size()))
            safe->activateAraTrackByKey(tracks[static_cast<size_t>(index)].first);
          return;
        }
        if (action == "araselectall") {
          // Every note of the tracks being edited (the active one and Edit).
          for (const auto &[key, clips] : tracks)
            if (key == safe->araActiveTrackKey || safe->araCoEditTrackKeys.count(key) != 0)
              for (const auto &clip : clips)
                if (clip.project != nullptr)
                  clip.project->selectAllNotes();
          safe->lastPublishedPreviewSignature.clear();
          safe->regionPreviewCache.clear();
          safe->refreshAraRegionList();
          return;
        }
        if (action == "araedit") {
          const int index = parts[1].getIntValue();
          if (index >= 0 && index < static_cast<int>(tracks.size())) {
            const auto key = tracks[static_cast<size_t>(index)].first;
            if (key != safe->araActiveTrackKey) {
              safe->araCoEditTrackKeys.insert(key);
              safe->pinnedTrackKeys.erase(key);
              if (!safe->mainView->isTrackViewMode())
                safe->mainView->requestTrackViewMode(true);
              safe->lastPublishedTrackSignature.clear();
              safe->lastPublishedPreviewSignature.clear();
              safe->refreshAraRegionList();
            }
          }
          return;
        }
        if (action == "aragroup") {
          Project::DoubleGroup group;
          group.id = juce::Uuid().toString();
          for (size_t i = 0; i < tracks.size(); ++i) {
            group.role = i == 0 ? 1 : 2;
            safe->setAraTrackGroup(tracks[i].first, group);
          }
          return;
        }
        if (action == "araunison" || action == "aratiming") {
          if (const auto it = safe->araGroups.begin(); it != safe->araGroups.end()) {
            const auto id = it->second.id;
            for (const auto &[key, g] : std::map<juce::String, Project::DoubleGroup>(safe->araGroups))
              if (g.id == id) {
                auto next = g;
                if (action == "araunison") {
                  next.unisonOn = parts[1].getIntValue() != 0;
                  next.unisonStrength = parts[2].getFloatValue() / 100.0f;
                } else {
                  next.timingOn = parts[1].getIntValue() != 0;
                  next.timingStrength = parts[2].getFloatValue() / 100.0f;
                }
                safe->setAraTrackGroup(key, next);
              }
          }
          return;
        }
        if (action == "aradump") {
          juce::Array<juce::var> tracksJson;
          for (const auto &[key, clips] : tracks) {
            auto *obj = new juce::DynamicObject();
            obj->setProperty("id", key);
            const auto g = safe->araGroups.find(key);
            obj->setProperty("role", g != safe->araGroups.end() ? g->second.role : 0);
            juce::Array<juce::var> notesJson;
            int analysed = 0;
            for (const auto &clip : clips) {
              if (clip.project == nullptr)
                continue;
              ++analysed;
              const int shift = static_cast<int>(
                  std::lround(clip.offsetSeconds / LeadProjection::frameSeconds(*clip.project)));
              for (const auto &note : clip.project->getNotes())
                if (!note.isRest())
                  notesJson.add(juce::Array<juce::var>{
                      note.getStartFrame() + shift, note.getEndFrame() + shift, note.getMidiNote(),
                      note.hasUnisonSource() ? note.getUnisonSourceMidi() : -1.0f,
                      note.getSibilantHead(), note.getSibilantTail(),
                      note.getSrcStartFrame() + shift, note.getSrcEndFrame() + shift});
            }
            juce::String state;
            for (const auto &clip : clips)
              state << (safe->audioProcessor.hasAraRegionProject(clip.key) ? "P" : "-")
                    << (safe->audioProcessor.araRegionProjectNeedsSourceHydration(clip.key) ? "h" : "")
                    << " ";
            obj->setProperty("clipState", state);
            obj->setProperty("clips", static_cast<int>(clips.size()));
            obj->setProperty("active", key == safe->araActiveTrackKey);
            obj->setProperty("ghost", safe->pinnedTrackKeys.count(key) != 0);
            obj->setProperty("edited", key == safe->araActiveTrackKey ? safe->araOwnTrackEditable
                                                                      : safe->araCoEditTrackKeys.count(key) != 0);
            obj->setProperty("analysed", analysed);
            obj->setProperty("notes", notesJson);
            tracksJson.add(juce::var(obj));
          }
          const auto path = juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_DUMP", {}) + "_" +
                            parts[1] + ".json";
          juce::File(path).replaceWithText(juce::JSON::toString(juce::var(tracksJson)));
          return;
        }
        if (step.startsWith("track:"))
          safe->mainView->showTrackList(true); // clicks land on its buttons
        safe->mainView->runEditCommand(step);
      });
    }
  }

  if (syncInitialARASelectionFromHost())
    return;

  // The playback renderer contains the regions assigned to this plugin
  // instance. Use that set to select the correct track/region sequence rather
  // than guessing from the first source in the shared ARA document.
  if (auto *renderer = audioProcessor.getPlaybackRenderer()) {
    if (pitchDocController->processPlaybackRegions(
            renderer->getPlaybackRegions(), audioProcessor.getSampleRate())) {
      // Hosts are not required to send a selection-change notification when
      // an ARA editor first opens. Activate the region discovered from this
      // instance's renderer immediately instead of relying on onNewSelection().
      audioProcessor.setActiveAraRegion(
          pitchDocController->getCurrentPlaybackRegion());
      return;
    }
  }

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (pitchDocController->processExistingAudioSources(
          static_cast<juce::ARADocument *>(juceDocument))) {
    audioProcessor.setActiveAraRegion(
        pitchDocController->getCurrentPlaybackRegion());
    return;
  }
#endif
}

void PitchNetAudioProcessorEditor::timerCallback() {
  PerfTrace::Scope perfScope("editor timer", 30.0);
  syncAAXARAPlayheadStateFromHost();
  // Tracks let go of when the window they were borrowed from closed or
  // changed track: open them again now, on a later tick, once that window
  // has finished going away (as a copy, or onto what it holds next).
  if (instanceTracksEnabled && !pendingCoEditReopen.empty())
    for (auto id : std::exchange(pendingCoEditReopen, {}))
      if (coEditTracks.count(id) == 0 && id != targetId &&
          InstanceRegistry::get().ownerOf(id) != nullptr)
        toggleCoEdit(InstanceRegistry::keyFor(id));
  // Hosts without ARA: Transfer state of every track header.
  if (instanceTracksEnabled)
    publishInstanceTracks();
  // The skin is shared by every window too.
  if (mainView != nullptr && Skin::active() != lastSkin) {
    lastSkin = Skin::active();
    if (auto *component = mainView->getComponent()) {
      component->sendLookAndFeelChange();
      component->repaint();
    }
  }
  // The key is shared by every track: follow a change made in any window.
  if (instanceTracksEnabled && mainView != nullptr) {
    const auto &shared = *sharedMacroParameters();
    juce::String signature;
    signature << shared.projectKeyRoot << ':' << static_cast<int>(shared.projectKeyMode) << ':'
              << static_cast<int>(shared.scaleMode) << ':' << shared.pitchReferenceHz << ':'
              << shared.customScale.name;
    for (size_t i = 0; i < 12; ++i)
      signature << (shared.customScale.used[i] ? '1' : '0') << shared.customScale.cents[i];
    for (const auto &marker : shared.keyMarkers)
      signature << '|' << marker.seconds << ',' << marker.root << ',' << static_cast<int>(marker.mode);
    if (signature != lastSharedKeySignature) {
      const bool first = lastSharedKeySignature.isEmpty();
      lastSharedKeySignature = signature;
      if (!first)
        mainView->refreshSharedKey();
    }
  }

#if JucePlugin_Enable_ARA
  // The AAX playhead sync needs 60 Hz; the region list does not. Divide down
  // rather than running a second timer.
  if (--regionListRefreshCountdown <= 0) {
    const int interval = juce::jmax(1, getTimerInterval());
    regionListRefreshCountdown =
        juce::jmax(1, (1000 / kRegionListRefreshHz) / interval);
    refreshAraRegionList();
    syncAraDoubles();
  }
  flushAraPersist(false);
#endif
}

void PitchNetAudioProcessorEditor::syncAAXARAPlayheadStateFromHost() {
#if JucePlugin_Enable_ARA
  if (audioProcessor.wrapperType != juce::AudioProcessor::wrapperType_AAX)
    return;

  if (!getARAEditorView() || !mainView)
    return;

  auto *playHead = audioProcessor.getPlayHead();
  if (!playHead)
    return;

  const auto positionInfo = playHead->getPosition();
  if (!positionInfo.hasValue()) {
    if (lastSyncedHostPlayState) {
      lastSyncedHostPlayState = false;
      mainView->updateHostPlaybackState(false);
    }
    return;
  }

  double timeInSeconds = 0.0;
  if (auto time = positionInfo->getTimeInSeconds())
    timeInSeconds = *time;
  else if (auto samples = positionInfo->getTimeInSamples()) {
    const double sampleRate = audioProcessor.getHostSampleRate();
    if (sampleRate > 0.0)
      timeInSeconds = static_cast<double>(*samples) / sampleRate;
  }

  const bool isPlaying = positionInfo->getIsPlaying();
  const bool positionChanged = !hasSyncedHostPlayhead ||
                               !juce::approximatelyEqual(
                                   lastSyncedHostPlayheadSeconds,
                                   timeInSeconds);
  const bool playStateChanged =
      !hasSyncedHostPlayhead || lastSyncedHostPlayState != isPlaying;

  if (positionChanged) {
    lastSyncedHostPlayheadSeconds = timeInSeconds;
    mainView->updatePlaybackPosition(timeInSeconds);
  }

  if (playStateChanged) {
    lastSyncedHostPlayState = isPlaying;
    mainView->updateHostPlaybackState(isPlaying);
  }

  hasSyncedHostPlayhead = true;
#endif
}

#if JucePlugin_Enable_ARA
bool PitchNetAudioProcessorEditor::syncInitialARASelectionFromHost() {
  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return false;

  const auto &viewSelection = editorView->getViewSelection();
  const auto regions =
      viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  if (regions.empty() || regions.front() == nullptr)
    return false;

  onNewSelection(viewSelection);
  return true;
}

size_t PitchNetAudioProcessorEditor::documentControllerRegionCount() const {
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  docController))
        return pitchDocController->getCurrentPlaybackRegions().size();
    }
  }
  return 0;
}

size_t PitchNetAudioProcessorEditor::documentRegionCount() const {
  size_t count = 0;
  if (auto *editorView = getARAEditorView()) {
    if (auto *docController = editorView->getDocumentController()) {
      if (auto *document =
              static_cast<juce::ARADocument *>(docController->getDocument())) {
        for (auto *sequence :
             document->getRegionSequences<juce::ARARegionSequence>())
          if (sequence != nullptr)
            count += sequence->getPlaybackRegions<juce::ARAPlaybackRegion>().size();
      }
    }
  }
  return count;
}

std::vector<juce::ARAPlaybackRegion *>
PitchNetAudioProcessorEditor::collectAraPlaybackRegions() const {
  // A union, not a first-hit lookup. Hosts disagree about what a plug-in
  // instance "owns": some assign every region of the track to the renderer,
  // some assign only the one being played, and per-event hosts assign exactly
  // one. Taking the first non-empty source therefore listed a single region
  // and hid the rest of the track. Instead: gather regions from every source
  // we have, then expand to the full region sequence (track) each of them
  // belongs to, so every clip on the track is selectable no matter which one
  // the host handed us.
  std::vector<juce::ARAPlaybackRegion *> regions;
  std::vector<juce::ARARegionSequence *> sequences;

  const auto isUsable = [](juce::ARAPlaybackRegion *region) {
    return region != nullptr && region->getAudioModification() != nullptr &&
           region->getAudioModification()->getAudioSource() != nullptr;
  };

  const auto addSequence = [&sequences](juce::ARARegionSequence *sequence) {
    if (sequence != nullptr &&
        std::find(sequences.begin(), sequences.end(), sequence) ==
            sequences.end())
      sequences.push_back(sequence);
  };

  const auto addRegions =
      [&](const std::vector<juce::ARAPlaybackRegion *> &candidates) {
        for (auto *candidate : candidates) {
          if (!isUsable(candidate))
            continue;
          if (std::find(regions.begin(), regions.end(), candidate) ==
              regions.end())
            regions.push_back(candidate);
          addSequence(candidate->getRegionSequence());
        }
      };

  // Regions this instance renders.
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    addRegions(renderer->getPlaybackRegions<juce::ARAPlaybackRegion>());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    addRegions(editorRenderer->getPlaybackRegions<juce::ARAPlaybackRegion>());

  auto *editorView = getARAEditorView();
  if (editorView == nullptr)
    return regions;

  // What the host currently has selected, including the track when only a
  // track is selected.
  const auto &viewSelection = editorView->getViewSelection();
  addRegions(viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>());
  addRegions(viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>());
  for (auto *sequence :
       viewSelection.getRegionSequences<juce::ARARegionSequence>())
    addSequence(sequence);

  auto *docController = editorView->getDocumentController();
  if (docController == nullptr)
    return regions;

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<PitchNetDocumentController>(
          docController);

  // What the document controller has been working with.
  if (pitchDocController != nullptr) {
    addRegions(pitchDocController->getCurrentPlaybackRegions());
    if (auto *current = pitchDocController->getCurrentPlaybackRegion())
      addRegions({current});
  }

  auto *document =
      static_cast<juce::ARADocument *>(docController->getDocument());

  // Every clip on each track we touched. This is what makes the other regions
  // of a track appear when the host only told us about one of them.
  for (size_t i = 0; i < sequences.size(); ++i)
    addRegions(sequences[i]->getPlaybackRegions<juce::ARAPlaybackRegion>());

  // Nothing anywhere: fall back to the whole document, so the card still lists
  // what PitchNet has detected even before a selection reaches us.
  if (regions.empty() && document != nullptr) {
    for (auto *sequence :
         document->getRegionSequences<juce::ARARegionSequence>())
      if (sequence != nullptr)
        addRegions(sequence->getPlaybackRegions<juce::ARAPlaybackRegion>());
  }

  // Timeline order, so the list reads the way the arrangement does.
  std::stable_sort(regions.begin(), regions.end(),
                   [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
                     return a->getStartInPlaybackTime() <
                            b->getStartInPlaybackTime();
                   });
  return regions;
}

void PitchNetAudioProcessorEditor::refreshAraRegionList() {
  if (mainView == nullptr || getARAEditorView() == nullptr)
    return;

  const auto regions = collectAraPlaybackRegions();

  std::vector<MainViewRegionEntry> entries;
  entries.reserve(regions.size());

  int index = 0;
  for (auto *region : regions) {
    ++index;

    juce::String name;
    // getEffectiveName() falls back through modification to audio source, so
    // this is the name the host shows for the clip wherever it set one.
    if (const char *effectiveName = region->getEffectiveName())
      name = juce::String::fromUTF8(effectiveName).trim();
    if (name.isEmpty())
      name = "Region " + juce::String(index);

    // Two clips of the same take share a name; number the repeats so the list
    // stays usable.
    int duplicates = 0;
    for (const auto &existing : entries)
      if (existing.name == name || existing.name.startsWith(name + " ("))
        ++duplicates;
    if (duplicates > 0)
      name << " (" << (duplicates + 1) << ")";

    // The list selects a playback region, so it is keyed by the region
    // selector. Keying it by the modification would give every sibling of a
    // split clip the same entry key: none of them could be told apart, and all
    // of them would render as active.
    entries.push_back({pitchnetRegionSelector(*region), name});
  }

  const auto activeSelector = audioProcessor.getActiveAraRegionSelector();

  audioProcessor.updateAraTrackAnalysisQueue(regions);
  publishAraRegionPreviews(regions, entries, activeSelector);
  publishAraTrackList(regions, entries, activeSelector);

  juce::String signature = activeSelector;
  for (const auto &entry : entries)
    signature << "\n" << entry.key << "\t" << entry.name;

  if (regionListPublished && signature == lastPublishedRegionSignature)
    return;

  regionListPublished = true;
  lastPublishedRegionSignature = signature;

  // Only fires when the list actually changes, so this stays quiet - one line
  // per change saying what the region list holds and where it came from.
  juce::String diagnostic;
  diagnostic << "ARA region list: " << static_cast<int>(entries.size())
             << " region(s), active='" << activeSelector << "'";
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    diagnostic << " renderer="
               << static_cast<int>(
                      renderer->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  if (auto *editorRenderer = audioProcessor.getEditorRenderer())
    diagnostic << " editorRenderer="
               << static_cast<int>(
                      editorRenderer
                          ->getPlaybackRegions<juce::ARAPlaybackRegion>()
                          .size());
  diagnostic << " docController="
             << static_cast<int>(documentControllerRegionCount())
             << " document=" << static_cast<int>(documentRegionCount());
  for (size_t i = 0; i < entries.size() && i < regions.size(); ++i)
    diagnostic << "\n    [" << static_cast<int>(i) << "] '" << entries[i].name
               << "' " << juce::String(regions[i]->getStartInPlaybackTime(), 3)
               << "s key=" << entries[i].key;
  LOG(diagnostic);
}

namespace {
// Session-local identity of a track (region sequence) for the track list.
// Never stored; resolved by re-walking the document's live sequences.
juce::String araTrackKey(juce::ARARegionSequence *sequence) {
  return juce::String::toHexString(static_cast<juce::int64>(
      reinterpret_cast<std::uintptr_t>(sequence)));
}

// The track colour the DAW sends through ARA. Hosts set it on the region
// sequence (the track) itself; some only colour the clips, so fall back to the
// first clip's colour on that track. hasHostColour is false when the DAW sent
// neither; the UI then uses its default colour.
juce::Colour araTrackColour(juce::ARARegionSequence &sequence,
                            bool &hasHostColour,
                            const char **source = nullptr) {
  const auto toColour = [](const ARA::ARAColor *araColour) {
    return juce::Colour::fromFloatRGBA(araColour->r, araColour->g,
                                       araColour->b, 1.0f);
  };
  hasHostColour = true;
  if (const ARA::ARAColor *araColour = sequence.getColor()) {
    if (source != nullptr)
      *source = "track";
    return toColour(araColour);
  }
  for (auto *region : sequence.getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (region != nullptr)
      if (const ARA::ARAColor *regionColour = region->getColor()) {
        if (source != nullptr)
          *source = "clip";
        return toColour(regionColour);
      }
  hasHostColour = false;
  if (source != nullptr)
    *source = "none";
  return {};
}
} // namespace

void PitchNetAudioProcessorEditor::activateAraTrackByKey(
    const juce::String &trackKey) {
  if (trackKey.isEmpty())
    return;

  auto *editorView = getARAEditorView();
  auto *docController =
      editorView != nullptr ? editorView->getDocumentController() : nullptr;
  auto *document =
      docController != nullptr
          ? static_cast<juce::ARADocument *>(docController->getDocument())
          : nullptr;
  if (document == nullptr)
    return;

  // Resolve against live sequences: the track may have been removed since the
  // list was published.
  juce::ARARegionSequence *sequence = nullptr;
  for (auto *candidate :
       document->getRegionSequences<juce::ARARegionSequence>())
    if (candidate != nullptr && araTrackKey(candidate) == trackKey) {
      sequence = candidate;
      break;
    }

  if (sequence == nullptr) {
    lastPublishedTrackSignature.clear();
    return;
  }

  std::vector<juce::ARAPlaybackRegion *> candidates;
  for (auto *region : sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
    if (region != nullptr && region->getAudioModification() != nullptr &&
        region->getAudioModification()->getAudioSource() != nullptr)
      candidates.push_back(region);

  // A track with no usable clip has nothing to put on the canvas.
  if (candidates.empty())
    return;

  juce::ARAPlaybackRegion *target = nullptr;
  if (const auto remembered = lastActiveRegionByTrack.find(trackKey);
      remembered != lastActiveRegionByTrack.end())
    for (auto *region : candidates)
      if (pitchnetRegionSelector(*region) == remembered->second) {
        target = region;
        break;
      }

  if (target == nullptr)
    target = *std::min_element(
        candidates.begin(), candidates.end(),
        [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
          return a->getStartInPlaybackTime() < b->getStartInPlaybackTime();
        });

  activateAraRegion(target, true);
}

void PitchNetAudioProcessorEditor::publishAraTrackList(
    const std::vector<juce::ARAPlaybackRegion *> &regions,
    const std::vector<MainViewRegionEntry> &entries,
    const juce::String &activeSelector) {
  if (mainView == nullptr)
    return;

  auto *editorView = getARAEditorView();
  auto *docController =
      editorView != nullptr ? editorView->getDocumentController() : nullptr;
  auto *document =
      docController != nullptr
          ? static_cast<juce::ARADocument *>(docController->getDocument())
          : nullptr;
  if (document == nullptr)
    return;

  // The active track is the one holding the region the canvas shows. Before a
  // region is active, fall back to the track this instance renders.
  juce::ARARegionSequence *activeSequence = nullptr;
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i)
    if (entries[i].key == activeSelector) {
      activeSequence = regions[i]->getRegionSequence();
      break;
    }
  if (activeSequence == nullptr)
    if (auto *renderer = audioProcessor.getPlaybackRenderer())
      for (auto *region :
           renderer->getPlaybackRegions<juce::ARAPlaybackRegion>())
        if (region != nullptr && region->getRegionSequence() != nullptr) {
          activeSequence = region->getRegionSequence();
          break;
        }

  // Groups are kept in the tracks' clips: read those not read yet.
  readAraGroups(collectAraClips());

  // The ARA document only holds region sequences for tracks PitchNet is on.
  auto sequences = document->getRegionSequences<juce::ARARegionSequence>();
  sequences.erase(std::remove(sequences.begin(), sequences.end(), nullptr),
                  sequences.end());
  // Host track order.
  std::stable_sort(sequences.begin(), sequences.end(),
                   [](juce::ARARegionSequence *a, juce::ARARegionSequence *b) {
                     return a->getOrderIndex() < b->getOrderIndex();
                   });

  const auto keyFor = araTrackKey;

  std::vector<MainViewTrackEntry> tracks;
  tracks.reserve(sequences.size());
  int index = 0;
  juce::String colourDiagnostic;
  for (auto *sequence : sequences) {
    ++index;

    juce::String name;
    if (const char *sequenceName = sequence->getName())
      name = juce::String::fromUTF8(sequenceName).trim();
    if (name.isEmpty())
      name = "Track " + juce::String(index);

    const char *colourSource = "none";
    bool hasHostColour = false;
    const juce::Colour colour =
        araTrackColour(*sequence, hasHostColour, &colourSource);

    const auto key = keyFor(sequence);
    tracks.push_back({key, name, colour, hasHostColour,
                      pinnedTrackKeys.count(key) > 0});
    tracks.back().coEdited = araCoEditTrackKeys.count(key) > 0;
    if (const auto group = araGroups.find(key); group != araGroups.end()) {
      tracks.back().groupId = group->second.id;
      tracks.back().role = group->second.role;
      tracks.back().unisonOn = group->second.unisonOn;
      tracks.back().unisonStrength = group->second.unisonStrength;
      tracks.back().timingOn = group->second.timingOn;
      tracks.back().timingStrength = group->second.timingStrength;
    }
    colourDiagnostic << "\n    '" << name << "' colour=";
    if (hasHostColour)
      colourDiagnostic << "#" << colour.toDisplayString(false) << " ("
                       << colourSource << ")";
    else
      colourDiagnostic << "none (DAW sent no colour)";
  }

  const juce::String activeKey =
      activeSequence != nullptr ? keyFor(activeSequence) : juce::String();
  araActiveTrackKey = activeKey;

  // A track that becomes active is edited (it loses a ghost or Edit it
  // had); while it stays active it can be switched off or made a ghost.
  // Forget pins of tracks that no longer exist.
  if (activeKey != araPreviousActiveKey) {
    araPreviousActiveKey = activeKey;
    pinnedTrackKeys.erase(activeKey);
    araCoEditTrackKeys.erase(activeKey);
    if (!araOwnTrackEditable) {
      araOwnTrackEditable = true;
      mainView->setOwnTrackEditable(true);
      lastPublishedPreviewSignature.clear();
    }
  }
  for (auto it = pinnedTrackKeys.begin(); it != pinnedTrackKeys.end();) {
    const bool exists =
        std::any_of(tracks.begin(), tracks.end(),
                    [&](const MainViewTrackEntry &t) { return t.key == *it; });
    it = exists ? std::next(it) : pinnedTrackKeys.erase(it);
  }
  for (auto &track : tracks) {
    track.pinned = pinnedTrackKeys.count(track.key) > 0;
    if (track.key == activeKey)
      track.coEdited = araOwnTrackEditable;
  }
  if (activeKey.isNotEmpty() && activeSelector.isNotEmpty())
    lastActiveRegionByTrack[activeKey] = activeSelector;

  juce::String signature = activeKey;
  for (const auto &track : tracks)
    signature << "\n" << track.key << "\t" << track.name << "\t"
              << (track.hasHostColour ? track.colour.toString() : "-") << "\t"
              << (track.pinned ? "P" : "-") << (track.coEdited ? "E" : "-")
              << "\tG" << track.groupId << ":" << track.role
              << ":" << (track.unisonOn ? 1 : 0) << juce::String(track.unisonStrength, 3)
              << ":" << (track.timingOn ? 1 : 0) << juce::String(track.timingStrength, 3);

  if (signature == lastPublishedTrackSignature)
    return;
  lastPublishedTrackSignature = signature;
  mainView->updateTrackList(tracks, activeKey);

  // One line per change: where each track's colour came from, so a host that
  // sends no ARA colour is easy to spot.
  LOG("ARA track list: " + juce::String(static_cast<int>(tracks.size())) +
      " track(s)" + colourDiagnostic);
}

void PitchNetAudioProcessorEditor::setAraOwnTrackEditable(bool editable) {
  araOwnTrackEditable = editable;
  if (editable)
    pinnedTrackKeys.erase(araActiveTrackKey); // edited, not a ghost
  if (mainView != nullptr)
    mainView->setOwnTrackEditable(editable);
  lastPublishedTrackSignature.clear();
  lastPublishedPreviewSignature.clear();
  regionPreviewCache.clear();
  refreshAraRegionList();
}

// ---------------------------------------------------------------------------
// Lead and doubles in ARA hosts (REAPER). The same groups, Unison and Timing
// as in hosts without ARA, but a track is a region sequence: its group is
// kept in its clips' projects, and each clip of a double follows the lead
// clips that play at the same time.

std::vector<std::pair<juce::String, std::vector<PitchNetAudioProcessorEditor::AraClip>>>
PitchNetAudioProcessorEditor::collectAraClips() {
  std::vector<std::pair<juce::String, std::vector<AraClip>>> tracks;
  std::map<juce::String, size_t> trackIndex;
  std::set<juce::String> seenModifications;
  auto regions = collectAraPlaybackRegions();
  std::stable_sort(regions.begin(), regions.end(),
                   [](juce::ARAPlaybackRegion *a, juce::ARAPlaybackRegion *b) {
                     const auto *sa = a->getRegionSequence(), *sb = b->getRegionSequence();
                     const int oa = sa != nullptr ? sa->getOrderIndex() : 0;
                     const int ob = sb != nullptr ? sb->getOrderIndex() : 0;
                     if (oa != ob)
                       return oa < ob;
                     return a->getStartInPlaybackTime() < b->getStartInPlaybackTime();
                   });
  for (auto *region : regions) {
    auto *sequence = region != nullptr ? region->getRegionSequence() : nullptr;
    auto *modification = region != nullptr
                             ? region->getAudioModification<PitchNetAudioModification>()
                             : nullptr;
    if (sequence == nullptr || modification == nullptr)
      continue;
    const auto trackKey = araTrackKey(sequence);
    if (trackIndex.count(trackKey) == 0) {
      trackIndex[trackKey] = tracks.size();
      tracks.push_back({trackKey, {}});
    }
    AraClip clip;
    clip.region = region;
    clip.modification = modification;
    clip.key = pitchnetModificationKey(*modification);
    // One clip per modification: copies of a clip share its edits.
    if (!seenModifications.insert(clip.key).second)
      continue;
    if (!audioProcessor.araRegionProjectNeedsSourceHydration(clip.key))
      clip.project = const_cast<Project *>(
          audioProcessor.getAraProjectForPreview(modification, clip.revision, clip.fromCanvas));
    clip.startSeconds = region->getStartInPlaybackTime();
    clip.endSeconds = region->getEndInPlaybackTime();
    clip.offsetSeconds = region->getStartInPlaybackTime() - region->getStartInAudioModificationTime();
    tracks[trackIndex[trackKey]].second.push_back(clip);
  }
  return tracks;
}

void PitchNetAudioProcessorEditor::readAraGroups(
    const std::vector<std::pair<juce::String, std::vector<AraClip>>> &tracks) {
  for (const auto &[trackKey, clips] : tracks) {
    if (araGroupsRead.count(trackKey) != 0)
      continue;
    bool anyProject = false;
    for (const auto &clip : clips)
      if (clip.project != nullptr) {
        anyProject = true;
        if (clip.project->getDoubleGroup().id.isNotEmpty()) {
          araGroups[trackKey] = clip.project->getDoubleGroup();
          break;
        }
      }
    if (anyProject)
      araGroupsRead.insert(trackKey);
  }
}

void PitchNetAudioProcessorEditor::persistAraClip(const AraClip &clip) {
  if (clip.modification == nullptr)
    return;
  juce::MemoryBlock archive;
  if (audioProcessor.serializeAraRegionProject(clip.key, archive))
    clip.modification->setProjectArchive(archive.getData(), archive.getSize());
}

void PitchNetAudioProcessorEditor::setAraTrackGroup(const juce::String &trackKey,
                                                    const Project::DoubleGroup &group) {
  if (group.id.isEmpty())
    araGroups.erase(trackKey);
  else
    araGroups[trackKey] = group;
  araGroupsRead.insert(trackKey);
  for (auto &[key, clips] : collectAraClips())
    if (key == trackKey)
      for (const auto &clip : clips)
        if (clip.project != nullptr) {
          clip.project->setDoubleGroup(group.id.isEmpty() ? Project::DoubleGroup{} : group);
          clip.project->setModified(true);
        }
  persistAraTrackLater(trackKey);
  lastPublishedTrackSignature.clear();
  refreshAraRegionListSoon();
}

void PitchNetAudioProcessorEditor::persistAraTrackLater(const juce::String &trackKey) {
  araPersistPending.insert(trackKey);
  araPersistDueMs = juce::Time::getMillisecondCounterHiRes() + 400.0;
}

void PitchNetAudioProcessorEditor::flushAraPersist(bool now) {
  if (araPersistPending.empty())
    return;
  if (!now && juce::Time::getMillisecondCounterHiRes() < araPersistDueMs)
    return;
  const auto keys = std::exchange(araPersistPending, {});
  PerfTrace::Scope scope("ara persist " + juce::String(static_cast<int>(keys.size())) + " track(s)");
  for (const auto &[key, clips] : collectAraClips())
    if (keys.count(key) != 0)
      for (const auto &clip : clips)
        if (clip.project != nullptr)
          persistAraClip(clip);
}

void PitchNetAudioProcessorEditor::refreshAraRegionListSoon() {
  // Many changes in one go (a whole group, a slider): one refresh after them.
  if (araRefreshQueued)
    return;
  araRefreshQueued = true;
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safe(this);
  juce::MessageManager::callAsync([safe] {
    if (safe == nullptr)
      return;
    safe->araRefreshQueued = false;
    safe->refreshAraRegionList();
  });
}

void PitchNetAudioProcessorEditor::setupAraGroupCallbacks() {
  if (mainView == nullptr)
    return;
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safe(this);
  TrackGroupCallbacks callbacks;
  callbacks.makeGroup = [safe](const juce::String &lead, const std::vector<juce::String> &doubles) {
    if (safe == nullptr)
      return;
    Project::DoubleGroup group;
    group.id = juce::Uuid().toString();
    group.role = 1;
    safe->setAraTrackGroup(lead, group);
    group.role = 2;
    for (const auto &key : doubles)
      if (key != lead)
        safe->setAraTrackGroup(key, group);
  };
  callbacks.addToGroup = [safe](const juce::String &key, const juce::String &groupId) {
    if (safe == nullptr)
      return;
    Project::DoubleGroup group;
    group.id = groupId;
    group.role = 2;
    safe->setAraTrackGroup(key, group);
  };
  callbacks.removeFromGroup = [safe](const juce::String &key) {
    if (safe == nullptr)
      return;
    const auto it = safe->araGroups.find(key);
    if (it == safe->araGroups.end())
      return;
    const auto group = it->second;
    // Taking out the lead ends the whole group.
    std::vector<juce::String> keys{key};
    if (group.role == 1)
      for (const auto &[other, g] : safe->araGroups)
        if (g.id == group.id && other != key)
          keys.push_back(other);
    for (const auto &k : keys)
      safe->setAraTrackGroup(k, {});
  };
  callbacks.setLead = [safe](const juce::String &key) {
    if (safe == nullptr)
      return;
    const auto it = safe->araGroups.find(key);
    if (it == safe->araGroups.end())
      return;
    auto group = it->second;
    // The old lead hands over the switches and becomes a double.
    for (const auto &[other, g] : std::map<juce::String, Project::DoubleGroup>(safe->araGroups))
      if (g.id == group.id && g.role == 1 && other != key) {
        auto asDouble = g;
        group.unisonOn = g.unisonOn;
        group.unisonStrength = g.unisonStrength;
        group.timingOn = g.timingOn;
        group.timingStrength = g.timingStrength;
        asDouble.role = 2;
        safe->setAraTrackGroup(other, asDouble);
      }
    group.role = 1;
    safe->setAraTrackGroup(key, group);
  };
  auto setSwitch = [safe](const juce::String &groupId, bool timing, bool on, float strength) {
    if (safe == nullptr)
      return;
    for (const auto &[key, g] : std::map<juce::String, Project::DoubleGroup>(safe->araGroups))
      if (g.id == groupId) {
        auto next = g;
        if (timing) {
          next.timingOn = on;
          next.timingStrength = strength;
        } else {
          next.unisonOn = on;
          next.unisonStrength = strength;
        }
        safe->setAraTrackGroup(key, next);
      }
  };
  callbacks.setUnison = [setSwitch](const juce::String &groupId, bool on, float strength) {
    setSwitch(groupId, false, on, strength);
  };
  callbacks.setTiming = [setSwitch](const juce::String &groupId, bool on, float strength) {
    setSwitch(groupId, true, on, strength);
  };
  callbacks.findDoubles = [safe] {
    if (safe != nullptr)
      safe->findAraDoubles();
  };
  mainView->setTrackGroupCallbacks(std::move(callbacks));
}

void PitchNetAudioProcessorEditor::findAraDoubles() {
  // Every track's notes on the song's timeline, by their own pitch.
  const auto tracks = collectAraClips();
  std::vector<std::vector<DoubleTuning::TNote>> notes;
  std::vector<juce::String> keyOf;
  for (const auto &[trackKey, clips] : tracks) {
    std::vector<DoubleTuning::TNote> trackNotes;
    for (const auto &clip : clips) {
      if (clip.project == nullptr)
        continue;
      const double hop = LeadProjection::frameSeconds(*clip.project);
      const int shift = static_cast<int>(std::lround(clip.offsetSeconds / hop));
      for (const auto &note : clip.project->getNotes())
        if (!note.isRest() && note.getEndFrame() > note.getStartFrame())
          trackNotes.push_back({note.getStartFrame() + shift, note.getEndFrame() + shift,
                                note.hasUnisonSource() ? note.getUnisonSourceMidi() : note.getMidiNote()});
    }
    if (trackNotes.empty())
      continue;
    std::sort(trackNotes.begin(), trackNotes.end(),
              [](const DoubleTuning::TNote &a, const DoubleTuning::TNote &b) { return a.start < b.start; });
    notes.push_back(std::move(trackNotes));
    keyOf.push_back(trackKey);
  }
  const auto groups = DoubleTuning::detectGroups(notes);
  if (groups.empty()) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Find doubles",
        "No doubles found. Let the tracks finish analysis first, or make a group by hand: "
        "select the tracks and press Group selected.");
    return;
  }
  std::map<juce::String, juce::String> names;
  if (auto *editorView = getARAEditorView())
    if (auto *docController = editorView->getDocumentController())
      if (auto *document = static_cast<juce::ARADocument *>(docController->getDocument()))
        for (auto *sequence : document->getRegionSequences<juce::ARARegionSequence>())
          if (sequence != nullptr && sequence->getName() != nullptr)
            names[araTrackKey(sequence)] = juce::String::fromUTF8(sequence->getName());
  auto nameOf = [&](int track) {
    const auto &key = keyOf[static_cast<size_t>(track)];
    return names.count(key) != 0 && names[key].isNotEmpty() ? names[key]
                                                            : "Track " + juce::String(track + 1);
  };
  juce::String message;
  std::vector<std::pair<juce::String, std::vector<juce::String>>> plan;
  for (const auto &group : groups) {
    message << "Lead: " << nameOf(group.lead) << "\nDoubles: ";
    std::vector<juce::String> doubles;
    for (size_t d = 0; d < group.doubles.size(); ++d) {
      message << (d > 0 ? ", " : "") << nameOf(group.doubles[d]);
      doubles.push_back(keyOf[static_cast<size_t>(group.doubles[d])]);
    }
    message << "\n\n";
    plan.push_back({keyOf[static_cast<size_t>(group.lead)], doubles});
  }
  message << "Create these groups?";
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safe(this);
  juce::AlertWindow::showOkCancelBox(
      juce::AlertWindow::QuestionIcon, "Find doubles", message, "Create", "Cancel", nullptr,
      juce::ModalCallbackFunction::create([safe, plan](int result) {
        if (result != 1 || safe == nullptr)
          return;
        for (const auto &[lead, doubles] : plan) {
          Project::DoubleGroup group;
          group.id = juce::Uuid().toString();
          group.role = 1;
          safe->setAraTrackGroup(lead, group);
          group.role = 2;
          for (const auto &key : doubles)
            safe->setAraTrackGroup(key, group);
        }
      }));
}

void PitchNetAudioProcessorEditor::syncAraDoubles() {
  if (araGroups.empty() && araDoubleApplied.empty() && araCoEditTrackKeys.empty())
    return;
  const auto tracks = collectAraClips();
  readAraGroups(tracks);

  // A group's clips must be analysed (and their sound loaded) before they
  // can follow or be followed; the window only analyses the clips it shows,
  // so ask for the others, one at a time (the analysis worker takes one).
  if (auto *editorView = getARAEditorView())
    if (auto *docController = editorView->getDocumentController())
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(docController))
        if (!audioProcessor.isAraCanvasWorkerRunning()) {
          bool requested = false;
          for (const auto &[trackKey, clips] : tracks) {
            const auto group = araGroups.find(trackKey);
            const bool grouped = group != araGroups.end() && group->second.id.isNotEmpty();
            if (requested || (!grouped && araCoEditTrackKeys.count(trackKey) == 0))
              continue;
            for (const auto &clip : clips)
              if (clip.project == nullptr && clip.region != nullptr) {
                // REAPER lets a plug-in read a track's audio only once the
                // song has played; until then nothing can be analysed.
                auto *source = clip.region->getAudioModification() != nullptr
                                   ? clip.region->getAudioModification()->getAudioSource()
                                   : nullptr;
                if (source != nullptr && !source->isSampleAccessEnabled()) {
                  mainView->setStatusMessage(
                      "Press Play once: the DAW lets PitchNet read the other tracks after playback.");
                } else {
                  mainView->setStatusMessage("Analysing the other tracks...");
                  pitchDocController->requestRegionCanvasAnalysis(clip.region);
                }
                requested = true;
                break;
              }
          }
        }

  auto hasFollowingNotes = [](const Project &project) {
    for (const auto &note : project.getNotes())
      if (note.hasUnisonSource() || note.hasAlignSource())
        return true;
    return false;
  };
  // Lead tracks and their state: settings plus each clip's revision. A lead
  // still changing (a drag, a macro preview) is followed once it is still.
  std::map<juce::String, const std::vector<AraClip> *> leadClips;
  std::map<juce::String, Project::DoubleGroup> leadGroup;
  std::map<juce::String, juce::String> leadState;
  for (const auto &[trackKey, clips] : tracks) {
    const auto it = araGroups.find(trackKey);
    if (it == araGroups.end() || it->second.role != 1)
      continue;
    const auto &g = it->second;
    juce::String state;
    state << (g.unisonOn ? "U" : "-") << juce::String(g.unisonStrength, 3)
          << (g.timingOn ? "T" : "-") << juce::String(g.timingStrength, 3);
    for (const auto &clip : clips)
      state << ";" << clip.key << ":" << juce::String(static_cast<juce::int64>(clip.revision))
            << (clip.project != nullptr ? "" : "?");
    leadClips[g.id] = &clips;
    leadGroup[g.id] = g;
    leadState[g.id] = state;
  }

  std::map<const Project *, TimingAligner::Features> features; // per lead clip, this pass
  // About 25 ms of following per tick (eight a second): a group of doubles
  // catches up over a few ticks instead of freezing the window in one.
  const double passStarted = juce::Time::getMillisecondCounterHiRes();
  for (const auto &[trackKey, clips] : tracks) {
    const auto groupIt = araGroups.find(trackKey);
    const bool isDouble = groupIt != araGroups.end() && groupIt->second.role == 2 &&
                          leadClips.count(groupIt->second.id) != 0;
    const Project::DoubleGroup group = isDouble ? leadGroup[groupIt->second.id] : Project::DoubleGroup{};
    const bool on = isDouble && (group.unisonOn || group.timingOn);
    const juce::String leadKey = isDouble ? group.id : juce::String();
    if (on && araLeadSeen[leadKey] != leadState[leadKey]) {
      araLeadSeen[leadKey] = leadState[leadKey]; // still changing: next tick
      continue;
    }
    for (const auto &clip : clips) {
      if (clip.project == nullptr)
        continue;
      if (juce::Time::getMillisecondCounterHiRes() - passStarted > 25.0)
        return; // the rest on the next tick
      if (!on && !hasFollowingNotes(*clip.project)) {
        araDoubleApplied.erase(clip.key);
        continue;
      }
      const juce::String state = (on ? leadState[leadKey] : juce::String("off")) + "|" +
                                 juce::String(static_cast<juce::int64>(clip.revision));
      if (araDoubleApplied[clip.key] == state)
        continue;
      araDoubleApplied[clip.key] = state;

      // The lead, laid out in this clip's frames.
      Project lead = LeadProjection::makeEmpty(*clip.project);
      if (on) {
        const LeadProjection::Placement dubAt{clip.offsetSeconds, clip.startSeconds, clip.endSeconds};
        for (const auto &leadClip : *leadClips[leadKey]) {
          if (leadClip.project == nullptr || leadClip.endSeconds <= clip.startSeconds ||
              leadClip.startSeconds >= clip.endSeconds)
            continue;
          auto found = features.find(leadClip.project);
          if (found == features.end()) {
            const auto &audio = leadClip.project->getAudioData();
            found = features.emplace(leadClip.project,
                                     TimingAligner::fromMel(audio.melSpectrogram, audio.voicedMask)).first;
          }
          LeadProjection::addClip(lead, *leadClip.project, found->second,
                                  {leadClip.offsetSeconds, leadClip.startSeconds, leadClip.endSeconds}, dubAt);
        }
      }
      PerfTrace::Scope perfScope("ara double " + clip.key);
      if (!UnisonCoordinator::applyToDouble(*clip.project, on ? &lead : nullptr, on && group.unisonOn,
                                            group.unisonStrength, on && group.timingOn,
                                            group.timingStrength))
        continue;
      if (clip.fromCanvas) {
        mainView->notifyProjectEditedExternally(true);
      } else {
        persistAraTrackLater(trackKey);
        audioProcessor.requestPluginProjectRender(*clip.project, clip.modification);
        regionPreviewCache.clear();
        lastPublishedPreviewSignature.clear();
      }
    }
  }
}

void PitchNetAudioProcessorEditor::publishAraRegionPreviews(
    const std::vector<juce::ARAPlaybackRegion *> &regions,
    const std::vector<MainViewRegionEntry> &entries,
    const juce::String &activeSelector) {
  if (mainView == nullptr)
    return;

  // Clip mode shows one modification in its own time and draws no other
  // regions. Publish nothing, and skip the work of building previews.
  if (!mainView->isTrackViewMode()) {
    regionPreviewCache.clear();
    if (lastPublishedPreviewSignature != "clip") {
      lastPublishedPreviewSignature = "clip";
      mainView->updateRegionPreviews(nullptr);
    }
    return;
  }

  // Only the track of the active region: other tracks have their own plug-in
  // instance and must not be drawn over this one.
  juce::ARARegionSequence *activeSequence = nullptr;
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i)
    if (entries[i].key == activeSelector) {
      activeSequence = regions[i]->getRegionSequence();
      break;
    }

  auto previews = std::make_shared<std::vector<MainViewRegionPreview>>();
  juce::String signature = "track\n" + activeSelector;
  std::map<juce::String, RegionPreviewCacheEntry> nextCache;

  // Builds (or reuses) one region's preview and appends it.
  const auto addPreview = [&](juce::ARAPlaybackRegion *region,
                              const juce::String &regionKey,
                              const juce::String &regionName, bool isActive,
                              bool pinned, juce::Colour outlineColour) {
    // regionKey is the region selector (which window); edits are owned by
    // the modification, so the Project is looked up by the modification key.
    const double start = std::max(0.0, region->getStartInPlaybackTime());
    const double end = std::max(start, region->getEndInPlaybackTime());
    double startInModification = 0.0;
    if (auto *modification = region->getAudioModification())
      if (auto *source = modification->getAudioSource())
        if (source->getSampleRate() > 0.0)
          startInModification =
              static_cast<double>(
                  region->getStartInAudioModificationSamples()) /
              source->getSampleRate();

    std::uint64_t revision = 0;
    bool fromCanvas = false;
    // The active region is drawn live from the canvas; its preview only
    // carries its range. Siblings of the active modification read the live
    // canvas Project, other modifications their stored Project.
    // Track gestures need the live source binding even when the region is
    // drawn directly from the canvas rather than from a cached window.
    const Project* sourceProject = audioProcessor.getAraProjectForPreview(
        region->getAudioModification<PitchNetAudioModification>(), revision, fromCanvas);
    const Project* project = isActive ? nullptr : sourceProject;

    RegionPreviewCacheEntry cacheEntry;
    const auto cached = regionPreviewCache.find(regionKey);
    const bool reusable =
        cached != regionPreviewCache.end() && !isActive &&
        cached->second.project == project &&
        cached->second.fromCanvas == fromCanvas &&
        cached->second.revision == revision &&
        juce::approximatelyEqual(cached->second.startSeconds, start) &&
        juce::approximatelyEqual(cached->second.endSeconds, end) &&
        juce::approximatelyEqual(cached->second.startInModificationSeconds,
                                 startInModification);

    if (reusable) {
      cacheEntry = cached->second;
    } else {
      cacheEntry.project = project;
      cacheEntry.fromCanvas = fromCanvas;
      cacheEntry.revision = revision;
      cacheEntry.startSeconds = start;
      cacheEntry.endSeconds = end;
      cacheEntry.startInModificationSeconds = startInModification;
      if (project != nullptr)
        cacheEntry.preview =
            buildRegionPreview(*project, start, end, startInModification);
    }

    auto &preview = cacheEntry.preview;
    preview.key = regionKey;
    preview.name = regionName;
    preview.startSeconds = start;
    preview.endSeconds = end;
    preview.active = isActive;
    if (isActive) {
      preview.sourceProject = sourceProject;
      preview.sourceFirstFrame = 0;
      preview.projectHostOffsetSeconds = start - startInModification;
    }
    preview.pinned = pinned;
    if (pinned && preview.project)
      for (auto& note : preview.project->getNotes()) note.setSelected(false);
    preview.outlineColour = outlineColour;
    previews->push_back(preview);

    signature << "\n" << regionKey << "\t" << regionName << "\t"
              << juce::String(start, 6) << "\t" << juce::String(end, 6)
              << "\t" << juce::String(startInModification, 6) << "\t"
              << (isActive ? "A" : "-") << (fromCanvas ? "C" : "S") << "\t"
              << juce::String::toHexString(
                     static_cast<juce::int64>(
                         reinterpret_cast<std::uintptr_t>(sourceProject)))
              << "\t" << juce::String(static_cast<juce::int64>(revision));
    if (pinned)
      signature << "\tP" << outlineColour.toString();
    nextCache[regionKey] = std::move(cacheEntry);
  };

  // The active track: edited (the canvas draws the active clip), a ghost
  // (every clip drawn grey) or off (none of it).
  const bool activeGhost =
      !araOwnTrackEditable && activeSequence != nullptr &&
      pinnedTrackKeys.count(araTrackKey(activeSequence)) != 0;
  signature << (araOwnTrackEditable ? "|own-on" : activeGhost ? "|own-ghost" : "|own-off");
  juce::Colour activeOutline;
  if (activeGhost) {
    MainViewTrackEntry track;
    track.colour = araTrackColour(*activeSequence, track.hasHostColour);
    activeOutline = resolveTrackColour(track, mainView->getUseDawTrackColour());
  }
  for (size_t i = 0; i < regions.size() && i < entries.size(); ++i) {
    auto *region = regions[i];
    if (activeSequence != nullptr &&
        region->getRegionSequence() != activeSequence)
      continue;
    if (araOwnTrackEditable)
      addPreview(region, entries[i].key, entries[i].name,
                 entries[i].key == activeSelector, false, {});
    else if (activeGhost)
      addPreview(region, entries[i].key, {}, false, true, activeOutline);
  }

  // Tracks open for editing (Edit): every clip, as an editable window
  // outlined in the track colour, like the active track's other clips.
  if (!araCoEditTrackKeys.empty()) {
    auto *editorView = getARAEditorView();
    auto *docController =
        editorView != nullptr ? editorView->getDocumentController() : nullptr;
    auto *document =
        docController != nullptr
            ? static_cast<juce::ARADocument *>(docController->getDocument())
            : nullptr;
    const bool useDawColour = mainView->getUseDawTrackColour();
    if (document != nullptr)
      for (auto *sequence :
           document->getRegionSequences<juce::ARARegionSequence>()) {
        if (sequence == nullptr || sequence == activeSequence ||
            araCoEditTrackKeys.count(araTrackKey(sequence)) == 0)
          continue;
        MainViewTrackEntry track;
        track.colour = araTrackColour(*sequence, track.hasHostColour);
        const auto outline = resolveTrackColour(track, useDawColour);
        juce::String trackName;
        if (const char *n = sequence->getName())
          trackName = juce::String::fromUTF8(n);
        for (auto *region :
             sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
          if (region != nullptr && region->getAudioModification() != nullptr)
            addPreview(region, pitchnetRegionSelector(*region), trackName, false, false,
                       outline);
      }
  }

  // Pinned tracks: every clip, drawn as notes outlined in the track colour.
  // Never the active track - its own clips are already here.
  if (!pinnedTrackKeys.empty()) {
    auto *editorView = getARAEditorView();
    auto *docController =
        editorView != nullptr ? editorView->getDocumentController() : nullptr;
    auto *document =
        docController != nullptr
            ? static_cast<juce::ARADocument *>(docController->getDocument())
            : nullptr;
    const bool useDawColour = mainView->getUseDawTrackColour();
    if (document != nullptr)
      for (auto *sequence :
           document->getRegionSequences<juce::ARARegionSequence>()) {
        if (sequence == nullptr || sequence == activeSequence ||
            pinnedTrackKeys.count(araTrackKey(sequence)) == 0)
          continue;
        MainViewTrackEntry track;
        track.colour = araTrackColour(*sequence, track.hasHostColour);
        const auto outline = resolveTrackColour(track, useDawColour);
        for (auto *region :
             sequence->getPlaybackRegions<juce::ARAPlaybackRegion>())
          if (region != nullptr && region->getAudioModification() != nullptr)
            addPreview(region, pitchnetRegionSelector(*region), {}, false, true,
                       outline);
      }
  }

  regionPreviewCache = std::move(nextCache);

  if (signature == lastPublishedPreviewSignature)
    return;
  lastPublishedPreviewSignature = signature;
  mainView->updateRegionPreviews(std::move(previews));
}

void PitchNetAudioProcessorEditor::activateAraRegionByKey(
    const juce::String &regionKey, bool focusView) {
  if (regionKey.isEmpty())
    return;

  // Match against freshly collected regions rather than a stored pointer: the
  // host may have destroyed a region since the list was last published.
  juce::ARAPlaybackRegion *target = nullptr;
  for (auto *region : collectAraPlaybackRegions()) {
    if (pitchnetRegionSelector(*region) == regionKey) {
      target = region;
      break;
    }
  }

  if (target == nullptr) {
    // Whatever the card is showing no longer exists; republish on the next tick.
    lastPublishedRegionSignature.clear();
    return;
  }

  activateAraRegion(target, focusView);
}

void PitchNetAudioProcessorEditor::activateAraRegion(
    juce::ARAPlaybackRegion *target, bool focusView) {
  if (target == nullptr)
    return;

  if (auto *araEditorView = getARAEditorView()) {
    if (auto *araDocController = araEditorView->getDocumentController()) {
      if (auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
              getSpecialisedDocumentController<PitchNetDocumentController>(
                  araDocController))
        pitchDocController->setCurrentPlaybackRegion(target);
    }
  }

  audioProcessor.setActiveAraRegion(target);
  if (focusView) {
    // The canvas is in modification time; the region's host position is not.
    const auto span = audioProcessor.activeRegionSpanInModificationTime();
    mainView->focusTimelineRange(span.first, span.second);
  }

  // The active key just changed. Publish now rather than on the next poll so
  // the canvas never shows the new region twice (as project and as preview)
  // or the old one not at all.
  lastPublishedRegionSignature.clear();
  refreshAraRegionList();
}

void PitchNetAudioProcessorEditor::onNewSelection(
    const juce::ARAViewSelection &viewSelection) {
  // Switch the canvas to whichever region the user selected in the host. Each
  // region/track carries its own persistent Project, so this swaps what is
  // shown and edited without disturbing the others. Prefer an explicitly
  // selected playback region; fall back to the effective selection (e.g. a
  // time-range/marquee selection that implies a set of regions).
  auto regions = viewSelection.getPlaybackRegions<juce::ARAPlaybackRegion>();
  const auto effective =
      viewSelection.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
  const auto sequences =
      viewSelection.getRegionSequences<juce::ARARegionSequence>();

  std::vector<juce::ARAPlaybackRegion *> rendererRegions;
  if (auto *renderer = audioProcessor.getPlaybackRenderer())
    rendererRegions = renderer->getPlaybackRegions<juce::ARAPlaybackRegion>();

  const auto isAssignedToThisInstance =
      [&rendererRegions](juce::ARAPlaybackRegion *region) {
        return rendererRegions.empty() ||
               std::find(rendererRegions.begin(), rendererRegions.end(),
                         region) != rendererRegions.end();
      };

  const auto pickAssignedRegion =
      [&isAssignedToThisInstance](
          const std::vector<juce::ARAPlaybackRegion *> &candidates)
      -> juce::ARAPlaybackRegion * {
    for (auto *candidate : candidates)
      if (candidate != nullptr && isAssignedToThisInstance(candidate))
        return candidate;

    return nullptr;
  };

  juce::ARAPlaybackRegion *target = nullptr;
  if (!regions.empty())
    target = pickAssignedRegion(regions);
  if (target == nullptr && !effective.empty())
    target = pickAssignedRegion(effective);
  if (target == nullptr && !sequences.empty()) {
    // Only a track/region-sequence was selected: focus its first region that
    // belongs to this plugin instance. AAX can report selections from the
    // shared ARA document, where blindly taking the first region sticks to the
    // first track/audio source.
    for (auto *sequence : sequences) {
      if (sequence == nullptr)
        continue;

      const auto &seqRegions =
          sequence->getPlaybackRegions<juce::ARAPlaybackRegion>();
      target = pickAssignedRegion(seqRegions);
      if (target != nullptr)
        break;
    }
  }

  if (target == nullptr) {
    if (!regions.empty())
      target = regions.front();
    else if (!effective.empty())
      target = effective.front();
    else if (!sequences.empty()) {
      const auto &seqRegions =
          sequences.front()->getPlaybackRegions<juce::ARAPlaybackRegion>();
      if (!seqRegions.empty())
        target = seqRegions.front();
    }
  }

  if (target != nullptr) {
    if (auto *araEditorView = getARAEditorView()) {
      if (auto *araDocController = araEditorView->getDocumentController()) {
        if (auto *pitchDocController =
                juce::ARADocumentControllerSpecialisation::
                    getSpecialisedDocumentController<
                        PitchNetDocumentController>(araDocController))
          pitchDocController->setCurrentPlaybackRegion(target);
      }
    }
    audioProcessor.setActiveAraRegion(target);
    // The canvas is in modification time; the region's host position is not.
    const auto span = audioProcessor.activeRegionSpanInModificationTime();
    mainView->focusTimelineRange(span.first, span.second);
  }

  // The selection is also what tells us which track's regions to list, so
  // update the Regions card now instead of waiting for the next poll.
  refreshAraRegionList();
}
#endif

void PitchNetAudioProcessorEditor::setupNonARAMode() {
  // Without ARA the plugin cannot drive the host transport, so play / stop /
  // cycle and cycle-range editing are disabled - capture is the only transport
  // action available here.
  mainView->setHostTransportControlAvailable(false);
  mainView->setRecordControlVisible(true);
  mainView->updateRecordArmState(target().isCaptureArmed());
  mainView->setOnRecordArmChanged([this](bool armed) {
    if (armed)
      target().startCapture();
    else
      target().stopCapture();
  });
  mainView->setOnRequestBackendRender([this](const Project &project) {
    target().requestPluginProjectRender(project);
  });
  mainView->setOnRequestBackendPreview(
      [this](const Project &project, int startFrame, int endFrame) {
        const auto &audioData = project.getAudioData();
        const double sampleRate =
            audioData.sampleRate > 0 ? static_cast<double>(audioData.sampleRate)
                                     : static_cast<double>(SAMPLE_RATE);
        const double start = static_cast<double>(startFrame) * HOP_SIZE / sampleRate;
        const double end = static_cast<double>(endFrame) * HOP_SIZE / sampleRate;
        target().setPreviewMixBed(buildPreviewMixBed(start, end));
        target().startPluginPreview(start, end);
      });
  // Editing Mix Fader, for previews played with the DAW stopped.
  mainView->setEditingMixAvailable(true);
  // Replace Ranges: where this track's audio replaces the DAW track.
  mainView->setReplaceRangesAvailable(true);
  mainView->setOnReplaceRangesEdited(
      [this](const std::vector<std::pair<double, double>> &ranges) {
        target().setReplaceRanges(ranges);
      });
  // MIDI export: this track, the tracks edited with it, the reference tracks.
  mainView->setMidiExportTracksProvider([this]() {
    std::vector<MidiExportTrack> tracks;
    const auto entries = InstanceRegistry::get().list();
    const auto nameOf = [&](std::uint64_t id) {
      for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id)
          return InstanceRegistry::displayName(entries[i], static_cast<int>(i));
      return juce::String();
    };
    if (auto *project = mainView->getProject())
      tracks.push_back({nameOf(targetId), project->getNotes()});
    for (const auto &entry : entries) {
      if (entry.id == targetId)
        continue;
      if (auto it = coEditTracks.find(entry.id); it != coEditTracks.end()) {
        tracks.push_back({nameOf(entry.id), it->second.project->getNotes()});
        continue;
      }
      if (entry.snapshot != nullptr &&
          pinnedInstanceKeys.count(InstanceRegistry::keyFor(entry.id)) > 0)
        tracks.push_back({nameOf(entry.id), entry.snapshot->getNotes()});
    }
    return tracks;
  });
  mainView->setOnStopBackendPreview(
      [this]() { target().stopPluginPreview(); });
  mainView->setOnRequestDragAudition(
      [this](const juce::AudioBuffer<float> &buffer, double sampleRate) {
        target().startPluginAudition(buffer, sampleRate);
      });
  mainView->setOnStopDragAudition(
      [this]() { target().stopPluginAudition(); });

  // Setup host transport control callbacks for non-ARA mode
  mainView->setOnRequestHostPlayState([this](bool shouldPlay) {
    target().requestHostPlayState(shouldPlay);
  });

  mainView->setOnRequestHostStop([this]() {
    target().requestHostStop();
  });

  mainView->setOnRequestHostSeek([this](double timeInSeconds) {
    target().requestHostSeek(timeInSeconds);
  });

  mainView->setOnRequestHostLoopRange(nullptr);

  setupHostTransportUiSync(true);

  // Track list: every PitchNet instance in this process.
  instanceTracksEnabled = true;
  mainView->setTrackListAvailable(true);
  mainView->setOnTrackPinChanged([this](const juce::String &key, bool pinned) {
    setReferenceTrack(key, pinned);
  });
  mainView->setOnTrackSelected([this](const juce::String &key) { editTrack(key); });
  mainView->setOnReferenceNoteDoubleClicked(
      [this](const juce::String &key) { editTrack(key); });
  mainView->setOnTrackTransferToggled(
      [this](const juce::String &key, bool armed) { setTrackTransfer(key, armed); });
  mainView->setOnTrackEditAdded([this](const juce::String &key) { toggleCoEdit(key); });
  {
    // Lead / doubles groups, kept by each track's processor (saved with the
    // host project) and shared through the instance registry.
    const auto ownerOf = [](const juce::String &key) {
      return static_cast<PitchNetAudioProcessor *>(
          InstanceRegistry::get().ownerOf(InstanceRegistry::idFromKey(key)));
    };
    TrackGroupCallbacks callbacks;
    callbacks.makeGroup = [ownerOf](const juce::String &lead,
                                    const std::vector<juce::String> &doubles) {
      const auto groupId = juce::Uuid().toString();
      if (auto *owner = ownerOf(lead))
        owner->setDoubleGroup(groupId, InstanceRegistry::Lead);
      for (const auto &key : doubles)
        if (key != lead)
          if (auto *owner = ownerOf(key))
            owner->setDoubleGroup(groupId, InstanceRegistry::Double);
    };
    callbacks.addToGroup = [ownerOf](const juce::String &key, const juce::String &groupId) {
      if (auto *owner = ownerOf(key))
        owner->setDoubleGroup(groupId, InstanceRegistry::Double);
    };
    callbacks.removeFromGroup = [ownerOf](const juce::String &key) {
      // Taking out the lead ends the whole group.
      const auto id = InstanceRegistry::idFromKey(key);
      juce::String groupId;
      int role = InstanceRegistry::None;
      for (const auto &entry : InstanceRegistry::get().list())
        if (entry.id == id) {
          groupId = entry.groupId;
          role = entry.role;
        }
      if (groupId.isEmpty())
        return;
      if (role == InstanceRegistry::Lead) {
        for (const auto &entry : InstanceRegistry::get().list())
          if (entry.groupId == groupId)
            if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner))
              owner->setDoubleGroup({}, InstanceRegistry::None);
      } else if (auto *owner = ownerOf(key)) {
        owner->setDoubleGroup({}, InstanceRegistry::None);
      }
    };
    callbacks.setLead = [](const juce::String &key) {
      // The double becomes the lead; the old lead becomes a double and hands
      // over the group's unison switch and strength.
      const auto id = InstanceRegistry::idFromKey(key);
      const auto entries = InstanceRegistry::get().list();
      juce::String groupId;
      for (const auto &entry : entries)
        if (entry.id == id)
          groupId = entry.groupId;
      if (groupId.isEmpty())
        return;
      bool on = false, timingOn = false;
      float strength = 1.0f, timingStrength = 1.0f;
      for (const auto &entry : entries)
        if (entry.groupId == groupId && entry.role == InstanceRegistry::Lead && entry.id != id) {
          on = entry.unisonOn;
          strength = entry.unisonStrength;
          timingOn = entry.timingOn;
          timingStrength = entry.timingStrength;
          if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner))
            owner->setDoubleGroup(groupId, InstanceRegistry::Double);
        }
      if (auto *owner = static_cast<PitchNetAudioProcessor *>(InstanceRegistry::get().ownerOf(id))) {
        owner->setDoubleGroup(groupId, InstanceRegistry::Lead);
        owner->setUnison(on, strength);
        owner->setTiming(timingOn, timingStrength);
      }
    };
    callbacks.setUnison = [](const juce::String &groupId, bool on, float strength) {
      for (const auto &entry : InstanceRegistry::get().list())
        if (entry.groupId == groupId && entry.role == InstanceRegistry::Lead)
          if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner))
            owner->setUnison(on, strength);
    };
    callbacks.setTiming = [](const juce::String &groupId, bool on, float strength) {
      for (const auto &entry : InstanceRegistry::get().list())
        if (entry.groupId == groupId && entry.role == InstanceRegistry::Lead)
          if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner))
            owner->setTiming(on, strength);
    };
    callbacks.findDoubles = [this] { findDoubles(); };
    mainView->setTrackGroupCallbacks(std::move(callbacks));
  }
  mainView->setTrackPitchMacroTargets([this] { return buildTrackEditTargets(); });

  InstanceRegistry::get().addChangeListener(this);
  publishInstanceTracks();
  // Transfer state changes on the audio thread; the headers poll it.
  if (!isTimerRunning())
    startTimerHz(10);

  // Test hook for tools/PluginHostProbe (unset in a real host): a script of
  // header actions, e.g. "edit:1;ref:0;arm:2", run once after the window
  // opens, with the track list shown. Indexes are load order.
  static bool probeScriptDone = false;
  const auto script =
      juce::SystemStats::getEnvironmentVariable("PITCHNET_PROBE_ACTIONS", {});
  if (!probeScriptDone && script.isNotEmpty()) {
    probeScriptDone = true;
    const auto steps = juce::StringArray::fromTokens(script, ";", "");
    int delayMs = 800;
    for (int s = 0; s < steps.size(); ++s, delayMs += 500) {
      // "sleep:N" waits N ms before the next step.
      if (steps[s].startsWith("sleep:")) {
        delayMs += steps[s].fromFirstOccurrenceOf(":", false, false).getIntValue();
        continue;
      }
      juce::Timer::callAfterDelay(
          delayMs,
          [safe = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(
               static_cast<PitchNetAudioProcessorEditor *>(this)),
           step = steps[s]] {
            if (safe == nullptr || safe->mainView == nullptr)
              return;
            safe->mainView->showTrackList(true);
            PerfTrace::mark("probe action " + step);
            const auto action = step.upToFirstOccurrenceOf(":", false, false);
            if (safe->mainView->runEditCommand(step))
              return;
            const int index = step.fromFirstOccurrenceOf(":", false, false).getIntValue();
            const auto entries = InstanceRegistry::get().list();
            // "dumpnotes": every track's notes (frames, pitch, own pitch) as
            // JSON to PITCHNET_PROBE_DUMP, for checking doubles offline.
            if (action == "dumpnotes") {
              juce::Array<juce::var> tracksJson;
              for (const auto &entry : entries) {
                auto *obj = new juce::DynamicObject();
                obj->setProperty("id", static_cast<juce::int64>(entry.id));
                obj->setProperty("role", entry.role);
                juce::Array<juce::var> notesJson;
                if (entry.snapshot != nullptr)
                  for (const auto &note : entry.snapshot->getNotes())
                    if (!note.isRest())
                      notesJson.add(juce::Array<juce::var>{
                          note.getStartFrame(), note.getEndFrame(), note.getMidiNote(),
                          note.hasUnisonSource() ? note.getUnisonSourceMidi() : -1.0f,
                          note.getSibilantHead(), note.getSibilantTail(),
                          note.getSrcStartFrame(), note.getSrcEndFrame()});
                obj->setProperty("notes", notesJson);
                juce::Array<juce::var> curve;
                if (entry.snapshot != nullptr) {
                  const auto &audio = entry.snapshot->getAudioData();
                  for (size_t f = 0; f < audio.basePitch.size() && f < audio.deltaPitch.size(); ++f)
                    curve.add(audio.basePitch[f] + audio.deltaPitch[f]);
                }
                obj->setProperty("curve", curve);
                tracksJson.add(juce::var(obj));
              }
              const auto path = juce::SystemStats::getEnvironmentVariable(
                  "PITCHNET_PROBE_DUMP", {}) + "_" + step.fromFirstOccurrenceOf(":", false, false) + ".json";
              juce::File(path).replaceWithText(juce::JSON::toString(juce::var(tracksJson)));
              return;
            }
            // "timing:<0|1>:<percent>": the first group's Timing switch.
            if (action == "timing") {
              const auto parts = juce::StringArray::fromTokens(step, ":", "");
              for (const auto &entry : entries)
                if (entry.role == InstanceRegistry::Lead)
                  if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner)) {
                    owner->setTiming(parts[1].getIntValue() != 0, parts[2].getFloatValue() / 100.0f);
                    break;
                  }
              return;
            }
            // "unison:<0|1>:<percent>": the first group's unison switch.
            if (action == "unison") {
              const auto parts = juce::StringArray::fromTokens(step, ":", "");
              for (const auto &entry : entries)
                if (entry.role == InstanceRegistry::Lead)
                  if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner)) {
                    owner->setUnison(parts[1].getIntValue() != 0, parts[2].getFloatValue() / 100.0f);
                    break;
                  }
              return;
            }
            if (index < 0 || index >= static_cast<int>(entries.size()))
              return;
            // "makegroup:<i>": track i leads, every other track doubles it.
            if (action == "makegroup") {
              const auto groupId = juce::Uuid().toString();
              for (int t = 0; t < static_cast<int>(entries.size()); ++t)
                if (auto *owner = static_cast<PitchNetAudioProcessor *>(entries[static_cast<size_t>(t)].owner))
                  owner->setDoubleGroup(groupId, t == index ? InstanceRegistry::Lead
                                                            : InstanceRegistry::Double);
              return;
            }
            const auto key = InstanceRegistry::keyFor(entries[static_cast<size_t>(index)].id);
            if (action == "edit")
              safe->editTrack(key);
            else if (action == "ref")
              safe->setReferenceTrack(key, true);
            else if (action == "unref")
              safe->setReferenceTrack(key, false);
            else if (action == "arm")
              safe->setTrackTransfer(key, true);
            else if (action == "disarm")
              safe->setTrackTransfer(key, false);
            else if (action == "coedit")
              safe->toggleCoEdit(key);
            else if (action == "selectall" && safe->mainView->getProject() != nullptr)
              for (auto &target : safe->buildTrackEditTargets())
                for (auto &note : target.project->getNotes())
                  note.setSelected(!note.isRest());
          });
    }
  }
}

void PitchNetAudioProcessorEditor::setReferenceTrack(const juce::String &key,
                                                     bool reference) {
  // The own track as a ghost: no longer editable here, drawn grey.
  if (InstanceRegistry::idFromKey(key) == targetId) {
    if (reference) {
      pinnedInstanceKeys.insert(key);
      setOwnTrackEditable(false);
    } else {
      pinnedInstanceKeys.erase(key);
      lastInstanceTrackSignature.clear();
      lastInstancePreviewSignature.clear();
      publishInstanceTracks();
    }
    return;
  }
  // A track is either editable here or a reference, not both.
  if (reference && coEditTracks.count(InstanceRegistry::idFromKey(key)) > 0)
    closeCoEdit(InstanceRegistry::idFromKey(key));
  if (reference)
    pinnedInstanceKeys.insert(key);
  else
    pinnedInstanceKeys.erase(key);
  publishInstanceTracks();
}

void PitchNetAudioProcessorEditor::editTrack(const juce::String &key) {
  // Edit on another track (or a double-click on one of its grey notes)
  // edits that instance in this window. When it was a reference
  // track, the two swap roles - the track left becomes a reference.
  const auto id = InstanceRegistry::idFromKey(key);
  if (InstanceRegistry::get().ownerOf(id) == nullptr || id == targetId)
    return;
  const bool swapRoles = pinnedInstanceKeys.count(key) > 0;
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safe(this);
  // Async: this runs inside the track list's click handler, and re-targeting
  // rebuilds that list.
  juce::MessageManager::callAsync([safe, id, swapRoles] {
    if (safe == nullptr)
      return;
    auto *processor = static_cast<PitchNetAudioProcessor *>(
        InstanceRegistry::get().ownerOf(id));
    if (processor == nullptr)
      return;
    const auto previousKey = InstanceRegistry::keyFor(safe->targetId);
    safe->pinnedInstanceKeys.erase(InstanceRegistry::keyFor(id));
    if (swapRoles)
      safe->pinnedInstanceKeys.insert(previousKey);
    safe->retargetTo(*processor);
  });
}

void PitchNetAudioProcessorEditor::setTrackTransfer(const juce::String &key,
                                                    bool armed) {
  // Transfer on any track, from this one window.
  auto *processor = static_cast<PitchNetAudioProcessor *>(
      InstanceRegistry::get().ownerOf(InstanceRegistry::idFromKey(key)));
  if (processor == nullptr)
    return;
  if (armed)
    processor->startCapture();
  else
    processor->stopCapture();
  if (processor == targetProcessor)
    mainView->updateRecordArmState(processor->isCaptureArmed());
  lastInstanceTrackSignature.clear();
  publishInstanceTracks();
}

namespace {
// Working copies of co-edited tracks are never freed while the plug-in is
// loaded: undo steps recorded against them hold pointers to their notes.
// Closed copies are flagged dead, so those steps no longer render anything.
std::vector<std::shared_ptr<Project>> &retiredCoEditProjects() {
  static std::vector<std::shared_ptr<Project>> retired;
  return retired;
}

// Keeps a closed copy for its undo steps, but lets go of its audio (tens of
// megabytes each) once nothing can draw it any more. Without this every
// refresh of a co-edited double (one per lead edit in unison) kept a whole
// copy of the track's audio for as long as the plug-in was loaded.
void retireCoEditProject(const std::shared_ptr<Project> &project) {
  if (project == nullptr)
    return;
  retiredCoEditProjects().push_back(project);
  std::weak_ptr<Project> weak = project;
  juce::Timer::callAfterDelay(3000, [weak] {
    if (auto retired = weak.lock()) {
      auto &audio = retired->getAudioData();
      audio.waveform.setSize(0, 0);
      audio.originalWaveform.setSize(0, 0);
      audio.melSpectrogram.clear();
      audio.melSpectrogram.shrink_to_fit();
      audio.segmentDebugChunks.clear();
    }
  });
}
} // namespace

void PitchNetAudioProcessorEditor::setOwnTrackEditable(bool editable) {
  ownTrackEditable = editable;
  if (editable)
    pinnedInstanceKeys.erase(InstanceRegistry::keyFor(targetId)); // edited, not a ghost
  if (mainView != nullptr)
    mainView->setOwnTrackEditable(editable);
  lastInstanceTrackSignature.clear();
  lastInstancePreviewSignature.clear();
  publishInstanceTracks();
}

void PitchNetAudioProcessorEditor::toggleCoEdit(const juce::String &key) {
  const auto id = InstanceRegistry::idFromKey(key);
  // This window's own track: Edit switches it on and off like any other.
  if (id == targetId) {
    setOwnTrackEditable(!ownTrackEditable);
    return;
  }
  if (coEditTracks.count(id) > 0) {
    closeCoEdit(id);
  } else {
    auto *owner = static_cast<PitchNetAudioProcessor *>(
        InstanceRegistry::get().ownerOf(id));
    if (owner == nullptr || owner == targetProcessor)
      return;
    CoEditTrack track;
    if (auto *view = owner->getMainComponent()) {
      // Its window is open too: edit that window's project itself, so both
      // windows always show (and play) the same notes.
      auto *live = view->getProject();
      if (live == nullptr || live->getNotes().empty()) {
        mainView->setStatusMessage("Transfer (record) that track first.");
        return;
      }
      track.project = std::shared_ptr<Project>(live, [](Project *) {});
      track.liveView = view;
    } else {
      const auto *full = owner->getHeadlessProject();
      if (full == nullptr || full->getNotes().empty()) {
        mainView->setStatusMessage("Transfer (record) that track first.");
        return;
      }
      track.project = std::make_shared<Project>(*full);
      for (auto &note : track.project->getNotes())
        note.setSelected(false);
    }
    track.alive = std::make_shared<bool>(true);
    for (const auto &entry : InstanceRegistry::get().list())
      if (entry.id == id)
        track.seenRevision = entry.revision;
    coEditTracks[id] = std::move(track);
    pinnedInstanceKeys.erase(key); // edited, not a reference
  }
  mainView->setCoEditActive(!coEditTracks.empty());
  lastInstanceTrackSignature.clear();
  lastInstancePreviewSignature.clear();
  publishInstanceTracks();
}

void PitchNetAudioProcessorEditor::findDoubles() {
  // Every track with notes, by its own pitch (not the one unison gave it).
  const auto entries = InstanceRegistry::get().list();
  std::vector<std::vector<DoubleTuning::TNote>> notes;
  std::vector<size_t> entryOf;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &snapshot = entries[i].snapshot;
    if (snapshot == nullptr)
      continue;
    std::vector<DoubleTuning::TNote> trackNotes;
    for (const auto &note : snapshot->getNotes())
      if (!note.isRest() && note.getEndFrame() > note.getStartFrame())
        trackNotes.push_back({note.getStartFrame(), note.getEndFrame(),
                              note.hasUnisonSource() ? note.getUnisonSourceMidi()
                                                     : note.getMidiNote()});
    if (trackNotes.empty())
      continue;
    notes.push_back(std::move(trackNotes));
    entryOf.push_back(i);
  }
  const auto groups = DoubleTuning::detectGroups(notes);
  if (groups.empty()) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Find doubles",
        "No doubles found. Record (Transfer) the tracks first, or make a group by hand: "
        "select the tracks, right-click the lead and choose \"Make lead\".");
    return;
  }
  const auto nameOf = [&](int track) {
    const auto index = entryOf[static_cast<size_t>(track)];
    return InstanceRegistry::displayName(entries[index], static_cast<int>(index));
  };
  juce::String message;
  for (const auto &group : groups) {
    message << "Lead: " << nameOf(group.lead) << "\nDoubles: ";
    for (size_t d = 0; d < group.doubles.size(); ++d)
      message << (d > 0 ? ", " : "") << nameOf(group.doubles[d]);
    message << "\n\n";
  }
  message << "Create these groups?";
  // Owners by registry id, looked up again when the answer comes.
  std::vector<std::pair<std::uint64_t, std::vector<std::uint64_t>>> plan;
  for (const auto &group : groups) {
    std::vector<std::uint64_t> doubles;
    for (int d : group.doubles)
      doubles.push_back(entries[entryOf[static_cast<size_t>(d)]].id);
    plan.push_back({entries[entryOf[static_cast<size_t>(group.lead)]].id, doubles});
  }
  juce::AlertWindow::showOkCancelBox(
      juce::AlertWindow::QuestionIcon, "Find doubles", message, "Create", "Cancel", nullptr,
      juce::ModalCallbackFunction::create([plan](int result) {
        if (result != 1)
          return;
        const auto ownerOf = [](std::uint64_t id) {
          return static_cast<PitchNetAudioProcessor *>(InstanceRegistry::get().ownerOf(id));
        };
        for (const auto &[lead, doubles] : plan) {
          const auto groupId = juce::Uuid().toString();
          if (auto *owner = ownerOf(lead))
            owner->setDoubleGroup(groupId, InstanceRegistry::Lead);
          for (auto id : doubles)
            if (auto *owner = ownerOf(id))
              owner->setDoubleGroup(groupId, InstanceRegistry::Double);
        }
      }));
}

void PitchNetAudioProcessorEditor::closeCoEdit(std::uint64_t id) {
  auto it = coEditTracks.find(id);
  if (it == coEditTracks.end())
    return;
  *it->second.alive = false;
  retireCoEditProject(it->second.project);
  coEditTracks.erase(it);
  if (mainView != nullptr)
    mainView->setCoEditActive(!coEditTracks.empty());
  lastInstanceTrackSignature.clear();
  lastInstancePreviewSignature.clear();
}

void PitchNetAudioProcessorEditor::syncCoEditTracks(
    const std::vector<InstanceRegistry::Entry> &entries) {
  std::vector<std::uint64_t> toClose, toReopen;
  // Fresh copies are whole projects: one per pass (the timer runs ten times
  // a second), so several doubles changing at once do not freeze the window.
  bool copied = false;
  for (auto &[id, track] : coEditTracks) {
    auto entry = std::find_if(entries.begin(), entries.end(),
                              [id = id](const auto &e) { return e.id == id; });
    auto *owner = entry != entries.end()
                      ? static_cast<PitchNetAudioProcessor *>(entry->owner)
                      : nullptr;
    // Gone, or now this window's own track.
    if (owner == nullptr || owner == targetProcessor) {
      toClose.push_back(id);
      continue;
    }
    // Its window opened or closed, or that window now holds another
    // project: open it again the right way (live or as a copy).
    auto *view = owner->getMainComponent();
    if (view != track.liveView ||
        (view != nullptr && view->getProject() != track.project.get())) {
      toReopen.push_back(id);
      continue;
    }
    if (entry->revision == track.seenRevision || copied)
      continue;
    if (track.liveView != nullptr) {
      // Live: the notes are already the window's own; just redraw.
      track.seenRevision = entry->revision;
      track.renderPending = false;
      lastInstancePreviewSignature.clear();
      continue;
    }
    track.seenRevision = entry->revision;
    const auto *full = owner->getHeadlessProject();
    if (full == nullptr)
      continue;
    // A double in unison or Timing: the lead decides its notes, so always show (and
    // edit) what it really plays, never a copy from before unison moved it.
    const bool followsLead =
        std::any_of(full->getNotes().begin(), full->getNotes().end(),
                    [](const Note &note) { return note.hasUnisonSource() || note.hasAlignSource(); }) ||
        std::any_of(track.project->getNotes().begin(), track.project->getNotes().end(),
                    [](const Note &note) { return note.hasUnisonSource() || note.hasAlignSource(); });
    if (track.renderPending && !followsLead) {
      // Our own edit came back rendered: take its audio, keep our notes
      // (undo steps point at them).
      track.project->getAudioData().waveform = full->getAudioData().waveform;
      track.renderPending = false;
    } else {
      // Changed elsewhere (a new transfer, say): start a fresh copy.
      PerfTrace::Scope scope("coedit copy #" + juce::String(static_cast<juce::int64>(id)));
      *track.alive = false;
      retireCoEditProject(track.project);
      track.project = std::make_shared<Project>(*full);
      for (auto &note : track.project->getNotes())
        note.setSelected(false);
      track.alive = std::make_shared<bool>(true);
      copied = true;
    }
    lastInstancePreviewSignature.clear();
  }
  for (auto id : toClose)
    closeCoEdit(id);
  for (auto id : toReopen) {
    closeCoEdit(id);
    toggleCoEdit(InstanceRegistry::keyFor(id));
  }
}

void PitchNetAudioProcessorEditor::dropLiveCoEditsOn(const IMainView *view) {
  std::vector<std::uint64_t> ids;
  for (const auto &[id, track] : coEditTracks)
    if (view != nullptr && track.liveView == view)
      ids.push_back(id);
  if (ids.empty())
    return;
  for (auto id : ids) {
    closeCoEdit(id);
    // The next timer tick opens it again: as a copy once that window has
    // closed, or onto the project it holds next.
    pendingCoEditReopen.push_back(id);
  }
  if (mainView != nullptr)
    publishInstanceTracks();
}

void PitchNetAudioProcessorEditor::releaseLiveView(const IMainView *view) {
  for (auto *editor : openEditors())
    if (editor != nullptr && editor->mainView.get() != view)
      editor->dropLiveCoEditsOn(view);
}

std::vector<TrackPitchMacroTarget>
PitchNetAudioProcessorEditor::buildTrackEditTargets() {
  std::vector<TrackPitchMacroTarget> targets;
  auto *own = mainView != nullptr ? mainView->getProject() : nullptr;
  if (own == nullptr)
    return targets;
  const auto wholeProject = [](const Project &project) {
    const int frames = static_cast<int>(project.getAudioData().f0.size());
    return std::vector<juce::Range<int>>{{0, std::max(1, frames)}};
  };
  // The own track only while it is switched on.
  if (ownTrackEditable) {
    TrackPitchMacroTarget ownTarget;
    ownTarget.project = own;
    ownTarget.frames = wholeProject(*own);
    targets.push_back(ownTarget);
  }

  auto safe = juce::Component::SafePointer<PitchNetAudioProcessorEditor>(this);
  for (auto &[id, track] : coEditTracks) {
    TrackPitchMacroTarget target;
    target.project = track.project.get();
    target.frames = wholeProject(*track.project);
    target.isValid = [alive = track.alive] { return *alive; };
    target.onChanged = [safe, id = id, alive = track.alive,
                        project = track.project](bool render) {
      if (!*alive || safe == nullptr)
        return;
      auto *editor = safe.getComponent();
      auto live = editor->coEditTracks.find(id);
      if (live != editor->coEditTracks.end() && live->second.liveView != nullptr) {
        // Borrowed from that track's open window: it redraws and renders.
        live->second.liveView->notifyProjectEditedExternally(render);
      } else if (render) {
        auto *owner = static_cast<PitchNetAudioProcessor *>(
            InstanceRegistry::get().ownerOf(id));
        if (owner != nullptr && owner->getMainComponent() == nullptr) {
          auto it = editor->coEditTracks.find(id);
          if (it != editor->coEditTracks.end())
            it->second.renderPending = true;
          owner->requestPluginProjectRender(*project);
        }
      }
      editor->lastInstancePreviewSignature.clear();
      editor->publishInstanceTracks();
    };
    targets.push_back(std::move(target));
  }
  return targets;
}

std::shared_ptr<const juce::AudioBuffer<float>>
PitchNetAudioProcessorEditor::buildPreviewMixBed(double startSeconds,
                                                 double endSeconds) {
  // Editing Mix Fader: hard left, only the tracks being edited (this one and
  // the ones switched on with Edit); towards the centre the reference
  // tracks fade in; right of centre every other track too.
  const double mix = mainView != nullptr ? mainView->getEditingMix() : 0.0;
  const double referenceGain = mix <= 0.0 ? mix + 1.0 : 1.0;
  const double otherGain = mix <= 0.0 ? 0.0 : mix;
  const double hostRate =
      target().getHostSampleRate() > 0.0 ? target().getHostSampleRate() : 44100.0;
  const int length =
      static_cast<int>(std::ceil(std::max(0.0, endSeconds - startSeconds) * hostRate));
  if (length <= 0)
    return nullptr;

  auto bed = std::make_shared<juce::AudioBuffer<float>>(2, length);
  bed->clear();
  bool any = false;
  const auto addTrack = [&](const Project &project, double gain) {
    const auto &audio = project.getAudioData();
    const auto &waveform = audio.waveform;
    if (gain <= 0.001 || waveform.getNumSamples() == 0 || audio.sampleRate <= 0)
      return;
    const int first = static_cast<int>(std::floor(startSeconds * audio.sampleRate));
    const int count = static_cast<int>(std::ceil((endSeconds - startSeconds) * audio.sampleRate));
    juce::AudioBuffer<float> slice(waveform.getNumChannels(), std::max(1, count));
    slice.clear();
    for (int ch = 0; ch < waveform.getNumChannels(); ++ch) {
      const int from = std::max(0, first), to = std::min(waveform.getNumSamples(), first + count);
      if (to > from)
        slice.copyFrom(ch, from - first, waveform, ch, from, to - from);
    }
    const auto atHostRate =
        AudioResampler::resample(slice, audio.sampleRate, hostRate, length);
    for (int ch = 0; ch < 2; ++ch)
      bed->addFrom(ch, 0, atHostRate, std::min(ch, atHostRate.getNumChannels() - 1), 0,
                   std::min(length, atHostRate.getNumSamples()),
                   static_cast<float>(gain));
    any = true;
  };

  for (const auto &entry : InstanceRegistry::get().list()) {
    if (entry.id == targetId)
      continue;
    if (auto it = coEditTracks.find(entry.id); it != coEditTracks.end()) {
      addTrack(*it->second.project, 1.0);
      continue;
    }
    if (entry.snapshot == nullptr)
      continue;
    const bool reference =
        pinnedInstanceKeys.count(InstanceRegistry::keyFor(entry.id)) > 0;
    addTrack(*entry.snapshot, reference ? referenceGain : otherGain);
  }
  return any ? bed : nullptr;
}

bool PitchNetAudioProcessorEditor::targetIsAlive() const {
  return targetProcessor != nullptr &&
         InstanceRegistry::get().ownerOf(targetId) == targetProcessor;
}

void PitchNetAudioProcessorEditor::detachFromTarget() {
  // Its project is about to change hands: windows borrowing it let go.
  releaseLiveView(mainView.get());
  if (!targetIsAlive() || target().getMainComponent() != mainView.get())
    return;
  // Exactly what closing this window does: the instance keeps playing its
  // edits headlessly from the snapshot it takes here.
  target().stopPluginPreview();
  target().stopPluginAudition();
  target().getTransportController().clearCallbacks();
  target().setMainComponent(nullptr);
}

void PitchNetAudioProcessorEditor::attachToTarget(
    PitchNetAudioProcessor &processor) {
  // Exactly what opening that instance's window does: it restores its
  // project into this canvas and binds its undo history and renderer.
  // Tracks opened alongside the previous one are closed: the window now
  // edits another instance, which may itself have been one of them.
  while (!coEditTracks.empty())
    closeCoEdit(coEditTracks.begin()->first);
  releaseLiveView(mainView.get());
  ownTrackEditable = true;
  if (mainView != nullptr)
    mainView->setOwnTrackEditable(true);
  targetProcessor = &processor;
  targetId = processor.getInstanceRegistryId();
  // Start from an empty canvas: an instance that has recorded nothing yet
  // restores nothing, and must not inherit the previous track's notes. The
  // previous instance already took its own copy when it was detached.
  auto displaced = mainView->exchangeProject(nullptr);
  juce::ignoreUnused(displaced);
  processor.setMainComponent(mainView.get());
  setupNonARAMode();
  setupCallbacks();
  lastInstanceTrackSignature.clear();
  lastInstancePreviewSignature.clear();
  publishInstanceTracks();
  mainView->getComponent()->repaint();
}

void PitchNetAudioProcessorEditor::retargetTo(PitchNetAudioProcessor &next) {
  if (!instanceTracksEnabled || &next == targetProcessor)
    return;
  auto *previous = targetIsAlive() ? targetProcessor : nullptr;

  // Another window may be editing `next`: the two windows swap tracks, so a
  // track is never edited in two places at once.
  PitchNetAudioProcessorEditor *other = nullptr;
  for (auto *editor : openEditors())
    if (editor != this && editor->targetIsAlive() &&
        editor->targetProcessor == &next &&
        next.getMainComponent() == editor->mainView.get())
      other = editor;

  detachFromTarget();
  if (other != nullptr) {
    other->detachFromTarget();
    if (previous != nullptr)
      other->attachToTarget(*previous);
    else
      other->attachToTarget(other->audioProcessor);
  }
  attachToTarget(next);
}

void pitchNetEditorsForgetProcessor(PitchNetAudioProcessor *processor) {
  // A processor is going away; any window editing it returns to its own
  // instance now, while the processor can still be detached cleanly.
  for (auto *editor : openEditors())
    if (editor->targetProcessor == processor &&
        &editor->audioProcessor != processor) {
      editor->detachFromTarget();
      editor->attachToTarget(editor->audioProcessor);
    }
}

void PitchNetAudioProcessorEditor::changeListenerCallback(
    juce::ChangeBroadcaster *) {
  publishInstanceTracks();
}

void PitchNetAudioProcessorEditor::publishInstanceTracks() {
  if (!instanceTracksEnabled || mainView == nullptr)
    return;

  // The instance this window edits went away without telling us (should
  // not happen: its destructor does). Fall back to our own.
  if (!targetIsAlive() && targetProcessor != &audioProcessor) {
    attachToTarget(audioProcessor);
    return;
  }

  // The active track is the one this window edits.
  const auto entries = InstanceRegistry::get().list();
  const auto ownKey = InstanceRegistry::keyFor(targetId);
  syncCoEditTracks(entries);

  // A host that sends no track colour gets a distinct one per load order.
  const auto colourFor = [](const InstanceRegistry::Entry &entry, int index) {
    return entry.hasColour
               ? entry.colour
               : juce::Colour::fromHSV(std::fmod(0.58f + 0.17f * index, 1.0f),
                                       0.55f, 0.95f, 1.0f);
  };

  // Forget pins of instances that have gone.
  for (auto it = pinnedInstanceKeys.begin(); it != pinnedInstanceKeys.end();) {
    const bool exists = std::any_of(
        entries.begin(), entries.end(), [&](const InstanceRegistry::Entry &e) {
          return InstanceRegistry::keyFor(e.id) == *it;
        });
    it = !exists ? pinnedInstanceKeys.erase(it) : std::next(it);
  }

  std::vector<MainViewTrackEntry> tracks;
  juce::String trackSignature = ownKey;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    const auto key = InstanceRegistry::keyFor(entry.id);
    MainViewTrackEntry track;
    track.key = key;
    track.name = InstanceRegistry::displayName(entry, static_cast<int>(i));
    track.colour = colourFor(entry, static_cast<int>(i));
    track.hasHostColour = true; // a colour is always set, see colourFor
    track.pinned = pinnedInstanceKeys.count(key) > 0;
    track.ownWindow = entry.id == audioProcessor.getInstanceRegistryId();
    track.coEdited = entry.id == targetId ? ownTrackEditable
                                          : coEditTracks.count(entry.id) > 0;
    track.transferAvailable = true;
    if (auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner)) {
      track.transferArmed = owner->isCaptureArmed();
      track.transferActive = owner->isCapturing();
    }
    track.groupId = entry.groupId;
    track.role = entry.role;
    track.unisonOn = entry.unisonOn;
    track.unisonStrength = entry.unisonStrength;
    track.timingOn = entry.timingOn;
    track.timingStrength = entry.timingStrength;
    tracks.push_back(track);
    trackSignature << "\n" << key << "\t" << track.name << "\t"
                   << track.colour.toString() << (track.pinned ? "\tP" : "")
                   << (track.ownWindow ? "\t*" : "")
                   << (track.transferArmed ? "\tT" : "")
                   << (track.transferActive ? "\tR" : "")
                   << (track.coEdited ? "\tE" : "")
                   << "\tG" << track.groupId << ":" << track.role << ":"
                   << (track.unisonOn ? 1 : 0) << ":" << juce::String(track.unisonStrength, 3)
                   << ":" << (track.timingOn ? 1 : 0) << ":" << juce::String(track.timingStrength, 3);
  }
  if (trackSignature != lastInstanceTrackSignature) {
    lastInstanceTrackSignature = trackSignature;
    mainView->updateTrackList(tracks, ownKey);
  }

#if JucePlugin_Enable_ARA
  // Ghost tracks: the pinned instances' notes, drawn on this canvas from the
  // snapshot each one publishes after analysis and every render. A capture's
  // project starts at host time zero (its waveform carries the capture
  // offset), so project time is host time.
  // Cheap signature first: this runs on the header refresh timer too, and
  // building previews copies note windows.
  juce::String previewSignature;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    const auto key = InstanceRegistry::keyFor(entry.id);
    if (pinnedInstanceKeys.count(key) == 0 || entry.snapshot == nullptr)
      continue;
    previewSignature << key << ":" << juce::String(static_cast<juce::int64>(entry.revision))
                     << ":" << colourFor(entry, static_cast<int>(i)).toString() << ";";
  }
  auto *ownProject = mainView->getProject();
  previewSignature << (ownTrackEditable ? "own-on;" : "own-off;");
  if (!coEditTracks.empty() && ownProject != nullptr) {
    previewSignature << "own:" << juce::String::toHexString(static_cast<juce::int64>(
                            reinterpret_cast<std::uintptr_t>(ownProject)))
                     << ":" << juce::String(static_cast<double>(
                                   ownProject->getAudioData().getDuration()), 3)
                     << ";";
    for (const auto &[id, track] : coEditTracks)
      previewSignature << "edit:" << juce::String(static_cast<juce::int64>(id)) << ":"
                       << juce::String::toHexString(static_cast<juce::int64>(
                              reinterpret_cast<std::uintptr_t>(track.project.get())))
                       << ";";
  }
  if (previewSignature == lastInstancePreviewSignature)
    return;

  auto previews = std::make_shared<std::vector<MainViewRegionPreview>>();
  std::map<std::uint64_t, std::shared_ptr<const Project>> keep;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto &entry = entries[i];
    const auto key = InstanceRegistry::keyFor(entry.id);
    if (pinnedInstanceKeys.count(key) == 0 || entry.snapshot == nullptr)
      continue;
    const auto &audio = entry.snapshot->getAudioData();
    const double start = std::max(0.0, audio.timelineOffsetSeconds);
    const double end = static_cast<double>(audio.getDuration());
    if (end <= start || entry.snapshot->getNotes().empty())
      continue;

    auto preview = buildRegionPreview(*entry.snapshot, start, end, start);
    preview.key = key;
    preview.name = InstanceRegistry::displayName(entry, static_cast<int>(i));
    preview.startSeconds = start;
    preview.endSeconds = end;
    preview.active = false;
    preview.pinned = true;
    preview.outlineColour = colourFor(entry, static_cast<int>(i));
    if (preview.project)
      for (auto &note : preview.project->getNotes())
        note.setSelected(false);
    previews->push_back(std::move(preview));
    keep[entry.id] = entry.snapshot;
  }
  // Tracks open for editing alongside this one (Ctrl+Edit), drawn and
  // edited through the same multi-track path ARA's Track mode uses: the
  // canvas's own track as the active window, the others as editable
  // windows onto their working copies.
  if (!coEditTracks.empty() && ownProject != nullptr) {
    const auto &ownAudio = ownProject->getAudioData();
    MainViewRegionPreview active;
    active.key = ownKey;
    active.active = true;
    active.hasContent = true;
    active.startSeconds = std::max(0.0, ownAudio.timelineOffsetSeconds);
    active.endSeconds = std::max(active.startSeconds,
                                 static_cast<double>(ownAudio.getDuration()));
    active.sourceProject = ownProject;
    active.sourceFirstFrame = 0;
    active.projectHostOffsetSeconds = 0.0;
    previews->push_back(std::move(active));

    for (const auto &[id, track] : coEditTracks) {
      const auto &audio = track.project->getAudioData();
      const double start = std::max(0.0, audio.timelineOffsetSeconds);
      const double end = static_cast<double>(audio.getDuration());
      if (end <= start)
        continue;
      auto preview = buildRegionPreview(*track.project, start, end, start);
      preview.key = InstanceRegistry::keyFor(id);
      preview.startSeconds = start;
      preview.endSeconds = end;
      preview.active = false;
      preview.pinned = false;
      for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id) {
          preview.name = InstanceRegistry::displayName(entries[i], static_cast<int>(i));
          preview.outlineColour = colourFor(entries[i], static_cast<int>(i));
        }
      if (preview.project) {
        // Mirror the working copy's selection onto the drawn window.
        const auto &source = track.project->getNotes();
        for (auto &copy : preview.project->getNotes())
          for (const auto &note : source)
            if (note.getStartFrame() == copy.getStartFrame() + preview.sourceFirstFrame &&
                note.getEndFrame() == copy.getEndFrame() + preview.sourceFirstFrame) {
              copy.setSelected(note.isSelected());
              break;
            }
      }
      previews->push_back(std::move(preview));
    }
  }

  lastInstancePreviewSignature = previewSignature;
  ghostSnapshots = std::move(keep);
  mainView->updateRegionPreviews(previews->empty() ? nullptr
                                                   : MainViewRegionPreviewList(previews));
#endif
}

void PitchNetAudioProcessorEditor::setupHostTransportUiSync(
    bool includePlayState) {
  auto safeMain =
      juce::Component::SafePointer<juce::Component>(mainView->getComponent());

  if (includePlayState) {
    target().getTransportController().setPlayStateCallback(
        [safeMain](bool isPlaying) {
          auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
          if (!view)
            return;

          view->updateHostPlaybackState(isPlaying);
        });
  } else {
    target().getTransportController().setPlayStateCallback(nullptr);
  }

  target().getTransportController().setPositionCallback(
      [safeMain](double timeInSeconds) {
        auto *view = dynamic_cast<IMainView *>(safeMain.getComponent());
        if (!view)
          return;

        view->updatePlaybackPosition(timeInSeconds);
      });

  target().getTransportController().setLoopCallback(
      [safeMain](const HostSyncService::LoopInfo &loop) {
        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostLoopRange(loop.loopStartSeconds, loop.loopEndSeconds,
                                    loop.isLoopEnabled, loop.hasLoopPoints);
      });

  target().getTransportController().setTempoCallback(
      [safeMain](const HostSyncService::TempoInfo &tempo) {
        if (!tempo.hasBpm && !tempo.hasTimeSignature)
          return;

        if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
          view->updateHostTimelineState(tempo.bpm, tempo.timeSigNumerator,
                                        tempo.timeSigDenominator);
      });

  const auto hostState =
      target().getTransportController().getCurrentState();
  if (hostState.tempo.hasBpm || hostState.tempo.hasTimeSignature)
    if (auto *view = dynamic_cast<IMainView *>(safeMain.getComponent()))
      view->updateHostTimelineState(hostState.tempo.bpm,
                                    hostState.tempo.timeSigNumerator,
                                    hostState.tempo.timeSigDenominator);

  syncHostLoopSnapshotFromPlayHead();
}

void PitchNetAudioProcessorEditor::syncHostLoopSnapshotFromPlayHead() {
  auto *playHead = target().getPlayHead();
  if (!playHead)
    return;

  auto posInfo = playHead->getPosition();
  if (!posInfo.hasValue())
    return;

  double startSeconds = 0.0;
  double endSeconds = 0.0;
  bool hasRange = false;

  if (auto loopPoints = posInfo->getLoopPoints()) {
    if (auto bpm = posInfo->getBpm()) {
      if (*bpm > 0.0) {
        startSeconds = loopPoints->ppqStart * 60.0 / *bpm;
        endSeconds = loopPoints->ppqEnd * 60.0 / *bpm;
        hasRange = endSeconds > startSeconds;
      }
    }
  }

  if (auto *view = mainView.get())
    view->updateHostLoopRange(startSeconds, endSeconds,
                              posInfo->getIsLooping(), hasRange);
}

void PitchNetAudioProcessorEditor::setupCallbacks() {
  // When project data changes (analysis complete or synthesis complete)
  mainView->setOnProjectDataChanged([this]() {
    if (auto *project = mainView->getProject())
      target().updateProjectStateFromEditor(*project);
    mainView->bindRealtimeProcessor(target().getRealtimeProcessor());
    target().getRealtimeProcessor().invalidate();
  });

  // onPitchEditFinished is handled by onProjectDataChanged (called after async
  // synthesis completes) No need for separate callback here
}

void PitchNetAudioProcessorEditor::paint(juce::Graphics &) {
  // MainComponent handles all painting
}

void PitchNetAudioProcessorEditor::resized() {
  audioProcessor.setLastEditorSize(getWidth(), getHeight());
  mainView->getComponent()->setBounds(getLocalBounds());
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::parentHierarchyChanged() {
  AudioProcessorEditor::parentHierarchyChanged();
  applyLunaSoftwareRenderer();
}

void PitchNetAudioProcessorEditor::visibilityChanged() {
  if (isVisible()) {
    applyLunaSoftwareRenderer();
    requestMainViewKeyboardFocusAsync();
  }
}

void PitchNetAudioProcessorEditor::mouseDown(const juce::MouseEvent &e) {
  juce::ignoreUnused(e);
  requestMainViewKeyboardFocusAsync();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocus() {
  auto *component = mainView ? mainView->getComponent() : nullptr;
  if (!component || (!component->isShowing() && !component->isOnDesktop()))
    return;

  if (auto *focused = juce::Component::getCurrentlyFocusedComponent()) {
    if (focused == this || isParentOf(focused) || focused == component ||
        component->isParentOf(focused))
      return;
  }

  if (auto *peer = getPeer())
    peer->grabFocus();

  grabKeyboardFocus();
  component->grabKeyboardFocus();
}

void PitchNetAudioProcessorEditor::requestMainViewKeyboardFocusAsync() {
  juce::Component::SafePointer<PitchNetAudioProcessorEditor> safeThis(this);
  juce::MessageManager::callAsync([safeThis]() {
    if (safeThis != nullptr) {
      safeThis->applyLunaSoftwareRenderer();
      safeThis->requestMainViewKeyboardFocus();
    }
  });
}

void PitchNetAudioProcessorEditor::applyLunaSoftwareRenderer() {
#if JUCE_WINDOWS
  if (lunaSoftwareRendererApplied || !isLunaHostProcess())
    return;

  auto *peer = getPeer();
  if (!peer)
    return;

  const auto engines = peer->getAvailableRenderingEngines();
  const int softwareRenderer = engines.indexOf("Software Renderer");
  if (softwareRenderer < 0) {
    lunaSoftwareRendererApplied = true;
    LOG("LUNA host detected, but JUCE software renderer is unavailable");
    return;
  }

  lunaSoftwareRendererApplied = true;
  if (peer->getCurrentRenderingEngine() != softwareRenderer)
    peer->setCurrentRenderingEngine(softwareRenderer);

  LOG("LUNA host detected: forcing JUCE software renderer");
#endif
}
