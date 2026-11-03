// Shared key, custom scales with cents, key changes over time.

#include "../Source/Models/Project.h"
#include "../Source/Models/ProjectSerializer.h"
#include "../Source/Utils/ScaleUtils.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

static bool near(float a, float b) { return std::abs(a - b) < 1.0e-3f; }

int main() {
  // Every project shares one set of key settings.
  {
    Project a, b;
    CHECK(a.getMacroParameters() == b.getMacroParameters());
    a.setProjectKey(2, ScaleMode::Minor);
    CHECK(b.getProjectKeyRoot() == 2 && b.getProjectKeyMode() == ScaleMode::Minor);
  }

  // Custom scale: C D E G A (absolute), E 14 cents flat.
  auto &shared = *sharedMacroParameters();
  shared.customScale.name = "Pentatonic just";
  shared.customScale.used = {{true, false, true, false, true, false, false, true, false, true, false, false}};
  shared.customScale.cents = {};
  shared.customScale.cents[4] = -14.0f;
  {
    CHECK(ScaleUtils::isPitchClassInScale(ScaleMode::Custom, 64, 0));  // E
    CHECK(!ScaleUtils::isPitchClassInScale(ScaleMode::Custom, 65, 0)); // F
    // Snap to the scale lands on the tuned E.
    CHECK(near(ScaleUtils::snapMidiToScale(64.2f, ScaleMode::Custom, 0, 440), 63.86f));
    // F (65) is not in the scale: nearest is the tuned E or G.
    CHECK(near(ScaleUtils::snapMidiToScale(65.4f, ScaleMode::Custom, 0, 440), 63.86f) ||
          near(ScaleUtils::snapMidiToScale(65.4f, ScaleMode::Custom, 0, 440), 67.0f));
    // Semitone snap uses the cents of every note.
    CHECK(near(ScaleUtils::snapMidiToTunedSemitone(63.9f, ScaleMode::Custom, 440), 63.86f));
    CHECK(near(ScaleUtils::snapMidiToTunedSemitone(63.9f, ScaleMode::Major, 440), 64.0f));
    // A4 = 432 shifts every target.
    const float offset = 12.0f * std::log2(432.0f / 440.0f);
    CHECK(near(ScaleUtils::snapMidiToSemitone(69.0f + offset + 0.2f, 432), 69.0f + offset));
  }

  // Key changes: the last one at or before a time applies.
  {
    Project p;
    p.setProjectKey(0, ScaleMode::Major);
    shared.keyMarkers = {{10.0, 9, ScaleMode::Minor}, {20.0, 7, ScaleMode::Major}};
    CHECK(ScaleUtils::keyAt(p, 5.0).root == 0);
    CHECK(ScaleUtils::keyAt(p, 10.0).root == 9 && ScaleUtils::keyAt(p, 10.0).mode == ScaleMode::Minor);
    CHECK(ScaleUtils::keyAt(p, 25.0).root == 7);

    // Saved with the project and read back.
    const auto json = ProjectSerializer::toJson(p);
    shared.keyMarkers.clear();
    shared.customScale = {};
    Project restored;
    CHECK(ProjectSerializer::fromJson(restored, json));
    CHECK(shared.keyMarkers.size() == 2 && shared.keyMarkers[1].root == 7);
    CHECK(shared.customScale.name == "Pentatonic just");
    CHECK(near(shared.customScale.cents[4], -14.0f));
    CHECK(!shared.customScale.used[5]);
    shared.keyMarkers.clear();
  }

  // The key applies even when the old scale-view switch is on Chromatic
  // (its default); only None turns the key off.
  {
    Project p;
    p.setUseProjectKey(true);
    p.setProjectKey(9, ScaleMode::Minor);
    p.setScaleMode(ScaleMode::Chromatic);
    CHECK(ScaleUtils::keyAt(p, 0.0).mode == ScaleMode::Minor);
    CHECK(ScaleUtils::keyAt(p, 0.0).root == 9);
    // A minor: G (67) is in the key, G# (68) is not, so it is a full miss.
    CHECK(near(ScaleUtils::snapMidiToScale(67.0f, ScaleMode::Minor, 9, 440), 67.0f));
    CHECK(std::abs(ScaleUtils::snapMidiToScale(68.0f, ScaleMode::Minor, 9, 440) - 68.0f) >= 0.99f);
    p.setScaleMode(ScaleMode::None);
    CHECK(ScaleUtils::keyAt(p, 0.0).mode == ScaleMode::None);
  }

  // Custom scale as the key: the tuned E is the exact target, F is a miss.
  {
    shared.customScale.used = {{true, false, true, false, true, false, false, true, false, true, false, false}};
    shared.customScale.cents = {};
    shared.customScale.cents[4] = -14.0f;
    Project p;
    p.setUseProjectKey(true);
    p.setProjectKey(0, ScaleMode::Custom);
    p.setScaleMode(ScaleMode::Chromatic);
    CHECK(ScaleUtils::keyAt(p, 0.0).mode == ScaleMode::Custom);
    CHECK(near(ScaleUtils::snapMidiToScale(63.86f, ScaleMode::Custom, 0, 440), 63.86f));
    CHECK(std::abs(ScaleUtils::snapMidiToScale(65.0f, ScaleMode::Custom, 0, 440) - 65.0f) >= 0.99f);
    shared.customScale = {};
  }

  // Note colour: how far a note misses the key (0 blue ... 1 red).
  {
    const ScaleUtils::Key aMinor{9, ScaleMode::Minor};
    CHECK(near(ScaleUtils::keyMissAmount(69.0f, aMinor, 440), 0.0f));   // A: exact
    CHECK(near(ScaleUtils::keyMissAmount(69.15f, aMinor, 440), 0.3f));  // 15 cents sharp
    CHECK(near(ScaleUtils::keyMissAmount(68.0f, aMinor, 440), 1.0f));   // G#: not in A minor
    const ScaleUtils::Key none{0, ScaleMode::None};
    CHECK(near(ScaleUtils::keyMissAmount(68.0f, none, 440), 0.0f));     // no key: any semitone
    shared.customScale.used = {{true, false, true, false, true, false, false, true, false, true, false, false}};
    shared.customScale.cents = {};
    shared.customScale.cents[4] = -14.0f;
    const ScaleUtils::Key custom{0, ScaleMode::Custom};
    CHECK(near(ScaleUtils::keyMissAmount(63.86f, custom, 440), 0.0f));  // tuned E: exact
    CHECK(near(ScaleUtils::keyMissAmount(64.0f, custom, 440), 0.28f));  // plain E: 14 cents off
    CHECK(near(ScaleUtils::keyMissAmount(65.0f, custom, 440), 1.0f));   // F: not in the scale
    shared.customScale = {};
  }

  // Pitch correction target: "tune to key" on moves to the key, off to the
  // nearest semitone; both at the A4 the user set.
  {
    const ScaleUtils::Key aMinor{9, ScaleMode::Minor};
    // G# + 30 cents in A minor: to the key it goes to A, otherwise to G#.
    CHECK(near(ScaleUtils::pitchCorrectionTarget(68.3f, aMinor, true, 440), 69.0f));
    CHECK(near(ScaleUtils::pitchCorrectionTarget(68.3f, aMinor, false, 440), 68.0f));
    // A note just under G (in the key) stays on G either way.
    CHECK(near(ScaleUtils::pitchCorrectionTarget(66.9f, aMinor, true, 440), 67.0f));
    // A4 = 432: targets shift with it, on and off.
    const float offset = 12.0f * std::log2(432.0f / 440.0f);
    CHECK(near(ScaleUtils::pitchCorrectionTarget(69.0f + offset + 0.2f, aMinor, false, 432), 69.0f + offset));
    CHECK(near(ScaleUtils::pitchCorrectionTarget(68.0f + offset + 0.3f, aMinor, true, 432), 69.0f + offset));
    // No key: always the nearest semitone.
    const ScaleUtils::Key none{0, ScaleMode::None};
    CHECK(near(ScaleUtils::pitchCorrectionTarget(68.3f, none, true, 440), 68.0f));
    // Custom scale (C D E G A, E -14 ct): to the key F+20 goes to the tuned E;
    // off it goes to F.
    shared.customScale.used = {{true, false, true, false, true, false, false, true, false, true, false, false}};
    shared.customScale.cents = {};
    shared.customScale.cents[4] = -14.0f;
    const ScaleUtils::Key custom{0, ScaleMode::Custom};
    CHECK(near(ScaleUtils::pitchCorrectionTarget(65.2f, custom, true, 440), 67.0f) ||
          near(ScaleUtils::pitchCorrectionTarget(65.2f, custom, true, 440), 63.86f));
    CHECK(near(ScaleUtils::pitchCorrectionTarget(64.9f, custom, true, 440), 63.86f));
    CHECK(near(ScaleUtils::pitchCorrectionTarget(65.2f, custom, false, 440), 65.0f));
    shared.customScale = {};
  }

  std::printf("TuningTests passed\n");
  return 0;
}
