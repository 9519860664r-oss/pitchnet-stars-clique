// Sibilants, a note's time map, and timing alignment of doubles.

#include "../Source/Utils/DoubleTuning.h"
#include "../Source/Utils/LeadProjection.h"
#include "../Source/Utils/NoteTimeMap.h"
#include "../Source/Utils/SibilantDetector.h"
#include "../Source/Utils/TimingAligner.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

namespace
{
AudioData voicing(int frames, std::initializer_list<std::pair<int, int>> voicedRuns)
{
    AudioData audio;
    audio.voicedMask.assign(static_cast<size_t>(frames), false);
    for (const auto &[a, b] : voicedRuns)
        for (int f = a; f < b; ++f)
            audio.voicedMask[static_cast<size_t>(f)] = true;
    return audio;
}

Note noteAt(int srcStart, int srcEnd, int start, int end)
{
    Note note;
    note.setSrcStartFrame(srcStart);
    note.setSrcEndFrame(srcEnd);
    note.setStartFrame(start);
    note.setEndFrame(end);
    return note;
}

// A fingerprint-friendly fake mel: smooth spectral movement plus a clear
// onset every 17 frames, so every moment looks a little different.
std::vector<std::vector<float>> fakeMel(int frames, int delay)
{
    std::vector<std::vector<float>> mel(static_cast<size_t>(frames), std::vector<float>(32, 0.0f));
    for (int t = 0; t < frames; ++t)
    {
        const int u = t - delay;
        for (int b = 0; b < 32; ++b)
        {
            float v = std::sin(u * 0.21f + b * 0.37f) + 0.6f * std::sin(u * 0.047f * (1 + b % 5));
            if (((u % 17) + 17) % 17 < 2)
                v += 3.0f;
            mel[static_cast<size_t>(t)][static_cast<size_t>(b)] = v;
        }
    }
    return mel;
}
} // namespace

