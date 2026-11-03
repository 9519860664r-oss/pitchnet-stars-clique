#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"

#include <cstdint>
#include <map>

/**
 * Doubles in unison: while a group's Unison switch is on, every double's
 * notes that double a lead note follow it (by the group's strength); notes
 * of its own (ad-libs, harmonies) are left alone. Switching it off, or taking
 * a track out of the group, puts every note back on its own pitch.
 *
 * One coordinator per process, alive while any PitchNet instance is. It
 * watches the instance registry on the message thread, so doubles follow the
 * lead even when their windows are closed.
 */
class UnisonCoordinator final : private juce::Timer, private juce::ChangeListener
{
public:
  /** Called by each processor when it is created / destroyed. */
  static void retain();
  static void release();

  /** Look again now (after a switch, strength or group change). */
  static void poke();

  /** Timing, then Unison, on one double against its lead (laid out in the
      double's frames). True when a note moved. ARA hosts call this for each
      clip of a double, since their clips are not separate instances. */
  static bool applyToDouble(Project &project, const Project *lead, bool unisonOn,
                            float unisonStrength, bool timingOn, float timingStrength);

private:
  UnisonCoordinator();
  ~UnisonCoordinator() override;
  void timerCallback() override;
  void changeListenerCallback(juce::ChangeBroadcaster *) override;
  void update();

  std::map<std::uint64_t, juce::String> applied; // per double: what was applied last
  bool passRunning = false; // doubles still being brought up to date, one per tick
};
