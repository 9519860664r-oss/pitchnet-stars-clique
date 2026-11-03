#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/PerfTrace.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/**
 * Every PitchNet instance loaded in this process, so each plug-in window can
 * list the other tracks, in hosts without ARA (FL Studio).
 *
 * The plug-in DLL is loaded once per process, so this function-local static
 * is shared by all instances the host runs in-process. A host that bridges or
 * sandboxes plug-ins into separate processes gives each instance its own
 * registry; the track list then shows only that instance.
 *
 * Instances publish a light copy of their project (notes, pitch curves,
 * rendered audio; no mel or original audio) after each analysis and render.
 * Listeners are told on the message thread, coalesced.
 */
class InstanceRegistry : public juce::ChangeBroadcaster
{
public:
  struct Entry
  {
    std::uint64_t id = 0;
    void *owner = nullptr; // the PitchNetAudioProcessor
    juce::String name;      // host track / channel name, if it sent one
    juce::Colour colour;
    bool hasColour = false;
    std::shared_ptr<const Project> snapshot;
    std::uint64_t revision = 0; // bumps on every snapshot
    // The notes as soon as they are edited, before the audio is rendered
    // (no audio in it): what doubles in unison follow, so they render
    // alongside the lead instead of after it.
    std::shared_ptr<const Project> notes;
    std::uint64_t notesRevision = 0;
    // Lead and doubles: tracks with the same group id belong together; one
    // is the lead. The lead carries the group's unison switch and strength.
    juce::String groupId;
    int role = 0; // Role
    bool unisonOn = false;
    float unisonStrength = 1.0f; // 0..1
    bool timingOn = false;       // doubles follow the lead's timing too
    float timingStrength = 1.0f; // 0..1
  };

  enum Role { None = 0, Lead = 1, Double = 2 };

  void setGroup(std::uint64_t id, const juce::String &groupId, int role)
  {
    bool changed = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        changed = entry->groupId != groupId || entry->role != role;
        entry->groupId = role == None ? juce::String() : groupId;
        entry->role = groupId.isEmpty() ? None : role;
      }
    }
    if (changed)
      sendChangeMessage();
  }

  void setUnison(std::uint64_t id, bool on, float strength)
  {
    bool changed = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        changed = entry->unisonOn != on || std::abs(entry->unisonStrength - strength) > 1.0e-4f;
        entry->unisonOn = on;
        entry->unisonStrength = juce::jlimit(0.0f, 1.0f, strength);
      }
    }
    if (changed)
      sendChangeMessage();
  }

  void setTiming(std::uint64_t id, bool on, float strength)
  {
    bool changed = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        changed = entry->timingOn != on || std::abs(entry->timingStrength - strength) > 1.0e-4f;
        entry->timingOn = on;
        entry->timingStrength = juce::jlimit(0.0f, 1.0f, strength);
      }
    }
    if (changed)
      sendChangeMessage();
  }

  static InstanceRegistry &get()
  {
    static InstanceRegistry registry;
    return registry;
  }

  std::uint64_t add(void *owner)
  {
    std::uint64_t id = 0;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      id = ++lastId;
      Entry entry;
      entry.id = id;
      entry.owner = owner;
      entries.push_back(std::move(entry));
    }
    sendChangeMessage();
    return id;
  }

  void remove(std::uint64_t id)
  {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      entries.erase(std::remove_if(entries.begin(), entries.end(),
                                   [id](const Entry &e) { return e.id == id; }),
                    entries.end());
    }
    sendChangeMessage();
  }

  void setTrackInfo(std::uint64_t id, const juce::String &name,
                    juce::Colour colour, bool hasColour)
  {
    bool changed = false;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        changed = entry->name != name || entry->hasColour != hasColour ||
                  (hasColour && entry->colour != colour);
        entry->name = name;
        entry->colour = colour;
        entry->hasColour = hasColour;
      }
    }
    if (changed)
      sendChangeMessage();
  }

  /** Publish a light copy of `project` for the other windows. */
  void publish(std::uint64_t id, const Project &project)
  {
    PerfTrace::Scope scope("publish #" + juce::String(static_cast<juce::int64>(id)));
    std::shared_ptr<const Project> light = project.makeLightCopy(true);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        entry->snapshot = light;
        ++entry->revision;
        entry->notes = std::move(light);
        ++entry->notesRevision;
      }
    }
    sendChangeMessage();
  }

  /** Publish just-edited notes (rendering has only started). */
  void publishNotes(std::uint64_t id, const Project &project)
  {
    PerfTrace::Scope scope("publish notes #" + juce::String(static_cast<juce::int64>(id)));
    std::shared_ptr<const Project> light = project.makeLightCopy(false);
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (auto *entry = find(id))
      {
        entry->notes = std::move(light);
        ++entry->notesRevision;
      }
    }
    sendChangeMessage();
  }

  /** Copy of every entry, in load order. */
  std::vector<Entry> list() const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return entries;
  }

  /** The owner of `id`, or nullptr once it has gone. */
  void *ownerOf(std::uint64_t id) const
  {
    const std::lock_guard<std::mutex> lock(mutex);
    for (const auto &entry : entries)
      if (entry.id == id)
        return entry.owner;
    return nullptr;
  }

  static juce::String keyFor(std::uint64_t id)
  {
    return "inst:" + juce::String(static_cast<juce::int64>(id));
  }

  static std::uint64_t idFromKey(const juce::String &key)
  {
    if (!key.startsWith("inst:"))
      return 0;
    return static_cast<std::uint64_t>(key.substring(5).getLargeIntValue());
  }

  /** Display name: the host's, else "PitchNet <n>" by load order. */
  static juce::String displayName(const Entry &entry, int indexInList)
  {
    const auto trimmed = entry.name.trim();
    return trimmed.isNotEmpty() ? trimmed
                                : "PitchNet " + juce::String(indexInList + 1);
  }

private:
  InstanceRegistry() = default;

  Entry *find(std::uint64_t id)
  {
    for (auto &entry : entries)
      if (entry.id == id)
        return &entry;
    return nullptr;
  }

  mutable std::mutex mutex;
  std::vector<Entry> entries;
  std::uint64_t lastId = 0;
};
