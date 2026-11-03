#pragma once

#include "../JuceHeader.h"
#include "Note.h"
#include <array>
#include <vector>
#include <memory>
#include <utility>

/**
 * Container for audio data and extracted features.
 */
struct AudioData
{
    struct SegmentDebugEvent
    {
        int startFrame = 0;
        int endFrame = 0;
        int attachedStartFrame = 0;
        float midiNote = 0.0f;
        bool isRest = false;
        float durationSeconds = 0.0f;
        int durationFrames = 0;
    };

    struct SegmentDebugChunk
    {
        int chunkIndex = 0;
        int startFrame = 0;
        int endFrame = 0;
        int shortRestThreshold = 0;
        std::vector<SegmentDebugEvent> events;
    };

    juce::AudioBuffer<float> waveform;
    juce::AudioBuffer<float> originalWaveform; // pristine copy for blend (never modified after analysis)

    int sampleRate = 44100;
    double timelineOffsetSeconds = 0.0;
    // Absolute host-timeline ranges occupied by ARA playback regions.
    // Empty for standalone/non-ARA projects.
    std::vector<std::pair<double, double>> playbackRegionRanges;

    // Extracted features
    std::vector<std::vector<float>> melSpectrogram;      // [T, NUM_MELS]
    // Pitch analysis stages, all aligned to the vocoder/mel frame grid.
    std::vector<float> rawF0;                            // [T] detector observations (0 = unvoiced)
    std::vector<float> cleanedF0;                        // [T] rawF0 after isolated-jump repair
    std::vector<float> denseF0;                          // [T] cleanedF0 with log-domain UV interpolation
    std::vector<float> f0;                               // [T] current composed/edited dense F0
    std::vector<float> baseF0;                           // [T] (cached base pitch in Hz)
    std::vector<float> basePitch;                        // [T] base pitch in MIDI (dense)
    std::vector<float> deltaPitch;                       // [T] delta pitch in MIDI (dense)
    std::vector<bool> voicedMask;                        // [T] uv mask (true = voiced, F0-based)
    std::vector<bool> vadMask;                           // [T] energy-based VAD (true = has audio energy, captures consonants)
    std::vector<std::pair<int, int>> segmentChunkRanges; // [N] GAME slicer chunks in frame range [start, end)
    // Timing alignment fingerprints (TimingAligner), made from the mel for
    // the light copies other tracks read; empty in full projects.
    std::vector<float> alignFeatures;
    std::vector<SegmentDebugChunk> segmentDebugChunks;   // raw GAME outputs for debug visualization

    float getDuration() const
    {
        if (waveform.getNumSamples() == 0)
            return 0.0f;
        return static_cast<float>(waveform.getNumSamples()) / sampleRate;
    }

    int getNumFrames() const
    {
        return static_cast<int>(melSpectrogram.size());
    }
};

/**
 * Loop playback range in seconds.
 */
struct LoopRange
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    bool enabled = false;

    bool isValid() const { return enabled && endSeconds > startSeconds; }
};

/**
 * Scale mode used for piano-roll grid coloring.
 */
enum class ScaleMode : int
{
    None = -1,
    Chromatic = 0,
    Major,
    Minor,
    Blues,
    Dorian,
    HarmonicMinor,
    Locrian,
    Lydian,
    MajorPentatonic,
    MelodicMinor,
    MinorPentatonic,
    Mixolydian,
    Phrygian,
    PhrygianDominant,
    WholeTone,
    // The user's own scale (CustomScale in the shared MacroParameters):
    // any set of notes, each detuned by its own cents.
    Custom = 100
};

/** A scale the user made: which of the 12 notes it uses (C = 0 ... B = 11,
    absolute, not relative to a root) and how many cents each one is tuned
    away from equal temperament. Notes are tuned to these pitches. */
struct CustomScale
{
    juce::String name;
    std::array<bool, 12> used{{true, false, true, false, true, true, false, true, false, true, false, true}};
    std::array<float, 12> cents{};
};

/** A key change on the host timeline: from `seconds` on, this key applies. */
struct KeyMarker
{
    double seconds = 0.0;
    int root = 0;
    ScaleMode mode = ScaleMode::Major;
};

enum class DragSnapMode : int
{
    Chromatic = 0,
    Scale
};

/**
 * Timeline ruler mode.
 */
enum class TimelineDisplayMode : int
{
    Beats = 0,
    Time
};

/**
 * Beat-grid subdivision expressed as note denominator (1/x).
 */
enum class TimelineGridDivision : int
{
    Whole = 1,
    Half = 2,
    Quarter = 4,
    Eighth = 8,
    Sixteenth = 16,
    ThirtySecond = 32
};

