// Quantize Time, align-to-reference and leveling arithmetic.

#include "../Source/Utils/NoteMacros.h"
#include "../Source/Utils/DoubleTuning.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

using namespace NoteMacros;

int main() {
  // Quantize to a 10-frame grid at full strength; connected notes follow.
  {
    std::vector<Span> notes{{12, 30}, {30, 47}, {55, 70}};
    const auto q = quantizeStarts(notes, 10.0, 0.0, 1.0);
    CHECK(q[0].start == 10);
    CHECK(q[1].start == 30);              // already on the grid
    CHECK(q[0].end == 30);                // connected, unchanged start
    CHECK(q[2].start == 60 && q[2].end == 70);
    // Half strength goes half way.
    const auto h = quantizeStarts(notes, 10.0, 0.0, 0.5);
    CHECK(h[0].start == 11);
    CHECK(h[2].start == 58); // 55 + (60 - 55) * 0.5 = 57.5 -> 58
    // A connected previous note's end follows a moved start.
    std::vector<Span> pair{{0, 23}, {23, 40}};
    const auto c = quantizeStarts(pair, 10.0, 0.0, 1.0);
    CHECK(c[1].start == 20 && c[0].end == 20);
    // Only movable notes move; a fixed rest before one stretches to meet it.
    std::vector<Span> mixed{{0, 23}, {23, 40}, {40, 52}};
    const auto m = quantizeStarts(mixed, 10.0, 0.0, 1.0, {}, 0, {false, true, false});
    CHECK(m[0].start == 0 && m[0].end == 20 && m[1].start == 20);
    CHECK(m[2].start == 40);
    // Never past the note's own end.
    std::vector<Span> tiny{{18, 19}};
    CHECK(quantizeStarts(tiny, 10.0, 0.0, 1.0)[0].start == 18);
  }

  // Quantize to another track's note starts, within reach only.
  {
    std::vector<Span> notes{{12, 30}, {80, 90}};
    const auto q = quantizeStarts(notes, 0.0, 0.0, 1.0, {10, 50}, 8);
    CHECK(q[0].start == 10);
    CHECK(q[1].start == 80); // nearest is 30 frames away: out of reach
  }

  // Align a double to the lead: starts, ends, unison and an interval.
  {
    std::vector<Span> notes{{12, 28}, {33, 52}};
    std::vector<float> midi{60.3f, 62.0f};
    std::vector<RefNote> lead{{10, 30, 60.0f}, {30, 50, 64.0f}};
    AlignOptions o;
    o.pitchMode = 1;
    const auto a = alignToReference(notes, midi, lead, o);
    CHECK(a[0].reference == 0 && a[1].reference == 1);
    CHECK(a[0].span.start == 10 && a[0].span.end == 30);
    CHECK(a[1].span.start == 30 && a[1].span.end == 50);
    CHECK(std::abs(a[0].midi - 60.0f) < 1e-4f && std::abs(a[1].midi - 64.0f) < 1e-4f);

    o.pitchMode = 2;
    o.intervalSemitones = -3.0;
    o.alignEnds = false;
    const auto b = alignToReference(notes, midi, lead, o);
    CHECK(std::abs(b[1].midi - 61.0f) < 1e-4f);
    CHECK(b[0].span.end == 28); // ends left alone

    o.pitchMode = 0;
    o.timingIntensity = 0.5;
    const auto half = alignToReference(notes, midi, lead, o);
    CHECK(half[0].span.start == 11);
    CHECK(std::abs(half[0].midi - 60.3f) < 1e-4f);

    // Far away notes stay unmatched and untouched.
    std::vector<Span> far{{300, 320}};
    const auto f = alignToReference(far, {60.0f}, lead, AlignOptions{});
    CHECK(f[0].reference == -1 && f[0].span.start == 300);
  }

  // Leveling toward the mean; short quiet notes are left alone.
  {
    std::vector<float> levels{-20.0f, -10.0f, -30.0f};
    std::vector<int> lengths{10, 10, 2};
    const auto g = levelingGains(levels, lengths, 1.0, 1.0);
    // Mean of -20,-10,-30 weighted 10,10,2 = (-200-100-60)/22 = -16.36
    CHECK(std::abs(g[0] - 3.636f) < 0.01f);  // raised to the mean
    CHECK(std::abs(g[1] + 6.364f) < 0.01f);  // lowered to the mean
    CHECK(g[2] == 0.0f);                       // short and quiet
    const auto half = levelingGains(levels, lengths, 0.5, 0.0);
    CHECK(std::abs(half[0] - 1.818f) < 0.01f && half[1] == 0.0f);
    CHECK(levelingGains({-12.0f}, {10}, 1.0, 1.0)[0] == 0.0f);
  }

  // Correct Pitch popup value: what the notes keep, or 0.
  {
    CHECK(NoteMacros::rememberedMacroValue({}) == 0.0f);
    CHECK(NoteMacros::rememberedMacroValue({-1.0f, -1.0f}) == 0.0f);     // never corrected
    CHECK(NoteMacros::rememberedMacroValue({70.0f, 70.0f}) == 70.0f);    // applied 70
    CHECK(NoteMacros::rememberedMacroValue({100.0f, 50.0f}) == 0.0f);    // different
    CHECK(NoteMacros::rememberedMacroValue({100.0f, -1.0f}) == 0.0f);    // partly corrected
  }

  // Doubles: matching, unison, detection.
  {
    using namespace DoubleTuning;
    const std::vector<TNote> lead{{0, 100, 60.0f}, {100, 200, 62.0f}, {220, 300, 64.0f},
                                  {400, 500, 67.0f}};
    // The double sings with the lead in places, misses by up to 4 semitones,
    // has an ad-lib of its own (at 600) and one note an octave up.
    const std::vector<TNote> dbl{{5, 95, 60.4f}, {105, 190, 65.5f}, {225, 295, 76.2f},
                                 {600, 700, 70.0f}};
    // Unison: every double note sounding with a lead note takes that note,
    // whatever it sang (no interval, no octave kept); the ad-lib is left.
    const auto m = matchNotes(dbl, lead);
    CHECK(m[0].lead == 0 && m[0].octaveShift == 0.0f);
    CHECK(m[1].lead == 1);
    CHECK(m[2].lead == 2 && m[2].octaveShift == 0.0f);
    CHECK(m[3].lead == -1);
    // Finding doubles still compares pitch: octave double kept as such.
    const auto similar = matchSimilarNotes(dbl, lead);
    CHECK(similar[1].lead == 1);                               // 3.5 semitones off
    CHECK(similar[2].lead == 2 && similar[2].octaveShift == 12.0f);
    // Unison: 0 % stays, 100 % note for note, 70 % most of the way.
    CHECK(std::abs(unisonMidi(60.4f, 60.0f, 0.0f, 0.0f) - 60.4f) < 1e-4f);
    CHECK(std::abs(unisonMidi(60.4f, 60.0f, 0.0f, 1.0f) - 60.0f) < 1e-4f);
    CHECK(std::abs(unisonMidi(65.5f, 62.0f, 0.0f, 0.7f) - 63.05f) < 1e-3f);
    CHECK(std::abs(unisonMidi(76.2f, 64.0f, 12.0f, 1.0f) - 76.0f) < 1e-4f);
    // Contour: 0 % keeps the double's own slide, 100 % copies the lead's
    // vibrato, 50 % halfway; frames the lead has no curve for stay as sung.
    {
      const std::vector<float> own{0.0f, 0.2f, 0.4f, 0.6f};
      const std::vector<float> leadC{0.1f, -0.1f, 0.1f, std::nanf("")};
      const auto none = unisonContour(own, leadC, 0.0f);
      const auto full = unisonContour(own, leadC, 1.0f);
      const auto half = unisonContour(own, leadC, 0.5f);
      CHECK(std::abs(none[1] - 0.2f) < 1e-5f);
      CHECK(std::abs(full[0] - 0.1f) < 1e-5f && std::abs(full[1] + 0.1f) < 1e-5f);
      CHECK(std::abs(half[2] - 0.25f) < 1e-5f);
      CHECK(std::abs(full[3] - 0.6f) < 1e-5f);
    }
    // Unison: a fifth away still goes to the lead's note.
    CHECK(matchNotes({{0, 100, 67.0f}}, {{0, 100, 60.0f}})[0].lead == 0);
    // ...but finding doubles does not count it as a double.
    CHECK(matchSimilarNotes({{0, 100, 67.0f}}, {{0, 100, 60.0f}})[0].lead == -1);
    // Sounding with the lead for under a quarter of its length: left alone.
    CHECK(matchNotes({{90, 190, 60.0f}}, {{0, 100, 60.0f}})[0].lead == -1);
    // A double note that spans two lead notes follows the longer overlap.
    CHECK(matchNotes({{50, 180, 61.0f}}, {{0, 100, 60.0f}, {100, 200, 62.0f}})[0].lead == 1);

    // Detection: lead, a double of it, and an unrelated harmony track.
    const std::vector<TNote> harmony{{0, 100, 67.0f}, {100, 200, 69.0f}, {220, 300, 71.0f}};
    const auto groups = detectGroups({dbl, lead, harmony});
    CHECK(groups.size() == 1);
    CHECK(groups[0].lead == 1);                       // most sung time
    CHECK(groups[0].doubles.size() == 1 && groups[0].doubles[0] == 0);
    CHECK(doubleScore(harmony, lead) < kDetectThreshold);
  }

  std::printf("NoteMacrosTests passed\n");
  return 0;
}
