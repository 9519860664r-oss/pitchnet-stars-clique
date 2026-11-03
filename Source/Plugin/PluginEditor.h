#pragma once

#include "../Utils/UI/Skin.h"

#include <set>

#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../UI/MainViewFactory.h"
#include <cstddef>
#include <map>
#include <memory>
#include <vector>
#include "PluginProcessor.h"
#include "InstanceRegistry.h"

/** Called by a processor being destroyed: every window editing it goes back
    to its own instance before the processor's members are gone. */
void pitchNetEditorsForgetProcessor(PitchNetAudioProcessor* processor);

class PitchNetAudioProcessorEditor : public juce::AudioProcessorEditor
    , private juce::Timer
    , private juce::ChangeListener
#if JucePlugin_Enable_ARA
    , public juce::AudioProcessorEditorARAExtension
    , private juce::ARAEditorView::Listener
#endif
{
public:
    explicit PitchNetAudioProcessorEditor(PitchNetAudioProcessor&);
    ~PitchNetAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void parentHierarchyChanged() override;

#if JucePlugin_Enable_ARA
    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override;
#endif

    // Grab keyboard focus when the editor becomes visible / is clicked, so that
    // MainComponent holds focus and host-forwarded key events are dispatched to
    // its command-key mappings.
    void visibilityChanged() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void setupARAMode();
    void setupNonARAMode();
    void setupCallbacks();
    void setupHostTransportUiSync(bool includePlayState);
    void syncHostLoopSnapshotFromPlayHead();
    void syncAAXARAPlayheadStateFromHost();

#if JucePlugin_Enable_ARA
    bool syncInitialARASelectionFromHost();
    // Regions this plugin instance can edit, newest host state each call:
    // the renderer's assignment when the host made one, otherwise whatever the
    // document controller discovered.
    std::vector<juce::ARAPlaybackRegion*> collectAraPlaybackRegions() const;
    // Diagnostics for the "No regions" case: how many regions each lookup sees.
    size_t documentControllerRegionCount() const;
    size_t documentRegionCount() const;
    // Rebuild the Regions card's region list and push it to the UI, but only
    // when it actually differs from what the card already shows.
    void refreshAraRegionList();
    // Switch the canvas to a region the user picked in the Regions card, or
    // clicked on the canvas. A canvas click leaves the view where it is
    // (focusView = false): the region is already on screen.
    void activateAraRegionByKey(const juce::String& regionKey,
                                bool focusView = true);
    // Make a live region the canvas region and republish the lists.
    void activateAraRegion(juce::ARAPlaybackRegion* target, bool focusView);
    // Switch the canvas to a track the user clicked in the left side panel:
    // the region last edited on that track, else its first region in time.
    void activateAraTrackByKey(const juce::String& trackKey);
    // Push a preview of every region on the active region's track to the
    // canvas and thumbnail, rebuilding only regions whose content changed.
    void publishAraRegionPreviews(
        const std::vector<juce::ARAPlaybackRegion*>& regions,
        const std::vector<MainViewRegionEntry>& entries,
        const juce::String& activeKey);
    // Push every track (region sequence) in the ARA document - i.e. every
    // track that has PitchNet on it - to the left side panel, marking the
    // track of the active region. Only republishes when something changed.
    void publishAraTrackList(
        const std::vector<juce::ARAPlaybackRegion*>& regions,
        const std::vector<MainViewRegionEntry>& entries,
        const juce::String& activeKey);
    // ARAEditorView::Listener. When the host selection changes, switch the
    // canvas to the selected region's per-region Project (each region/track is
    // analysed and edited independently). Format-agnostic: works in AAX/VST3/AU.
    void onNewSelection(const juce::ARAViewSelection& viewSelection) override;
#endif

    void requestMainViewKeyboardFocus();
    void requestMainViewKeyboardFocusAsync();
    void applyLunaSoftwareRenderer();

    PitchNetAudioProcessor& audioProcessor;
    std::unique_ptr<IMainView> mainView;
    double lastSyncedHostPlayheadSeconds = 0.0;
    bool lastSyncedHostPlayState = false;
    bool hasSyncedHostPlayhead = false;
#if JucePlugin_Enable_ARA
    juce::String lastPublishedRegionSignature;
    bool regionListPublished = false;
    struct RegionPreviewCacheEntry {
        const Project* project = nullptr;
        bool fromCanvas = false;
        std::uint64_t revision = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        double startInModificationSeconds = 0.0;
        MainViewRegionPreview preview;
    };
    std::map<juce::String, RegionPreviewCacheEntry> regionPreviewCache;
    juce::String lastPublishedPreviewSignature;
    juce::String lastPublishedTrackSignature;
    // Track key -> selector of the region last active on that track, so
    // switching back to a track returns to the clip you were editing.
    std::map<juce::String, juce::String> lastActiveRegionByTrack;
    // Tracks pinned in the track list (session-local keys). Their notes are
    // drawn on the canvas. Never contains the active track.
    std::set<juce::String> pinnedTrackKeys;
    // Tracks open for editing next to the active one (Edit in the track
    // list): their clips are editable windows on the canvas (Track mode).
    std::set<juce::String> araCoEditTrackKeys;
    // The active track (the one the canvas shows) can be switched off or
    // made a ghost too, as the own track is without ARA.
    bool araOwnTrackEditable = true;
    juce::String araPreviousActiveKey;
    void setAraOwnTrackEditable(bool editable);
    // The track holding the region the canvas shows (last published).
    juce::String araActiveTrackKey;
    int regionListRefreshCountdown = 0;

    // Lead and doubles in ARA hosts (REAPER): tracks are region sequences,
    // each clip has its own project, and a track's group is kept in its
    // clips' projects (saved with the host project).
    struct AraClip {
        juce::ARAPlaybackRegion* region = nullptr;
        PitchNetAudioModification* modification = nullptr;
        juce::String key;               // the modification's key
        Project* project = nullptr;     // nullptr until analysed
        bool fromCanvas = false;
        std::uint64_t revision = 0;
        double offsetSeconds = 0.0;     // timeline start - start in its audio
        double startSeconds = 0.0;      // on the timeline
        double endSeconds = 0.0;
    };
    // Track key -> its clips (one per modification), in host track order.
    std::vector<std::pair<juce::String, std::vector<AraClip>>> collectAraClips();
    std::map<juce::String, Project::DoubleGroup> araGroups; // by track key
    std::set<juce::String> araGroupsRead;                   // tracks read from their clips
    void readAraGroups(const std::vector<std::pair<juce::String, std::vector<AraClip>>>& tracks);
    void setAraTrackGroup(const juce::String& trackKey, const Project::DoubleGroup& group);
    void persistAraClip(const AraClip& clip);
    // Saving a track's clips into the host project serialises every one of
    // them: done once changes stop (a slider drag is many changes), not on
    // each. Flushed when the window closes.
    std::set<juce::String> araPersistPending;
    double araPersistDueMs = 0.0;
    bool araRefreshQueued = false;
    void persistAraTrackLater(const juce::String& trackKey);
    void flushAraPersist(bool now);
    void refreshAraRegionListSoon();
    void setupAraGroupCallbacks();
    void findAraDoubles();
    // Doubles follow their lead: run from the region-list refresh.
    void syncAraDoubles();
    std::map<juce::String, juce::String> araLeadSeen;     // lead track -> its state last tick
    std::map<juce::String, juce::String> araDoubleApplied; // clip -> state applied
#endif
    bool lunaSoftwareRendererApplied = false;

    // Hosts without ARA (FL Studio): every PitchNet instance in the process
    // is listed as a track, from InstanceRegistry. Pinned
    // ("ghost") tracks have their notes drawn on this canvas.
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void publishInstanceTracks();

    // The instance this window edits. Its own processor at first; clicking
    // another track in the list re-targets the window to that instance, the
    // way closing this window and opening that one would.
    PitchNetAudioProcessor* targetProcessor = nullptr;
    std::uint64_t targetId = 0;
    PitchNetAudioProcessor& target() { return *targetProcessor; }
    bool targetIsAlive() const;
    void detachFromTarget();
    void attachToTarget(PitchNetAudioProcessor& processor);
    void retargetTo(PitchNetAudioProcessor& processor);
    // Ctrl+Edit (hosts without ARA): other instances' tracks open
    // for editing in this canvas. Each is a working copy of that instance's
    // project; edits are rendered by that instance.
    struct CoEditTrack {
        std::shared_ptr<Project> project;
        std::shared_ptr<bool> alive;     // false once closed; guards undo
        std::uint64_t seenRevision = 0;  // registry revision last merged
        bool renderPending = false;      // our edit is being rendered
        // Set when that track's own window is open (several
        // windows edit the same tracks): `project` is then that window's
        // live project, borrowed, and edits are rendered by that window.
        IMainView* liveView = nullptr;
    };
    // Let go of every co-edit borrowing `view`'s project, in every window:
    // called before that project goes away or changes hands.
    static void releaseLiveView(const IMainView* view);
    std::vector<std::uint64_t> pendingCoEditReopen;
    juce::String lastSharedKeySignature;
    Skin::Id lastSkin = Skin::active();
    // The track this window belongs to, switched on for editing (Edit).
    bool ownTrackEditable = true;
    void setOwnTrackEditable(bool editable);
    void dropLiveCoEditsOn(const IMainView* view);
    std::map<std::uint64_t, CoEditTrack> coEditTracks;
    // Lead / doubles: suggest groups from the tracks' notes, then ask.
  void findDoubles();
  void toggleCoEdit(const juce::String& key);
    void closeCoEdit(std::uint64_t id);
    void syncCoEditTracks(const std::vector<InstanceRegistry::Entry>& entries);
    std::vector<TrackPitchMacroTarget> buildTrackEditTargets();
    // Other tracks mixed under a local preview (Editing Mix Fader).
    std::shared_ptr<const juce::AudioBuffer<float>> buildPreviewMixBed(double startSeconds,
                                                                       double endSeconds);

    // Track header actions (hosts without ARA).
    void editTrack(const juce::String& key);
    void setReferenceTrack(const juce::String& key, bool reference);
    void setTrackTransfer(const juce::String& key, bool armed);
    friend void pitchNetEditorsForgetProcessor(PitchNetAudioProcessor*);
    bool instanceTracksEnabled = false;
    std::set<juce::String> pinnedInstanceKeys;
    // Snapshots the published ghost previews point into; kept alive here.
    std::map<std::uint64_t, std::shared_ptr<const Project>> ghostSnapshots;
    juce::String lastInstanceTrackSignature;
    juce::String lastInstancePreviewSignature;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchNetAudioProcessorEditor)
};
