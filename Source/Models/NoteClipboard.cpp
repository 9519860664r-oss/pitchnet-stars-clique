#include "NoteClipboard.h"
#include "../Utils/Constants.h"

#include <algorithm>

namespace NoteClipboard
{
namespace
{
template <typename T>
std::vector<T> slice(const std::vector<T>& source, int start, int length)
{
    std::vector<T> result(static_cast<size_t>(std::max(0, length)), T{});
    for (int i = 0; i < length; ++i)
    {
        const int from = start + i;
        if (from >= 0 && from < static_cast<int>(source.size()))
            result[static_cast<size_t>(i)] = source[static_cast<size_t>(from)];
    }
    return result;
}

template <typename T>
void write(std::vector<T>& dest, const std::vector<T>& source, int start, int length)
{
    for (int i = 0; i < length && i < static_cast<int>(source.size()); ++i)
    {
        const int to = start + i;
        if (to >= 0 && to < static_cast<int>(dest.size()))
            dest[static_cast<size_t>(to)] = source[static_cast<size_t>(i)];
    }
}

juce::AudioBuffer<float> sliceAudio(const juce::AudioBuffer<float>& source,
                                    int startSample, int numSamples)
{
    juce::AudioBuffer<float> result(std::max(1, source.getNumChannels()),
                                    std::max(0, numSamples));
    result.clear();
    for (int ch = 0; ch < source.getNumChannels(); ++ch)
    {
        const int from = std::max(0, startSample);
        const int to = std::min(source.getNumSamples(), startSample + numSamples);
        if (to > from)
            result.copyFrom(ch, from - startSample, source, ch, from, to - from);
    }
    return result;
}

// Write `source` over dest at startSample; a short crossfade at both ends
// keeps the seams free of clicks.
void writeAudio(juce::AudioBuffer<float>& dest, const juce::AudioBuffer<float>& source,
                int startSample, bool crossfade)
{
    constexpr int fade = 64;
    const int n = source.getNumSamples();
    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
    {
        const int srcCh = std::min(ch, source.getNumChannels() - 1);
        if (srcCh < 0)
            continue;
        auto* d = dest.getWritePointer(ch);
        const auto* s = source.getReadPointer(srcCh);
        for (int i = 0; i < n; ++i)
        {
            const int to = startSample + i;
            if (to < 0 || to >= dest.getNumSamples())
                continue;
            float w = 1.0f;
            if (crossfade)
            {
                if (i < fade) w = static_cast<float>(i) / fade;
                else if (i >= n - fade) w = static_cast<float>(n - 1 - i) / fade;
            }
            d[to] = w * s[i] + (1.0f - w) * d[to];
        }
    }
}

void shiftNote(Note& note, int delta)
{
    note.setStartFrame(note.getStartFrame() + delta);
    note.setEndFrame(note.getEndFrame() + delta);
    note.setSrcStartFrame(note.getSrcStartFrame() + delta);
    note.setSrcEndFrame(note.getSrcEndFrame() + delta);
    note.setRenderedFrames(note.getRenderedStartFrame() + delta,
                           note.getRenderedEndFrame() + delta);
}

int totalFrames(const Project& project)
{
    return static_cast<int>(project.getAudioData().f0.size());
}
} // namespace

std::shared_ptr<Clip> copySelection(const Project& project, const juce::String& sourceKey)
{
    int first = std::numeric_limits<int>::max(), last = -1;
    for (const auto& note : project.getNotes())
        if (!note.isRest() && note.isSelected())
        {
            first = std::min(first, note.getStartFrame());
            last = std::max(last, note.getEndFrame());
        }
    if (last <= first)
        return nullptr;

    const auto& audio = project.getAudioData();
    auto clip = std::make_shared<Clip>();
    clip->sourceKey = sourceKey;
    clip->lengthFrames = last - first;
    clip->firstNoteSeconds = framesToSeconds(first);
    for (const auto& note : project.getNotes())
        if (!note.isRest() && note.getStartFrame() >= first && note.getEndFrame() <= last)
        {
            Note copy = note;
            shiftNote(copy, -first);
            copy.setSelected(false);
            clip->notes.push_back(copy);
        }
    const int L = clip->lengthFrames;
    clip->rawF0 = slice(audio.rawF0, first, L);
    clip->cleanedF0 = slice(audio.cleanedF0, first, L);
    clip->denseF0 = slice(audio.denseF0, first, L);
    clip->f0 = slice(audio.f0, first, L);
    clip->baseF0 = slice(audio.baseF0, first, L);
    clip->basePitch = slice(audio.basePitch, first, L);
    clip->deltaPitch = slice(audio.deltaPitch, first, L);
    clip->voicedMask = slice(audio.voicedMask, first, L);
    clip->vadMask = slice(audio.vadMask, first, L);
    clip->mel = slice(audio.melSpectrogram, first, L);
    clip->original = sliceAudio(audio.originalWaveform, first * HOP_SIZE, L * HOP_SIZE);
    clip->rendered = sliceAudio(audio.waveform, first * HOP_SIZE, L * HOP_SIZE);
    return clip;
}

Slice paste(Project& project, const Clip& clip, int destStartFrame)
{
    Slice before;
    const int D = std::max(0, destStartFrame);
    const int L = std::min(clip.lengthFrames, totalFrames(project) - D);
    if (L <= 0)
        return before;

    auto& audio = project.getAudioData();
    before.startFrame = D;
    before.lengthFrames = L;
    before.notes = project.getNotes();
    before.rawF0 = slice(audio.rawF0, D, L);
    before.cleanedF0 = slice(audio.cleanedF0, D, L);
    before.denseF0 = slice(audio.denseF0, D, L);
    before.f0 = slice(audio.f0, D, L);
    before.baseF0 = slice(audio.baseF0, D, L);
    before.basePitch = slice(audio.basePitch, D, L);
    before.deltaPitch = slice(audio.deltaPitch, D, L);
    before.voicedMask = slice(audio.voicedMask, D, L);
    before.vadMask = slice(audio.vadMask, D, L);
    before.mel = slice(audio.melSpectrogram, D, L);
    before.original = sliceAudio(audio.originalWaveform, D * HOP_SIZE, L * HOP_SIZE);
    before.rendered = sliceAudio(audio.waveform, D * HOP_SIZE, L * HOP_SIZE);

    write(audio.rawF0, clip.rawF0, D, L);
    write(audio.cleanedF0, clip.cleanedF0, D, L);
    write(audio.denseF0, clip.denseF0, D, L);
    write(audio.f0, clip.f0, D, L);
    write(audio.baseF0, clip.baseF0, D, L);
    write(audio.basePitch, clip.basePitch, D, L);
    write(audio.deltaPitch, clip.deltaPitch, D, L);
    write(audio.voicedMask, clip.voicedMask, D, L);
    write(audio.vadMask, clip.vadMask, D, L);
    write(audio.melSpectrogram, clip.mel, D, L);
    const auto cut = [&](const juce::AudioBuffer<float>& source) {
        return sliceAudio(source, 0, L * HOP_SIZE);
    };
    writeAudio(audio.originalWaveform, cut(clip.original), D * HOP_SIZE, true);
    writeAudio(audio.waveform, cut(clip.rendered), D * HOP_SIZE, true);

    // Notes already there give way: trimmed at the paste's edges, removed
    // when inside it.
    std::vector<Note> notes;
    for (auto note : project.getNotes())
    {
        const int s = note.getStartFrame(), e = note.getEndFrame();
        if (e <= D || s >= D + L)
        {
            notes.push_back(note);
            continue;
        }
        if (s < D)
        {
            note.setEndFrame(D);
            note.markDirty();
            note.markSynthDirty();
            notes.push_back(note);
        }
        else if (e > D + L)
        {
            note.setStartFrame(D + L);
            note.markDirty();
            note.markSynthDirty();
            notes.push_back(note);
        }
    }
    for (auto note : clip.notes)
    {
        if (note.getStartFrame() >= L)
            continue;
        if (note.getEndFrame() > L)
            note.setEndFrame(L);
        shiftNote(note, D);
        note.setSelected(true);
        note.markDirty();
        note.markSynthDirty();
        notes.push_back(note);
    }
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return a.getStartFrame() < b.getStartFrame();
    });
    for (auto& note : project.getNotes())
        note.setSelected(false);
    project.getNotes() = std::move(notes);
    return before;
}

