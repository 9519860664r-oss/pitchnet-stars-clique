#include "UnisonCoordinator.h"

#include "InstanceRegistry.h"
#include "PluginProcessor.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../UI/IMainView.h"
#include "../Utils/DoubleTuning.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/BasePitchCurve.h"
#include "../Utils/PerfTrace.h"
#include "../Utils/NoteTimeMap.h"
#include "../Utils/TimingAligner.h"

#include <algorithm>
#include <cmath>

#include <limits>
#include <memory>

namespace
{
UnisonCoordinator *instance = nullptr;
int users = 0;

bool hasUnisonNotes(const Project *project)
{
  if (project == nullptr)
    return false;
  for (const auto &note : project->getNotes())
    if (note.hasUnisonSource())
      return true;
  return false;
}

/** A project's fingerprints along its output timeline: retimed notes read
    their source frames through their time maps. From its stored features
    (a published copy) or from its mel (a full project). */
TimingAligner::Features outputFeatures(const Project &project)
{
  TimingAligner::Features features;
  const auto &audio = project.getAudioData();
  constexpr int dims = TimingAligner::kDims;
  std::vector<float> source;
  if (!audio.alignFeatures.empty())
    source = audio.alignFeatures;
  else if (!audio.melSpectrogram.empty())
    source = TimingAligner::fromMel(audio.melSpectrogram, audio.voicedMask).data;
  const int frames = static_cast<int>(source.size()) / dims;
  if (frames <= 0)
    return features;
  features.frames = frames;
  features.data = source;
  for (const auto &note : project.getNotes())
  {
    if (note.isRest() || (note.getStartFrame() == note.getSrcStartFrame() &&
                          note.getEndFrame() == note.getSrcEndFrame() &&
                          note.getWarpAnchors().empty()))
      continue;
    const auto knots = NoteTimeMap::knots(note);
    for (int t = std::max(0, note.getStartFrame()); t < std::min(frames, note.getEndFrame()); ++t)
    {
      const int s = std::clamp(static_cast<int>(std::lround(NoteTimeMap::sourceFrameFor(knots, t))), 0,
                               frames - 1);
      std::copy(source.begin() + static_cast<std::ptrdiff_t>(s) * dims,
                source.begin() + static_cast<std::ptrdiff_t>(s + 1) * dims,
                features.data.begin() + static_cast<std::ptrdiff_t>(t) * dims);
    }
  }
  return features;
}

/** The lead's fingerprints along its output timeline (it publishes them). */
TimingAligner::Features guideFeatures(const Project &lead)
{
  if (lead.getAudioData().alignFeatures.empty())
    return {};
  return outputFeatures(lead);
}

/** True where a note sounds pitched: voiced, and not in its sibilants. */
bool pitchedAt(const std::vector<NoteTimeMap::Knot> &knots, const NoteTimeMap::SibilantBounds &bounds,
               const std::vector<bool> &voiced, int frame)
{
  if (frame + 0.5 < bounds.headEnd || frame + 0.5 >= bounds.tailStart)
    return false;
  if (voiced.empty())
    return true;
  const int s = static_cast<int>(std::lround(NoteTimeMap::sourceFrameFor(knots, frame)));
  return s >= 0 && s < static_cast<int>(voiced.size()) && voiced[static_cast<size_t>(s)];
}

/** The base curve rendering builds for notes at these pitches (the same
    segments PitchCurveProcessor makes from them). */
std::vector<float> baseCurveFor(const std::vector<Note *> &notes, const std::vector<float> &midi,
                                int totalFrames)
{
  std::vector<BasePitchCurve::NoteSegment> segments;
  segments.reserve(notes.size());
  for (size_t i = 0; i < notes.size(); ++i)
    segments.push_back({notes[i]->getStartFrame(), notes[i]->getEndFrame(),
                        midi[i] + notes[i]->getPitchOffset() -
                            (notes[i]->getTiltLeft() + notes[i]->getTiltRight()) / 2.0f});
  std::sort(segments.begin(), segments.end(),
            [](const auto &a, const auto &b) { return a.startFrame < b.startFrame; });
  auto base = BasePitchCurve::generateForNotes(segments, totalFrames);
  base.resize(static_cast<size_t>(std::max(0, totalFrames)), 0.0f);
  return base;
}

/** A contour computed per output frame, stored the way rendering reads it:
    over the note's source frames when its time map is uneven and the two
    lengths are equal (otherwise it is taken as output frames). */
std::vector<float> storedContour(const Note &note, std::vector<float> outputContour)
{
  const auto knots = NoteTimeMap::knots(note);
  const int frames = note.getEndFrame() - note.getStartFrame();
  if (!NoteTimeMap::isUneven(knots) || note.getSrcDurationFrames() != frames || frames <= 0)
    return outputContour;
  std::vector<float> source(static_cast<size_t>(frames));
  for (int j = 0; j < frames; ++j)
  {
    const double out = NoteTimeMap::outputAt(knots, note.getSrcStartFrame() + j + 0.5) - 0.5 -
                       note.getStartFrame();
    const double pos = std::clamp(out, 0.0, static_cast<double>(frames - 1));
    const auto left = static_cast<size_t>(std::floor(pos));
    const auto right = std::min(left + 1, outputContour.size() - 1);
    const auto t = static_cast<float>(pos - static_cast<double>(left));
    source[static_cast<size_t>(j)] = outputContour[left] * (1.0f - t) + outputContour[right] * t;
  }
  return source;
}

/** Applies (or undoes) unison on `project`. Returns true when a note moved.
    With `dryRun` nothing is changed: it only tells whether anything would
    (so a track is copied and re-rendered only when it has to be).
    Natural unison (DoubleTuning::unisonOffsets): the double keeps its own
    vibrato, jitter and transitions and takes the lead's note centres and
    drift, compared at the moments both sing the same sound. */
bool applyUnison(Project &project, const Project *lead, bool on, float strength,
                 bool dryRun = false)
{
  std::vector<Note *> notes;
  for (auto &note : project.getNotes())
    if (!note.isRest() && note.getEndFrame() > note.getStartFrame())
      notes.push_back(&note);
  std::sort(notes.begin(), notes.end(),
            [](const Note *a, const Note *b) { return a->getStartFrame() < b->getStartFrame(); });

  int first = std::numeric_limits<int>::max(), last = -1;
  auto touched = [&](const Note &note) {
    first = std::min(first, note.getStartFrame());
    last = std::max(last, note.getEndFrame());
  };
  auto finish = [&]() {
    if (dryRun || last < first)
      return false;
    PitchCurveProcessor::rebuildBaseFromNotes(project);
    project.markNoteEditDirtyRange(first, last);
    project.setModified(true);
    return true;
  };

  const auto &audio = project.getAudioData();
  int totalFrames = audio.getNumFrames();
  if (totalFrames <= 0)
    totalFrames = static_cast<int>(audio.basePitch.size());
  const bool active = on && lead != nullptr && !notes.empty() && totalFrames > 0 &&
                      !lead->getAudioData().basePitch.empty();

  if (!active)
  {
    // Back to its own pitch and contour.
    for (auto *note : notes)
    {
      if (!note->hasUnisonSource())
        continue;
      if (dryRun)
        return true;
      note->setMidiNoteFromPitchCorrection(note->getUnisonSourceMidi());
      if (note->getUnisonSourceWasBaked())
        note->setBakedDeltaPitch(note->getUnisonSourceDelta());
      else
        note->clearBakedDeltaPitch();
      note->clearUnisonSource();
      note->markDirty();
      note->markSynthDirty();
      touched(*note);
    }
    return finish();
  }

  // The double as sung: each note's own pitch and contour.
  std::vector<float> sourceMidi(notes.size());
  std::vector<std::vector<float>> sourceDelta(notes.size());
  std::vector<char> sourceBaked(notes.size());
  for (size_t i = 0; i < notes.size(); ++i)
  {
    const auto *note = notes[i];
    const bool had = note->hasUnisonSource();
    sourceMidi[i] = had ? note->getUnisonSourceMidi() : note->getMidiNote();
    sourceDelta[i] = had ? note->getUnisonSourceDelta() : note->getActiveDeltaPitch();
    sourceBaked[i] = had ? note->getUnisonSourceWasBaked() : note->hasBakedDeltaPitch();
  }
  const auto ownBase = baseCurveFor(notes, sourceMidi, totalFrames);
  const float nan = std::nanf("");
  std::vector<float> ownPitch(static_cast<size_t>(totalFrames), nan);   // every note frame
  std::vector<float> ownPitched(static_cast<size_t>(totalFrames), nan); // pitched frames only
  std::vector<int> noteOf(static_cast<size_t>(totalFrames), -1);
  for (size_t i = 0; i < notes.size(); ++i)
  {
    const auto &note = *notes[i];
    const int frames = note.getEndFrame() - note.getStartFrame();
    const auto delta = sourceDelta[i].empty()
                           ? std::vector<float>(static_cast<size_t>(frames), 0.0f)
                           : NoteTimeMap::toOutput(sourceDelta[i], note, frames);
    const auto knots = NoteTimeMap::knots(note);
    const auto bounds = NoteTimeMap::sibilantBounds(note);
    for (int t = std::max(0, note.getStartFrame()); t < std::min(totalFrames, note.getEndFrame()); ++t)
    {
      const float pitch =
          ownBase[static_cast<size_t>(t)] + delta[static_cast<size_t>(t - note.getStartFrame())];
      ownPitch[static_cast<size_t>(t)] = pitch;
      noteOf[static_cast<size_t>(t)] = static_cast<int>(i);
      if (pitchedAt(knots, bounds, audio.voicedMask, t))
        ownPitched[static_cast<size_t>(t)] = pitch;
    }
  }

  // The lead's pitch wherever it sings pitched, and its slow part: its
  // note pitches (base curve) plus the slow movement inside each note.
  const auto &leadAudio = lead->getAudioData();
  const int leadFrames =
      static_cast<int>(std::min(leadAudio.basePitch.size(), leadAudio.deltaPitch.size()));
  std::vector<float> leadDelta(static_cast<size_t>(leadFrames), nan);
  std::vector<int> leadNoteOf(static_cast<size_t>(leadFrames), -1);
  {
    std::vector<const Note *> leadNotes;
    for (const auto &note : lead->getNotes())
      if (!note.isRest() && note.getEndFrame() > note.getStartFrame())
        leadNotes.push_back(&note);
    std::sort(leadNotes.begin(), leadNotes.end(),
              [](const Note *a, const Note *b) { return a->getStartFrame() < b->getStartFrame(); });
    for (size_t i = 0; i < leadNotes.size(); ++i)
    {
      const auto &note = *leadNotes[i];
      const auto knots = NoteTimeMap::knots(note);
      const auto bounds = NoteTimeMap::sibilantBounds(note);
      for (int t = std::max(0, note.getStartFrame()); t < std::min(leadFrames, note.getEndFrame()); ++t)
      {
        leadNoteOf[static_cast<size_t>(t)] = static_cast<int>(i);
        const float base = leadAudio.basePitch[static_cast<size_t>(t)];
        const float delta = leadAudio.deltaPitch[static_cast<size_t>(t)];
        if (base > 0.0f && std::isfinite(delta) && pitchedAt(knots, bounds, leadAudio.voicedMask, t))
          leadDelta[static_cast<size_t>(t)] = delta;
      }
    }
  }
  const auto leadSlow = DoubleTuning::slowWithinNotes(leadDelta, leadNoteOf);
  std::vector<float> leadRef(static_cast<size_t>(leadFrames), nan);
  for (int t = 0; t < leadFrames; ++t)
    if (std::isfinite(leadDelta[static_cast<size_t>(t)]) && std::isfinite(leadSlow[static_cast<size_t>(t)]))
      leadRef[static_cast<size_t>(t)] = leadAudio.basePitch[static_cast<size_t>(t)] + leadSlow[static_cast<size_t>(t)];

  // Which lead moment each double frame sings (the same sound, found the
  // way Timing finds it); the same frame when either has no fingerprints.
  std::vector<double> shift;
  {
    const auto dub = outputFeatures(project);
    const auto guide = guideFeatures(*lead);
    if (!dub.empty() && !guide.empty())
    {
      std::vector<std::pair<int, int>> spans;
      for (const auto *note : notes)
        spans.emplace_back(note->getStartFrame(), note->getEndFrame());
      TimingAligner::Options options;
      options.strength = 1.0;
      options.toleranceFrames = 0;
      shift = TimingAligner::offsets(dub, guide, TimingAligner::phrasesFrom(spans), options);
    }
  }
  std::vector<float> leadAt(static_cast<size_t>(totalFrames), nan);
  for (int t = 0; t < totalFrames; ++t)
  {
    if (noteOf[static_cast<size_t>(t)] < 0)
      continue;
    const double move = t < static_cast<int>(shift.size()) ? shift[static_cast<size_t>(t)] : 0.0;
    const int g = static_cast<int>(std::lround(t + move));
    if (g >= 0 && g < leadFrames)
      leadAt[static_cast<size_t>(t)] = leadRef[static_cast<size_t>(g)];
  }

  // The double's slow part the same way: its note pitches plus the slow
  // movement inside each note. What is left of its pitch is its own life.
  std::vector<float> ownDelta(static_cast<size_t>(totalFrames), nan);
  for (int t = 0; t < totalFrames; ++t)
    if (std::isfinite(ownPitched[static_cast<size_t>(t)]))
      ownDelta[static_cast<size_t>(t)] = ownPitched[static_cast<size_t>(t)] - ownBase[static_cast<size_t>(t)];
  const auto ownSlow = DoubleTuning::slowWithinNotes(ownDelta, noteOf);
  std::vector<float> ownRef(static_cast<size_t>(totalFrames), nan);
  for (int t = 0; t < totalFrames; ++t)
    if (std::isfinite(ownDelta[static_cast<size_t>(t)]) && std::isfinite(ownSlow[static_cast<size_t>(t)]))
      ownRef[static_cast<size_t>(t)] = ownBase[static_cast<size_t>(t)] + ownSlow[static_cast<size_t>(t)];

  const auto offset = DoubleTuning::unisonOffsets(ownRef, leadAt, noteOf);
  const float s = std::clamp(strength, 0.0f, 1.0f);

  // New note pitches (their centres moved by the offset), then contours
  // that make base + contour the double's own pitch plus the offset.
  std::vector<float> targetMidi(notes.size());
  for (size_t i = 0; i < notes.size(); ++i)
  {
    const auto &note = *notes[i];
    double sum = 0.0;
    int count = 0;
    for (int t = std::max(0, note.getStartFrame()); t < std::min(totalFrames, note.getEndFrame()); ++t)
    {
      sum += offset[static_cast<size_t>(t)];
      ++count;
    }
    targetMidi[i] = sourceMidi[i] + s * static_cast<float>(count > 0 ? sum / count : 0.0);
  }
  const auto targetBase = baseCurveFor(notes, targetMidi, totalFrames);

  for (size_t i = 0; i < notes.size(); ++i)
  {
    auto *note = notes[i];
    const int frames = note->getEndFrame() - note->getStartFrame();
    std::vector<float> contour(static_cast<size_t>(frames), 0.0f);
    for (int k = 0; k < frames; ++k)
    {
      const int t = note->getStartFrame() + k;
      if (t >= 0 && t < totalFrames && std::isfinite(ownPitch[static_cast<size_t>(t)]))
        contour[static_cast<size_t>(k)] = ownPitch[static_cast<size_t>(t)] +
                                          s * offset[static_cast<size_t>(t)] -
                                          targetBase[static_cast<size_t>(t)];
    }
    contour = storedContour(*note, std::move(contour));
    const bool moveMidi = std::abs(note->getMidiNote() - targetMidi[i]) > 1.0e-4f;
    const auto &current = note->getBakedDeltaPitch();
    bool moveContour = current.size() != contour.size();
    for (size_t k = 0; !moveContour && k < contour.size(); ++k)
      moveContour = std::abs(current[k] - contour[k]) > 1.0e-4f;
    if (note->hasUnisonSource() && !moveMidi && !moveContour)
      continue;
    if (dryRun)
      return true;
    if (!note->hasUnisonSource())
    {
      note->setUnisonSourceMidi(sourceMidi[i]);
      note->setUnisonSourceDelta(sourceDelta[i], sourceBaked[i] != 0);
    }
    if (moveMidi)
      note->setMidiNoteFromPitchCorrection(targetMidi[i]);
    if (moveContour)
      note->setBakedDeltaPitch(std::move(contour));
    note->markDirty();
    note->markSynthDirty();
    touched(*note);
  }
  return finish();
}

struct NoteTiming
{
  int start = 0;
  int end = 0;
  std::vector<Note::WarpAnchor> anchors;
  bool operator==(const NoteTiming &o) const
  {
    return start == o.start && end == o.end && anchors == o.anchors;
  }
};

/**
 * Applies (or undoes) timing alignment on a double: its
 * sound is matched to the lead's frame by frame, and each note moves (and
 * warps inside) toward where the lead sings the same syllable, by the
 * strength. Its sibilants move without stretching. Switching off puts every
 * note back on its own timing. Returns true when a note moved.
 */
bool applyTiming(Project &project, const Project *lead, bool on, float strength,
                 bool dryRun = false)
{
  std::vector<Note *> notes;
  for (auto &note : project.getNotes())
    if (!note.isRest() && note.getSrcDurationFrames() > 0)
      notes.push_back(&note);
  std::sort(notes.begin(), notes.end(),
            [](const Note *a, const Note *b) { return a->getSrcStartFrame() < b->getSrcStartFrame(); });

  auto ownOf = [](const Note &note) {
    return note.hasAlignSource()
               ? NoteTiming{note.getAlignSourceStart(), note.getAlignSourceEnd(), note.getAlignSourceAnchors()}
               : NoteTiming{note.getStartFrame(), note.getEndFrame(), note.getWarpAnchors()};
  };

  const auto &audio = project.getAudioData();
  const bool active = on && lead != nullptr && !lead->getAudioData().alignFeatures.empty() &&
                      !audio.melSpectrogram.empty() && !notes.empty();
  std::vector<NoteTiming> targets;
  targets.reserve(notes.size());
  if (active)
  {
    const auto dub = TimingAligner::fromMel(audio.melSpectrogram, audio.voicedMask);
    const auto guide = guideFeatures(*lead);
    std::vector<std::pair<int, int>> spans;
    for (const auto *note : notes)
      spans.emplace_back(note->getSrcStartFrame(), note->getSrcEndFrame());
    TimingAligner::Options options;
    options.strength = juce::jlimit(0.0f, 1.0f, strength);
    const auto offsets =
        TimingAligner::offsets(dub, guide, TimingAligner::phrasesFrom(spans), options);
    auto offAt = [&](double src) {
      if (offsets.empty())
        return 0.0;
      const int i = std::clamp(static_cast<int>(std::lround(src)), 0,
                               static_cast<int>(offsets.size()) - 1);
      return offsets[static_cast<size_t>(i)];
    };
    int previousEnd = std::numeric_limits<int>::min();
    for (const auto *note : notes)
    {
      const int s0 = note->getSrcStartFrame(), s1 = note->getSrcEndFrame();
      const int length = s1 - s0;
      int head = std::max(0, note->getSibilantHead());
      int tail = std::max(0, note->getSibilantTail());
      if (note->isAllSibilant() || head + tail >= length)
        head = tail = 0;
      // The musical start and end: after / before the sibilants, which
      // move with them unstretched.
      const double musicalStart = s0 + head, musicalEnd = s1 - tail;
      const double newMusicalStart = musicalStart + offAt(musicalStart);
      const double newMusicalEnd = musicalEnd + offAt(musicalEnd - 1.0);
      NoteTiming t;
      t.start = static_cast<int>(std::lround(newMusicalStart)) - head;
      t.end = static_cast<int>(std::lround(newMusicalEnd)) + tail;
      t.start = std::max(t.start, previousEnd);
      t.end = std::max(t.end, t.start + std::max(2, length / 4));
      // Inside the note: pin the moments that do not follow a straight
      // stretch (a syllable longer or shorter than the lead's).
      const double span = musicalEnd - musicalStart;
      const double newSpan = newMusicalEnd - newMusicalStart;
      if (span > 8.0 && newSpan > 1.0)
        for (double p = musicalStart + 6.0; p < musicalEnd - 3.0; p += 6.0)
        {
          const double dst = p + offAt(p);
          const double straight = newMusicalStart + (p - musicalStart) * newSpan / span;
          if (std::abs(dst - straight) < 0.6)
            continue;
          Note::WarpAnchor anchor;
          anchor.src = static_cast<float>((p - s0) / length);
          anchor.dst = static_cast<float>((dst - t.start) / static_cast<double>(t.end - t.start));
          if (anchor.dst > 0.0f && anchor.dst < 1.0f &&
              (t.anchors.empty() || (anchor.src > t.anchors.back().src && anchor.dst > t.anchors.back().dst)))
            t.anchors.push_back(anchor);
        }
      previousEnd = t.end;
      targets.push_back(std::move(t));
    }
  }

  int first = std::numeric_limits<int>::max(), last = -1;
  for (size_t i = 0; i < notes.size(); ++i)
  {
    auto *note = notes[i];
    const NoteTiming current{note->getStartFrame(), note->getEndFrame(), note->getWarpAnchors()};
    NoteTiming next;
    if (active)
      next = targets[i];
    else if (note->hasAlignSource())
      next = ownOf(*note);
    else
      continue;
    const bool moves = !(next == current);
    if (!moves && (active || !note->hasAlignSource()))
      continue;
    if (dryRun)
      return true;
    if (active && !note->hasAlignSource())
      note->setAlignSource(current.start, current.end, current.anchors);
    first = std::min({first, current.start, next.start});
    last = std::max({last, current.end, next.end});
    note->setStartFrame(next.start);
    note->setEndFrame(next.end);
    note->setWarpAnchors(next.anchors);
    if (!active)
      note->clearAlignSource();
    note->markDirty();
    note->markSynthDirty();
  }
  if (dryRun || last < first)
    return false;
  PitchCurveProcessor::rebuildBaseFromNotes(project);
  project.markNoteEditDirtyRange(first, last);
  project.setModified(true);
  return true;
}

bool hasAlignedNotes(const Project *project)
{
  if (project == nullptr)
    return false;
  for (const auto &note : project->getNotes())
    if (note.hasAlignSource())
      return true;
  return false;
}

/** Timing first (the doubles' syllables to the lead's), then pitch. */
bool applyDoubles(Project &project, const Project *lead, bool unisonOn, float unisonStrength,
                  bool timingOn, float timingStrength, bool dryRun = false)
{
  const bool timed = applyTiming(project, lead, timingOn, timingStrength, dryRun);
  if (dryRun && timed)
    return true;
  const bool tuned = applyUnison(project, lead, unisonOn, unisonStrength, dryRun);
  return timed || tuned;
}
} // namespace