int main()
{
    // --- Sibilant detection: unpitched frames at the ends of a note.
    {
        const auto audio = voicing(60, {{15, 36}});
        const auto r = SibilantDetector::detect(audio, 10, 40);
        CHECK(r.head == 5 && r.tail == 4);
    }
    {
        // A lone voiced frame inside an "s" does not end it.
        const auto audio = voicing(60, {{12, 13}, {15, 36}});
        CHECK(SibilantDetector::detect(audio, 10, 40).head == 5);
    }
    {
        // One unvoiced frame is not a sibilant; a breath is all sibilant.
        const auto audio = voicing(60, {{11, 40}});
        CHECK(SibilantDetector::detect(audio, 10, 40).head == 0);
        const auto breath = voicing(60, {});
        CHECK(SibilantDetector::detect(breath, 10, 40).head == 30);
    }
    {
        Note note = noteAt(10, 40, 10, 40);
        note.setSibilants(5, 4, false);
        CHECK(note.hasSibilantInfo() && note.getSibilantHead() == 5 && note.getSibilantTail() == 4);
        note.setSrcStartFrame(12); // cut from other frames: found again later
        CHECK(!note.hasSibilantInfo());
        note.setSibilants(3, 0, true);
        note.setSrcEndFrame(38); // set by hand: kept
        CHECK(note.getSibilantHead() == 3 && note.getSibilantsSetByHand());
    }

    // --- Time map: sibilants keep their length when a note stretches.
    {
        Note note = noteAt(100, 140, 200, 260);
        note.setSibilants(5, 4, false);
        const auto k = NoteTimeMap::knots(note);
        CHECK(k.size() == 4);
        CHECK(std::abs(k[1].src - 105) < 1e-9 && std::abs(k[1].dst - 205) < 1e-9);
        CHECK(std::abs(k[2].src - 136) < 1e-9 && std::abs(k[2].dst - 256) < 1e-9);
        // The "s": one source frame per output frame, just moved.
        CHECK(std::abs(NoteTimeMap::sourceFrameFor(k, 202) - 102.0) < 1e-9);
        CHECK(std::abs(NoteTimeMap::sourceFrameFor(k, 258) - 138.0) < 1e-9);
        const auto b = NoteTimeMap::sibilantBounds(note);
        CHECK(std::abs(b.headEnd - 205) < 1e-9 && std::abs(b.tailStart - 256) < 1e-9);
    }
    {
        // No sibilants, no anchors: the even stretch, exactly as before.
        Note note = noteAt(100, 140, 200, 260);
        const auto k = NoteTimeMap::knots(note);
        CHECK(k.size() == 2);
        for (int f = 200; f < 260; ++f)
        {
            const double old = 100.0 + (f - 200 + 0.5) / 60.0 * 40.0 - 0.5;
            CHECK(std::abs(NoteTimeMap::sourceFrameFor(k, f) - old) < 1e-9);
        }
    }
    {
        // Squeezed too far: the whole note squeezes, sibilants too.
        Note note = noteAt(100, 140, 200, 211);
        note.setSibilants(5, 4, false);
        CHECK(NoteTimeMap::knots(note).size() == 2);
    }
    {
        // A warp anchor pins a moment inside the note.
        Note note = noteAt(100, 140, 100, 140);
        note.setWarpAnchors({{0.5f, 0.25f}}); // source middle plays a quarter in
        const auto k = NoteTimeMap::knots(note);
        CHECK(k.size() == 3);
        CHECK(std::abs(NoteTimeMap::sourceAt(k, 110.0) - 120.0) < 1e-9);
        // A curve recorded over the source frames follows the map.
        std::vector<float> curve(40);
        for (int i = 0; i < 40; ++i)
            curve[static_cast<size_t>(i)] = static_cast<float>(i);
        const auto out = NoteTimeMap::toOutput(curve, note, 40);
        CHECK(std::abs(out[10] - (NoteTimeMap::sourceFrameFor(k, 110) - 100.0)) < 1e-4);
        CHECK(out[10] > 15.0f); // ahead of an even stretch
    }

    // --- Alignment: a double sung 6 frames late is moved back.
    {
        const auto guideMel = fakeMel(300, 0);
        const auto dubMel = fakeMel(300, 6);
        const std::vector<bool> voiced(300, true);
        const auto guide = TimingAligner::fromMel(guideMel, voiced);
        const auto dub = TimingAligner::fromMel(dubMel, voiced);
        TimingAligner::Options options;
        options.toleranceFrames = 0;
        auto off = TimingAligner::offsets(dub, guide, {{40, 260}}, options);
        double mean = 0.0;
        for (int f = 80; f < 220; ++f)
            mean += off[static_cast<size_t>(f)];
        mean /= 140.0;
        std::printf("full strength: %.2f frames\n", mean);
        CHECK(std::abs(mean + 6.0) < 1.0);

        options.strength = 0.5;
        off = TimingAligner::offsets(dub, guide, {{40, 260}}, options);
        mean = 0.0;
        for (int f = 80; f < 220; ++f)
            mean += off[static_cast<size_t>(f)];
        mean /= 140.0;
        std::printf("half strength: %.2f frames\n", mean);
        CHECK(std::abs(mean + 3.0) < 1.0);

        // Beyond twice the tolerance a difference is taken whole.
        options.strength = 1.0;
        options.toleranceFrames = 2;
        off = TimingAligner::offsets(dub, guide, {{40, 260}}, options);
        mean = 0.0;
        for (int f = 80; f < 220; ++f)
            mean += off[static_cast<size_t>(f)];
        mean /= 140.0;
        std::printf("with tolerance: %.2f frames\n", mean);
        CHECK(std::abs(mean + 6.0) < 1.0);

        // Within the tolerance nothing moves.
        options.toleranceFrames = 8;
        off = TimingAligner::offsets(dub, guide, {{40, 260}}, options);
        for (int f = 80; f < 220; ++f)
            CHECK(std::abs(off[static_cast<size_t>(f)]) < 0.5);
        // Outside the phrases nothing moves either.
        CHECK(off[10] == 0.0 && off[290] == 0.0);
    }
    {
        // SmartAlign: notes join into phrases across short gaps only.
        const auto phrases = TimingAligner::phrasesFrom({{0, 10}, {12, 20}, {80, 90}});
        CHECK(phrases.size() == 2 && phrases[0].second == 20 && phrases[1].first == 80);
    }

    // --- ARA: the lead laid out in a double clip's frames by the timeline.
    {
        Project lead;
        auto &la = lead.getAudioData();
        la.sampleRate = 44100;
        la.basePitch.assign(300, 0.0f);
        la.deltaPitch.assign(300, 0.0f);
        for (int f = 100; f < 150; ++f)
            la.basePitch[static_cast<size_t>(f)] = 62.0f;
        Note n = noteAt(100, 150, 100, 150);
        n.setMidiNote(62.0f);
        n.setSibilants(3, 2, false);
        lead.addNote(n);
        Project dub;
        dub.getAudioData().sampleRate = 44100;
        dub.getAudioData().basePitch.assign(400, 0.0f);
        auto out = LeadProjection::makeEmpty(dub);
        CHECK(out.getAudioData().basePitch.size() == 400);
        // Lead clip: its audio from 0 s plays at 1.0 s; the double's from 0 s at 0.5 s.
        LeadProjection::addClip(out, lead, {}, {1.0, 1.0, 4.0}, {0.5, 0.5, 4.0});
        const int shift = static_cast<int>(std::lround(0.5 / (512.0 / 44100.0)));
        CHECK(out.getNotes().size() == 1);
        CHECK(out.getNotes()[0].getStartFrame() == 100 + shift);
        CHECK(out.getNotes()[0].getSrcEndFrame() == 150 + shift);
        CHECK(out.getNotes()[0].getSibilantHead() == 3);
        CHECK(out.getAudioData().basePitch[static_cast<size_t>(120 + shift)] == 62.0f);
        CHECK(out.getAudioData().basePitch[static_cast<size_t>(90 + shift)] == 0.0f);
        // Only what the lead clip plays: a clip ending at 1.5 s leaves the note out.
        auto cut = LeadProjection::makeEmpty(dub);
        LeadProjection::addClip(cut, lead, {}, {1.0, 1.0, 1.5}, {0.5, 0.5, 4.0});
        CHECK(cut.getNotes().empty());
    }

    // --- Natural unison: the lead's note centres, the double's own life.
    {
        const int n = 200;
        const float pi = 3.14159265f;
        std::vector<float> own(n), lead(n);
        std::vector<int> noteOf(n, -1);
        for (int t = 0; t < n; ++t)
        {
            // Two notes; the double sings 40 cents flat on the first and 70
            // cents sharp on the second, with its own vibrato; the lead has a
            // different vibrato.
            const bool second = t >= 100;
            noteOf[static_cast<size_t>(t)] = second ? 1 : 0;
            const float ownVib = 0.3f * std::sin(2.0f * pi * t / 16.0f);
            const float leadVib = 0.25f * std::sin(2.0f * pi * t / 13.0f + 1.0f);
            own[static_cast<size_t>(t)] = (second ? 62.7f : 59.6f) + ownVib;
            lead[static_cast<size_t>(t)] = (second ? 62.0f : 60.0f) + leadVib;
        }
        // Slow parts (notes and drift), as the coordinator passes them.
        const auto offset = DoubleTuning::unisonOffsets(DoubleTuning::slowWithinNotes(own, noteOf),
                                                        DoubleTuning::slowWithinNotes(lead, noteOf), noteOf);
        // Centres: within the tolerance (6 cents) of the lead.
        double c0 = 0.0, c1 = 0.0;
        for (int t = 20; t < 80; ++t)
            c0 += own[static_cast<size_t>(t)] + offset[static_cast<size_t>(t)] - 60.0f;
        for (int t = 120; t < 180; ++t)
            c1 += own[static_cast<size_t>(t)] + offset[static_cast<size_t>(t)] - 62.0f;
        c0 /= 60.0;
        c1 /= 60.0;
        std::printf("unison centres off by %.3f / %.3f st\n", c0, c1);
        CHECK(std::abs(c0) < 0.08 && std::abs(c1) < 0.08);
        // The double's own vibrato stays; the lead's is not copied.
        double ownCorr = 0.0, leadCorr = 0.0, energy = 0.0;
        for (int t = 20; t < 80; ++t)
        {
            const double v = own[static_cast<size_t>(t)] + offset[static_cast<size_t>(t)];
            const double ownVib = 0.3 * std::sin(2.0 * pi * t / 16.0);
            const double leadVib = 0.25 * std::sin(2.0 * pi * t / 13.0 + 1.0);
            ownCorr += (v - 60.0) * ownVib;
            leadCorr += (v - 60.0) * leadVib;
            energy += ownVib * ownVib;
        }
        std::printf("vibrato kept %.2f, lead's copied %.2f\n", ownCorr / energy, leadCorr / energy);
        // Vibrato is faster than the slow part, but not by much: most of
        // the double's stays, little of the lead's comes in.
        CHECK(ownCorr / energy > 0.55);
        CHECK(std::abs(leadCorr / energy) < 0.25);
        // No jumps: the offset moves at most a little per frame.
        float worst = 0.0f;
        for (int t = 1; t < n; ++t)
            worst = std::max(worst, std::abs(offset[static_cast<size_t>(t)] - offset[static_cast<size_t>(t - 1)]));
        std::printf("largest offset step %.3f st/frame\n", worst);
        CHECK(worst < 0.3f);
    }
    {
        // An octave double stays an octave down; silence stays untouched.
        const int n = 60;
        std::vector<float> own(n, 47.8f), lead(n, 60.0f);
        std::vector<int> noteOf(n, 0);
        for (int t = 50; t < n; ++t)
            noteOf[static_cast<size_t>(t)] = -1;
        const auto offset = DoubleTuning::unisonOffsets(own, lead, noteOf);
        CHECK(std::abs(offset[25] - 0.2f) < 0.08f);
        CHECK(offset[55] == 0.0f);
    }
    {
        // A note sung without the lead (a consonant before it) moves with
        // the note it leads into; unpitched frames get the note's offset.
        const int n = 80;
        const float nan = std::nanf("");
        std::vector<float> own(n, 60.5f), lead(n, nan);
        std::vector<int> noteOf(n, -1);
        for (int t = 10; t < 16; ++t)
            noteOf[static_cast<size_t>(t)] = 0;
        for (int t = 16; t < 70; ++t)
        {
            noteOf[static_cast<size_t>(t)] = 1;
            lead[static_cast<size_t>(t)] = 60.0f;
        }
        own[30] = own[31] = nan; // unpitched inside the note
        const auto offset = DoubleTuning::unisonOffsets(own, lead, noteOf);
        CHECK(offset[12] < -0.3f);
        CHECK(std::abs(offset[30] - offset[40]) < 0.02f);
    }

    std::printf("DoubleTimingTests passed\n");
    return 0;
}