void swapSlice(Project& project, Slice& slice)
{
    if (slice.lengthFrames <= 0)
        return;
    auto& audio = project.getAudioData();
    const int D = slice.startFrame, L = slice.lengthFrames;
    const auto swapFloats = [&](std::vector<float>& live, std::vector<float>& saved) {
        auto current = NoteClipboard::slice(live, D, L);
        write(live, saved, D, L);
        saved = std::move(current);
    };
    swapFloats(audio.rawF0, slice.rawF0);
    swapFloats(audio.cleanedF0, slice.cleanedF0);
    swapFloats(audio.denseF0, slice.denseF0);
    swapFloats(audio.f0, slice.f0);
    swapFloats(audio.baseF0, slice.baseF0);
    swapFloats(audio.basePitch, slice.basePitch);
    swapFloats(audio.deltaPitch, slice.deltaPitch);
    {
        auto current = NoteClipboard::slice(audio.voicedMask, D, L);
        write(audio.voicedMask, slice.voicedMask, D, L);
        slice.voicedMask = std::move(current);
    }
    {
        auto current = NoteClipboard::slice(audio.vadMask, D, L);
        write(audio.vadMask, slice.vadMask, D, L);
        slice.vadMask = std::move(current);
    }
    {
        auto current = NoteClipboard::slice(audio.melSpectrogram, D, L);
        write(audio.melSpectrogram, slice.mel, D, L);
        slice.mel = std::move(current);
    }
    {
        auto current = sliceAudio(audio.originalWaveform, D * HOP_SIZE, L * HOP_SIZE);
        writeAudio(audio.originalWaveform, slice.original, D * HOP_SIZE, false);
        slice.original = std::move(current);
    }
    {
        auto current = sliceAudio(audio.waveform, D * HOP_SIZE, L * HOP_SIZE);
        writeAudio(audio.waveform, slice.rendered, D * HOP_SIZE, false);
        slice.rendered = std::move(current);
    }
    std::swap(project.getNotes(), slice.notes);
    for (auto& note : project.getNotes())
        if (note.getEndFrame() > D && note.getStartFrame() < D + L)
        {
            note.markDirty();
            note.markSynthDirty();
        }
}

std::vector<std::shared_ptr<Clip>>& contents()
{
    static std::vector<std::shared_ptr<Clip>> clips;
    return clips;
}
} // namespace NoteClipboard