void UnisonCoordinator::retain()
{
  if (users++ == 0 && instance == nullptr)
    instance = new UnisonCoordinator();
}

void UnisonCoordinator::release()
{
  if (--users <= 0)
  {
    users = 0;
    delete instance;
    instance = nullptr;
  }
}

bool UnisonCoordinator::applyToDouble(Project &project, const Project *lead, bool unisonOn,
                                      float unisonStrength, bool timingOn, float timingStrength)
{
  return applyDoubles(project, (unisonOn || timingOn) ? lead : nullptr, unisonOn, unisonStrength,
                      timingOn, timingStrength);
}

void UnisonCoordinator::poke()
{
  if (instance != nullptr)
    instance->update();
}

// A lead edit reaches its doubles this long after the last change (a burst
// of changes, such as a macro preview, is followed once it stops).
constexpr int kFollowDelayMs = 90;

UnisonCoordinator::UnisonCoordinator()
{
  InstanceRegistry::get().addChangeListener(this);
}

UnisonCoordinator::~UnisonCoordinator()
{
  InstanceRegistry::get().removeChangeListener(this);
}

void UnisonCoordinator::changeListenerCallback(juce::ChangeBroadcaster *)
{
  // Restarts the wait on every change; a pass already running picks the
  // change up on its next step instead.
  if (!passRunning)
    startTimer(kFollowDelayMs);
}

