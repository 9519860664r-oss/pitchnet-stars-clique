#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/NoteTimeMap.h"

#include <climits>
#include <functional>

/** A note's sibilant settings (Sibilant Range and Balance). */
struct SibilantState
{
    int head = -1;
    int tail = -1;
    bool byHand = false;
    float balance = 0.0f;

    static SibilantState of(const Note &note)
    {
        return {note.getSibilantHead(), note.getSibilantTail(), note.getSibilantsSetByHand(),
                note.getSibilantBalance()};
    }
    void applyTo(Note &note) const
    {
        if (head < 0)
            note.clearSibilantInfo();
        else
            note.setSibilants(head, tail, byHand);
        note.setSibilantBalance(balance);
    }
};

/**
 * Sibilant ranges set by hand, found again or removed. Which frames keep
 * their original sound changes, so the notes render again.
 */
class SibilantRangeAction : public UndoableAction
{
public:
    SibilantRangeAction(Project &project, std::vector<Note *> notes, std::vector<SibilantState> before,
                        std::vector<SibilantState> after, juce::String name)
        : project(project), notes(std::move(notes)), before(std::move(before)),
          after(std::move(after)), name(std::move(name)) {}

    void undo() override { apply(before); }
    void redo() override { apply(after); }
    juce::String getName() const override { return name; }

private:
    void apply(const std::vector<SibilantState> &states)
    {
        int first = INT_MAX, last = -1;
        for (size_t i = 0; i < notes.size() && i < states.size(); ++i)
            if (auto *note = notes[i])
            {
                states[i].applyTo(*note);
                note->markDirty();
                note->markSynthDirty();
                first = std::min(first, note->getStartFrame());
                last = std::max(last, note->getEndFrame());
            }
        if (last > first)
            project.markNoteEditDirtyRange(first, last);
        project.setModified(true);
    }

    Project &project;
    std::vector<Note *> notes;
    std::vector<SibilantState> before, after;
    juce::String name;
};

/**
 * Sibilant Balance: a gain on the note's sibilants or on its pitched part,
 * applied straight to the rendered audio (no new render), like a level edit.
 */
class SibilantBalanceAction : public UndoableAction
{
public:
    SibilantBalanceAction(Project &project, std::vector<Note *> notes, std::vector<float> before,
                          std::vector<float> after, std::function<void()> changed)
        : project(project), notes(std::move(notes)), before(std::move(before)),
          after(std::move(after)), changed(std::move(changed)) {}

    void undo() override { apply(after, before); }
    void redo() override { apply(before, after); }
    bool requiresAudioResynthesis() const override { return false; }
    juce::String getName() const override { return "Sibilant Balance"; }

private:
    void apply(const std::vector<float> &from, const std::vector<float> &to)
    {
        auto &audio = project.getAudioData();
        const int total = audio.waveform.getNumSamples();
        const int hop = HOP_SIZE;
        for (size_t n = 0; n < notes.size() && n < from.size() && n < to.size(); ++n)
        {
            auto *note = notes[n];
            if (note == nullptr)
                continue;
            const auto bounds = NoteTimeMap::sibilantBounds(*note);
            Note oldNote = *note, newNote = *note;
            oldNote.setSibilantBalance(from[n]);
            newNote.setSibilantBalance(to[n]);
            const int start = std::clamp(note->getStartFrame() * hop, 0, total);
            const int end = std::clamp(note->getEndFrame() * hop, start, total);
            for (int ch = 0; ch < audio.waveform.getNumChannels(); ++ch)
            {
                auto *samples = audio.waveform.getWritePointer(ch);
                for (int i = start; i < end; ++i)
                {
                    const double pos = static_cast<double>(i);
                    samples[i] *= NoteTimeMap::balanceGain(newNote, bounds, pos, hop) /
                                  NoteTimeMap::balanceGain(oldNote, bounds, pos, hop);
                }
            }
            note->setSibilantBalance(to[n]);
            note->setRenderedEdit(!note->isNeutralForOriginalWaveform());
        }
        project.setModified(true);
        if (changed)
            changed();
    }

    Project &project;
    std::vector<Note *> notes;
    std::vector<float> before, after;
    std::function<void()> changed;
};
