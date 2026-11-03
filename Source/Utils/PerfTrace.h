#pragma once

#include "../JuceHeader.h"

#include <cstdio>
#include <mutex>

/**
 * Timing trace for finding lags. Off unless PITCHNET_PERF_LOG names a file;
 * then every mark is appended to it as "<ms since start>\t<text>".
 */
namespace PerfTrace
{
inline std::FILE *file()
{
    static std::FILE *f = [] {
        const auto path = juce::SystemStats::getEnvironmentVariable("PITCHNET_PERF_LOG", {});
        return path.isEmpty() ? nullptr : std::fopen(path.toRawUTF8(), "a");
    }();
    return f;
}

inline double now() { return juce::Time::getMillisecondCounterHiRes(); }

inline void mark(const juce::String &text)
{
    if (auto *f = file())
    {
        static std::mutex lock;
        const std::lock_guard<std::mutex> guard(lock);
        std::fprintf(f, "%.1f\t%s\n", now(), text.toRawUTF8());
        std::fflush(f);
    }
}

/** Logs how long its scope took. */
struct Scope
{
    explicit Scope(juce::String what, double onlyOverMs = 0.0)
        : text(std::move(what)), start(file() ? now() : 0.0), minMs(onlyOverMs) {}
    ~Scope()
    {
        if (file() != nullptr && now() - start >= minMs)
            mark(text + " took " + juce::String(now() - start, 1) + " ms");
    }
    juce::String text;
    double start;
    double minMs;
};
} // namespace PerfTrace