void UnisonCoordinator::timerCallback()
{
  stopTimer();
  update();
}

void UnisonCoordinator::update()
{
  const auto entries = InstanceRegistry::get().list();
  std::map<juce::String, const InstanceRegistry::Entry *> leads;
  for (const auto &entry : entries)
    if (entry.role == InstanceRegistry::Lead && entry.groupId.isNotEmpty())
      leads[entry.groupId] = &entry;

  // The notes each track has now (published as soon as they are edited,
  // before rendering), else its last rendered snapshot.
  auto notesOf = [](const InstanceRegistry::Entry &entry) -> const Project * {
    return entry.notes != nullptr ? entry.notes.get() : entry.snapshot.get();
  };

  for (const auto &entry : entries)
  {
    const InstanceRegistry::Entry *lead = nullptr;
    if (entry.role == InstanceRegistry::Double)
      if (auto it = leads.find(entry.groupId); it != leads.end())
        lead = it->second;
    const Project *leadNotes = lead != nullptr ? notesOf(*lead) : nullptr;
    const bool on = lead != nullptr && lead->unisonOn && leadNotes != nullptr;
    const bool timingOn = lead != nullptr && lead->timingOn && leadNotes != nullptr;

    // Nothing to do for tracks that neither follow a lead nor still carry
    // notes that did.
    if (!on && !timingOn && !hasUnisonNotes(notesOf(entry)) && !hasAlignedNotes(notesOf(entry)))
    {
      applied.erase(entry.id);
      continue;
    }

    const juce::String signature =
        (on || timingOn)
            ? juce::String(static_cast<juce::int64>(lead->id)) + ":" +
                  juce::String(static_cast<juce::int64>(lead->notesRevision)) + ":" +
                  (on ? juce::String(lead->unisonStrength, 4) : juce::String("-")) + ":" +
                  (timingOn ? juce::String(lead->timingStrength, 4) : juce::String("-")) + ":" +
                  juce::String(static_cast<juce::int64>(entry.revision))
            : "off:" + juce::String(static_cast<juce::int64>(entry.revision));
    if (applied[entry.id] == signature)
      continue;
    applied[entry.id] = signature;

    auto *owner = static_cast<PitchNetAudioProcessor *>(entry.owner);
    if (owner == nullptr)
      continue;
    const float strength = on ? lead->unisonStrength : 0.0f;
    const float timingStrength = timingOn ? lead->timingStrength : 0.0f;
    // One double per pass: the rest follow on the next timer tick, so a
    // group of doubles never freezes the window in one go.
    passRunning = true;
    startTimer(1);
    PerfTrace::Scope scope("unison apply #" + juce::String(static_cast<juce::int64>(entry.id)));
    if (auto *view = owner->getMainComponent())
    {
      // Its window is open: edit that window's project; it redraws and renders.
      if (auto *project = view->getProject())
        if (applyDoubles(*project, (on || timingOn) ? leadNotes : nullptr, on, strength, timingOn,
                         timingStrength))
          view->notifyProjectEditedExternally(true);
    }
    else if (const auto *headless = owner->getHeadlessProject())
    {
      // Window closed: edit the project it plays in place (its render
      // backend, kept current, so nothing is copied) and render it when a
      // note moved. One pass: aligning a whole song costs tens of ms.
      juce::ignoreUnused(headless);
      std::shared_ptr<const Project> leadHold =
          (on || timingOn) ? (lead->notes != nullptr ? lead->notes : lead->snapshot) : nullptr;
      owner->editAndRenderHeadless([leadHold, on, strength, timingOn, timingStrength](Project &project) {
        return applyDoubles(project, leadHold.get(), on, strength, timingOn, timingStrength);
      });
    }
    return;
  }
  passRunning = false;
  stopTimer(); // every double is up to date
}
