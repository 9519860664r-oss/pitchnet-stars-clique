// InstanceRegistry: the in-process list of PitchNet instances that the track
// list shows in hosts without ARA.

#include "../Source/Plugin/InstanceRegistry.h"
#include "../Source/Utils/Constants.h"
#include "TestAssert.h"

#include <cstdio>

int main() {
  juce::ScopedJuceInitialiser_GUI juceInit;
  auto &registry = InstanceRegistry::get();
  int ownerA = 0, ownerB = 0;

  const auto a = registry.add(&ownerA);
  const auto b = registry.add(&ownerB);
  CHECK(a != b);
  auto list = registry.list();
  CHECK(list.size() == 2 && list[0].id == a && list[1].id == b);
  CHECK(registry.ownerOf(b) == &ownerB);

  // Names: the host's, else "PitchNet <n>" by load order.
  CHECK(InstanceRegistry::displayName(list[1], 1) == "PitchNet 2");
  registry.setTrackInfo(b, "  Backs L  ", juce::Colours::red, true);
  list = registry.list();
  CHECK(InstanceRegistry::displayName(list[1], 1) == "Backs L");
  CHECK(list[1].hasColour && list[1].colour == juce::Colours::red);

  // Keys round-trip; foreign keys (ARA track keys) map to no instance.
  CHECK(InstanceRegistry::idFromKey(InstanceRegistry::keyFor(b)) == b);
  CHECK(InstanceRegistry::idFromKey("seq:123") == 0);

  // A published snapshot is light: notes and curves, no mel or original.
  Project project;
  project.addNote(Note(0, 10, 60.0f));
  auto &audio = project.getAudioData();
  audio.sampleRate = SAMPLE_RATE;
  audio.waveform.setSize(1, 100);
  audio.originalWaveform.setSize(1, 100);
  audio.melSpectrogram.assign(5, std::vector<float>(NUM_MELS, 0.0f));
  audio.f0.assign(5, 100.0f);
  registry.publish(a, project);
  list = registry.list();
  CHECK(list[0].snapshot != nullptr && list[0].revision == 1);
  CHECK(list[0].snapshot->getNotes().size() == 1);
  CHECK(list[0].snapshot->getAudioData().f0.size() == 5);
  CHECK(list[0].snapshot->getAudioData().waveform.getNumSamples() == 100);
  CHECK(list[0].snapshot->getAudioData().originalWaveform.getNumSamples() == 0);
  CHECK(list[0].snapshot->getAudioData().melSpectrogram.empty());
  registry.publish(a, project);
  CHECK(registry.list()[0].revision == 2);

  // Removal; operations on a removed id are harmless.
  registry.remove(a);
  CHECK(registry.list().size() == 1 && registry.ownerOf(a) == nullptr);
  registry.publish(a, project);
  registry.setTrackInfo(a, "gone", {}, false);
  CHECK(registry.list().size() == 1);
  registry.remove(b);
  CHECK(registry.list().empty());

  std::printf("InstanceRegistryTests passed\n");
  return 0;
}
