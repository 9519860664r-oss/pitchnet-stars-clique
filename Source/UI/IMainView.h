#pragma once

#include "../JuceHeader.h"
#include "RegionPreview.h"
#include "../Audio/IO/MidiExporter.h"
#include <functional>
#include <memory>
#include <vector>

class Project;
class Vocoder;
class RealtimePitchProcessor;
class EditorController;
class PitchUndoManager;

struct MainViewViewportState
{
  double scrollX = 0.0;
  double scrollY = 0.0;
  float pixelsPerSecond = 100.0f;
  float pixelsPerSemitone = 12.0f;
  bool valid = false;
};

/**
 * One selectable region, as the side panel sees it.
 *
 * The UI must not depend on the ARA SDK, so regions reach it as a stable key
 * (the plug-in's persistent region key) plus the name to display. The key is
 * what comes back when the user picks an entry.
 */
struct MainViewRegionEntry {
  juce::String key;
  juce::String name;
};

/**
 * One track (ARA region sequence) that has PitchNet on it, as the left side
 * panel sees it. The key is session-local and only used to mark the active
 * track; the colour is the DAW's track colour sent through ARA, when it sent
 * one. The panel decides whether to use it or its neutral default.
 */
struct MainViewTrackEntry {
  juce::String key;
  juce::String name;
  juce::Colour colour;        // the DAW's colour; only valid if hasHostColour
  bool hasHostColour = false; // false when the DAW sent no colour via ARA
  bool pinned = false;        // Reference: notes drawn grey on the canvas
  // Header extras (hosts without ARA):
  bool ownWindow = false;         // the star: this window's own instance
  bool transferAvailable = false; // show the Transfer (record) button
  bool transferArmed = false;     // Transfer on: waiting for or recording audio
  bool transferActive = false;    // audio is being recorded right now
  bool coEdited = false;          // also open for editing (Ctrl+Edit)
  // Lead and doubles: tracks with the same group id; role 1 lead, 2 double.
  // The lead's entry carries the group's unison switch and strength.
  juce::String groupId;
  int role = 0;
  bool unisonOn = false;
  float unisonStrength = 1.0f;
  bool timingOn = false;
  float timingStrength = 1.0f;
};

/** Track list actions for lead / doubles groups. */
struct TrackGroupCallbacks {
  std::function<void(const juce::String &lead, const std::vector<juce::String> &doubles)> makeGroup;
  std::function<void(const juce::String &key, const juce::String &groupId)> addToGroup;
  std::function<void(const juce::String &key)> removeFromGroup;
  std::function<void(const juce::String &key)> setLead; // a double becomes its group's lead
  std::function<void(const juce::String &groupId, bool on, float strength)> setUnison;
  std::function<void(const juce::String &groupId, bool on, float strength)> setTiming;
  std::function<void()> findDoubles;
};

/** Track colour when the DAW sends none, or "Use DAW Track Color" is off. */
constexpr juce::uint32 kDefaultTrackColour = 0xFFE6E6E6u;

/** The colour a track is shown in, given the "Use DAW Track Color" switch. */
inline juce::Colour resolveTrackColour(const MainViewTrackEntry &track,
                                       bool useDawTrackColour) {
  return useDawTrackColour && track.hasHostColour
             ? track.colour
             : juce::Colour(kDefaultTrackColour);
}

class IMainView {
public:
  virtual ~IMainView() = default;