struct MacroParameters
{
    ScaleMode scaleMode = ScaleMode::Chromatic;
    int projectKeyRoot = 0;
    ScaleMode projectKeyMode = ScaleMode::Major;
    bool projectKeyInitialized = false;
    int pitchReferenceHz = 440;
    bool snapToSemitones = false;
    DragSnapMode dragSnapMode = DragSnapMode::Chromatic;
    TimelineDisplayMode timelineDisplayMode = TimelineDisplayMode::Beats;
    int timelineBeatNumerator = 4;
    int timelineBeatDenominator = 4;
    double timelineTempoBpm = 120.0;
    TimelineGridDivision timelineGridDivision = TimelineGridDivision::Quarter;
    bool timelineSnapCycle = false;
    CustomScale customScale;
    std::vector<KeyMarker> keyMarkers; // sorted by time
};

/** One set of these for the whole plug-in process: every track shares the
    key, A4, custom scale and key changes, and changing them in one window
    changes them for all (multitrack editing). */
inline std::shared_ptr<MacroParameters> &sharedMacroParameters()
{
    static auto parameters = std::make_shared<MacroParameters>();
    return parameters;
}

/**
 * Project data container.
 */
class Project
{
public:
    Project();
    ~Project() = default;

    // Presentation state saved with the project; zero size uses app defaults.
    int uiWidth = 0;
    int uiHeight = 0;
    bool thumbnailVisible = false;

    // File operations
    void setFilePath(const juce::File &file) { filePath = file; }
    juce::File getFilePath() const { return filePath; }
    void setProjectFilePath(const juce::File &file) { projectFilePath = file; }
    juce::File getProjectFilePath() const { return projectFilePath; }
    void setAudioSha256(const juce::String &sha) { audioSha256 = sha; }
    juce::String getAudioSha256() const { return audioSha256; }
    juce::String getName() const { return name; }
    void setName(const juce::String &n) { name = n; }

    // Audio data
    AudioData &getAudioData() { return audioData; }
    const AudioData &getAudioData() const { return audioData; }

    // Notes
    std::vector<Note> &getNotes() { return notes; }
    const std::vector<Note> &getNotes() const { return notes; }
    void addNote(Note note);
    void clearNotes() { notes.clear(); }

    Note *getNoteAtFrame(int frame);
    std::vector<Note *> getNotesInRange(int startFrame, int endFrame);
    std::vector<Note *> getSelectedNotes();
    bool removeNoteByStartFrame(int startFrame);
    std::vector<Note *> getDirtyNotes();
    void selectAllNotes(bool includeRests = false);
    void deselectAllNotes();
    void clearAllDirty();

    // Global settings
    float getGlobalPitchOffset() const { return globalPitchOffset; }
    void setGlobalPitchOffset(float offset) { globalPitchOffset = offset; }

    float getFormantShift() const { return formantShift; }
    void setFormantShift(float shift) { formantShift = shift; }

    float getVolume() const { return volume; }
    void setVolume(float vol) { volume = vol; }

    // Per-project pitch-center correction strength (0-100%).  ARA regions
    // carry their own Project, so this remains independent per region.
    float getPitchCenter() const { return pitchCenter; }
    void setPitchCenter(float amount)
    {
        const float normalized = juce::jlimit(0.0f, 100.0f, amount);
        if (std::abs(pitchCenter - normalized) > 0.0001f)
        {
            pitchCenter = normalized;
            modified = true;
        }
    }

    // Get adjusted F0 with all modifications applied
    std::vector<float> getAdjustedF0() const;

    // Get adjusted F0 for a specific frame range
    std::vector<float> getAdjustedF0ForRange(int startFrame, int endFrame) const;

    // Get frame range that needs resynthesis (based on dirty notes)
    // Returns {-1, -1} if no dirty notes
    std::pair<int, int> getDirtyFrameRange() const;

    // Check if any notes are dirty
    bool hasDirtyNotes() const;

    // F0 curve dirty tracking (for pitch tools and undo/redo)
    void setF0DirtyRange(int startFrame, int endFrame);

    // The one F0 dirty range for note edits that re-render audio (pitch drag,
    // nudge, snap, pitch macro, pitch tools, formant, curve drawing) and for
    // their undo, redo and Restore. Pass the edited notes' own frame span; it
    // is padded by 60 frames each side. This range sets the synthesiser's
    // render window, so an edit, its undo and its Restore must mark it the
    // same way or the re-rendered audio around the note differs and clicks.
    void markNoteEditDirtyRange(int startFrame, int endFrame);
    void clearF0DirtyRange();
    bool hasF0DirtyRange() const;
    std::pair<int, int> getF0DirtyRange() const;

    // Modified state
    bool isModified() const { return modified; }
    void setModified(bool mod) { modified = mod; }

    // Loop range
    const LoopRange &getLoopRange() const { return loopRange; }
    void setLoopRange(double startSeconds, double endSeconds);
    void setLoopEnabled(bool enabled);
    void clearLoopRange();

    // Replace Ranges (Transfer without ARA): where this track's
    // audio replaces the DAW track, in project seconds. Empty: everywhere.
    const std::vector<std::pair<double, double>> &getReplaceRanges() const { return replaceRanges; }

    // Lead and doubles in ARA hosts (REAPER): the track's group, kept in
    // each of its regions' projects so it is saved with the host project.
    // Hosts without ARA keep it in the plug-in state instead.
    struct DoubleGroup
    {
        juce::String id;
        int role = 0; // 1 lead, 2 double
        bool unisonOn = false;
        float unisonStrength = 1.0f;
        bool timingOn = false;
        float timingStrength = 1.0f;
    };
    const DoubleGroup &getDoubleGroup() const { return doubleGroup; }
    void setDoubleGroup(DoubleGroup group) { doubleGroup = std::move(group); }
    void setReplaceRanges(std::vector<std::pair<double, double>> ranges) { replaceRanges = std::move(ranges); }

    // Piano-roll scale visualization
    ScaleMode getScaleMode() const {
        const auto mode = macroParameters->scaleMode;
        return mode == ScaleMode::None || mode == ScaleMode::Chromatic ? mode : getPreferredScaleMode();
    }
    void setScaleMode(ScaleMode mode);
    ScaleMode getPreferredScaleMode() const { return useProjectKey ? getProjectKeyMode() : preferredScaleMode; }
    void setPreferredScaleMode(ScaleMode mode);
    int getScaleRootNote() const { return useProjectKey ? getProjectKeyRoot() : scaleRootNote; }
    void setScaleRootNote(int noteInOctave);
    void setDetectedKey(int root, ScaleMode mode) {
        detectedKeyRoot = juce::jlimit(0, 11, root);
        detectedKeyMode = mode;
        modified = true;
    }
    int getDetectedScaleRootNote() const { return detectedKeyRoot; }
    ScaleMode getDetectedScaleMode() const { return detectedKeyMode; }
    int getRegionKeyRoot() const { return scaleRootNote; }
    ScaleMode getRegionKeyMode() const { return preferredScaleMode; }
    bool getUseProjectKey() const { return useProjectKey; }
    void setUseProjectKey(bool inherit);
    int getProjectKeyRoot() const { return macroParameters->projectKeyRoot; }
    ScaleMode getProjectKeyMode() const { return macroParameters->projectKeyMode; }
    void setProjectKey(int root, ScaleMode mode);

    int getPitchReferenceHz() const { return macroParameters->pitchReferenceHz; }
    void setPitchReferenceHz(int hz);
    bool getSnapToSemitones() const { return macroParameters->snapToSemitones; }
    void setSnapToSemitones(bool enabled);
    DragSnapMode getDragSnapMode() const { return macroParameters->dragSnapMode; }
    void setDragSnapMode(DragSnapMode mode);

    // Timeline/grid settings
    TimelineDisplayMode getTimelineDisplayMode() const { return macroParameters->timelineDisplayMode; }
    void setTimelineDisplayMode(TimelineDisplayMode mode);
    int getTimelineBeatNumerator() const { return macroParameters->timelineBeatNumerator; }
    int getTimelineBeatDenominator() const { return macroParameters->timelineBeatDenominator; }
    void setTimelineBeatSignature(int numerator, int denominator);
    double getTimelineTempoBpm() const { return macroParameters->timelineTempoBpm; }
    void setTimelineTempoBpm(double bpm);
    TimelineGridDivision getTimelineGridDivision() const { return macroParameters->timelineGridDivision; }
    void setTimelineGridDivision(TimelineGridDivision division);
    bool getTimelineSnapCycle() const { return macroParameters->timelineSnapCycle; }
    void setTimelineSnapCycle(bool enabled);

    std::shared_ptr<MacroParameters> getMacroParameters() const
    {
        return macroParameters;
    }
    void setMacroParameters(std::shared_ptr<MacroParameters> parameters);

    /** A copy for other windows to read: notes, pitch curves and settings,
        without the mel spectrogram, the original audio or debug data (and,
        unless `keepWaveform`, without the rendered audio either). Copying
        only this is what keeps sharing a track cheap. */
    std::shared_ptr<Project> makeLightCopy(bool keepWaveform) const;

private:
    juce::String name = "Untitled";
    juce::File filePath;
    juce::File projectFilePath;
    juce::String audioSha256;

    AudioData audioData;
    std::vector<Note> notes;

    float globalPitchOffset = 0.0f;
    float formantShift = 0.0f;
    float volume = 0.0f; // dB
    float pitchCenter = 0.0f;

    // F0 direct edit dirty range
    int f0DirtyStart = -1;
    int f0DirtyEnd = -1;

    bool modified = false;

    LoopRange loopRange;
    std::vector<std::pair<double, double>> replaceRanges;
    DoubleGroup doubleGroup;
    bool useProjectKey = true;
    int detectedKeyRoot = 0;
    ScaleMode detectedKeyMode = ScaleMode::Major;
    ScaleMode preferredScaleMode = ScaleMode::Major;
    int scaleRootNote = 0; // 0 = C, 1 = C#, ..., 11 = B
    std::shared_ptr<MacroParameters> macroParameters;
};