  virtual juce::Component *getComponent() = 0;
  virtual Project *getProject() const = 0;
  virtual std::unique_ptr<Project>
  exchangeProject(std::unique_ptr<Project> project) = 0;
  virtual Vocoder *getVocoder() const = 0;
  virtual void bindBackendController(EditorController *controller) = 0;
  virtual void bindUndoManager(PitchUndoManager *manager) = 0;
  virtual MainViewViewportState getViewportState() const = 0;
  virtual void restoreViewportState(const MainViewViewportState &state) = 0;
  virtual bool hasAnalyzedProject() const = 0;
  virtual void bindRealtimeProcessor(RealtimePitchProcessor &processor) = 0;
  virtual juce::String serializeProjectJson() const = 0;
  virtual bool restoreProjectJson(const juce::String &json) = 0;
  virtual bool restoreProjectSnapshot(const Project &project) = 0;
  virtual void setStatusMessage(const juce::String &message) = 0;
  virtual void showAnalysisProgress(double progress) = 0;
  virtual void hideAnalysisProgress() = 0;
  virtual void finishBackendRender(bool success) = 0;
  virtual void setTrackPitchMacroTargets(
      std::function<std::vector<TrackPitchMacroTarget>()>) {}
  virtual void setOnProjectDataChanged(std::function<void()> callback) = 0;
  virtual void setOnPitchEditFinished(std::function<void()> callback) = 0;
  virtual void setOnRequestBackendRender(
      std::function<void(const Project &)> callback) = 0;
  virtual void setOnRequestBackendPreview(
      std::function<void(const Project &, int, int)> callback) = 0;
  virtual void setOnStopBackendPreview(std::function<void()> callback) = 0;
  virtual void setOnRequestDragAudition(
      std::function<void(const juce::AudioBuffer<float> &, double)> callback) = 0;
  virtual void setOnStopDragAudition(std::function<void()> callback) = 0;
  virtual void setOnRequestHostPlayState(
      std::function<void(bool)> callback) = 0;
  virtual void setOnRequestHostStop(std::function<void()> callback) = 0;
  virtual void setOnRequestHostSeek(
      std::function<void(double)> callback) = 0;
  virtual void setOnRequestHostLoopRange(
      std::function<void(double, double, bool, bool)> callback) = 0;
  virtual void setOnRecordArmChanged(std::function<void(bool)> callback) = 0;
  virtual void setRecordControlVisible(bool visible) = 0;
  /**
   * Replace the tracks the left side panel lists and mark the active one (an
   * empty key means none of them).
   */
  virtual void updateTrackList(const std::vector<MainViewTrackEntry> &tracks,
                               const juce::String &activeKey) = 0;
  /** Called with a track key when the user clicks an inactive track. */
  virtual void setOnTrackSelected(
      std::function<void(const juce::String &)> callback) = 0;
  /** Called with a track key and the new state when a track's pin is toggled. */
  virtual void setOnTrackPinChanged(
      std::function<void(const juce::String &, bool)> callback) = 0;
  /** Called with a track key and the requested state when its Transfer
      button is clicked (hosts without ARA). */
  virtual void setOnTrackTransferToggled(
      std::function<void(const juce::String &, bool)>) {}
  /** Called with a track key when a reference (grey) note of that track is
      double-clicked on the canvas: the two tracks swap roles. */
  virtual void setOnReferenceNoteDoubleClicked(
      std::function<void(const juce::String &)>) {}
  /** Called with a track key on Ctrl/Cmd+click of its Edit button: open it
      for editing alongside the current track (or close it again). */
  virtual void setOnTrackEditAdded(std::function<void(const juce::String &)>) {}
  /** Other tracks are open for editing in this canvas (hosts without ARA). */
  virtual void setCoEditActive(bool) {}
  /** Another window edited this window's project (both windows edit the
      same tracks): redraw, and render when `render`. */
  virtual void notifyProjectEditedExternally(bool) {}
  /** The key, A4, custom scale or key changes (shared by every track)
      changed, maybe in another window: re-read and redraw. */
  virtual void refreshSharedKey() {}
  /** The window's own track switched on or off for editing (Edit). */
  virtual void setOwnTrackEditable(bool) {}
  /** Editing Mix Fader (hosts without ARA): show it, and hear about moves. */
  virtual void setEditingMixAvailable(bool) {}
  virtual void setOnEditingMixChanged(std::function<void(double)>) {}
  virtual double getEditingMix() const { return 0.0; }
  /** Replace Ranges on the time ruler (Transfer without ARA). */
  virtual void setReplaceRangesAvailable(bool) {}
  virtual void setOnReplaceRangesEdited(
      std::function<void(const std::vector<std::pair<double, double>> &)>) {}
  /** MIDI export: every track shown in the editor, each on its own channel.
      The provider returns them all, this one included; none or one track
      exports this track alone. */
  virtual void setMidiExportTracksProvider(std::function<std::vector<MidiExportTrack>()>) {}
  /** Edit commands by name, for test scripts: "copy", "cut", "paste",
      "deselect", "cursor:<seconds>". Returns false when unknown. */
  virtual bool runEditCommand(const juce::String &) { return false; }
  /** State of the "Use DAW Track Color" switch. */
  virtual bool getUseDawTrackColour() const = 0;
  /** Called after the "Use DAW Track Color" switch changes. */
  virtual void setOnTrackColourModeChanged(std::function<void()> callback) = 0;
  /**
   * Draw every region of the track on the canvas and thumbnail. Exactly one
   * entry may be flagged active; it keeps the editable backdrop, the others
   * are shown dimmed and read-only. An empty list shows only the canvas
   * project, as in standalone mode.
   */
  virtual void updateRegionPreviews(MainViewRegionPreviewList previews) = 0;
  /**
   * Called with a region key when the user clicks inside an inactive region on
   * the canvas (empty space or one of its notes). Unlike a host selection
   * change, the handler must not scroll or refit the view: the user clicked on what is
   * already visible.
   */
  virtual void setOnRegionActivationRequested(
      std::function<void(const juce::String &)> callback) = 0;
  /**
   * Clip / Track view (ARA only). Clip mode shows the selected region's
   * modification in its own time; Track mode shows every region of the track
   * on the host timeline (region previews are only drawn there). Making the
   * mode available shows its toggle button and restores the saved choice.
   */
  virtual void setTrackViewModeAvailable(bool available) = 0;
  /** Show the left track list without the Clip / Track toggle (hosts without
      ARA list the other PitchNet instances there). Pinned tracks' notes are
      then drawn in Clip mode too. */
  virtual void setTrackListAvailable(bool) {}
  virtual void setTrackGroupCallbacks(TrackGroupCallbacks) {}
  /** Open or close the left track list, as its toolbar button does. */
  virtual void showTrackList(bool) {}
  virtual bool isTrackViewMode() const = 0;
  /** Switch Clip / Track view as the toolbar toggle does. */
  virtual void requestTrackViewMode(bool) {}
  virtual void setOnTrackViewModeChanged(
      std::function<void(bool)> callback) = 0;
  /**
   * Tell the view whether the host transport can be driven from here.
   * False in non-ARA plugin mode: play / stop / cycle controls and cycle-range
   * editing are disabled, leaving capture as the only transport action.
   */
  virtual void setHostTransportControlAvailable(bool available) = 0;
  virtual void updateRecordArmState(bool armed) = 0;
  virtual void beginLiveRecording(double sampleRate,
                                  double timelineOffsetSeconds) = 0;
  virtual void appendLiveRecordingAudio(
      const juce::AudioBuffer<float> &buffer) = 0;

  virtual void setHostAudio(const juce::AudioBuffer<float> &buffer,
                            double sampleRate,
                            double timelineOffsetSeconds = 0.0) = 0;
  virtual void updateHostAudioTimelineOffset(double timelineOffsetSeconds) = 0;
  virtual void clearHostAudio() = 0;
  virtual void focusTimelineRange(double startSeconds, double endSeconds) = 0;
  virtual void updatePlaybackPosition(double timeSeconds) = 0;
  // Where the drawn project content sits on the host timeline, in seconds.
  // Display only; may be negative. See CoordinateMapper for the full contract.
  virtual void setTimelineDisplayOffset(double seconds) = 0;
  virtual void updateHostPlaybackState(bool isPlaying) = 0;
  virtual void updateHostTimelineState(double bpm, int numerator,
                                       int denominator) = 0;
  virtual void updateHostLoopRange(double startSeconds, double endSeconds,
                                   bool enabled, bool hasRange) = 0;
  virtual void notifyHostStopped() = 0;

  /**
   * Trigger re-synthesis from external parameter changes (e.g. DAW automation).
   * Called on the message thread when plugin parameters like pitch offset or
   * formant shift are changed via host automation.
   */
  virtual void triggerResynthesis() = 0;
};
