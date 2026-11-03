#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_6.h>
#include <windows.h>
#pragma comment(lib, "dxgi.lib")
#endif

/**
 * Which GPU to run inference on when the user has not picked one.
 *
 * DirectML numbers adapters in IDXGIFactory::EnumAdapters order, and that
 * order is not "integrated first": on a laptop whose screen runs on the
 * discrete GPU, the integrated one comes last. Taking the last adapter put
 * the editor on an integrated Radeon (three times slower than the GeForce
 * next to it, and busy drawing the screen, so the UI stalled for a second
 * at a time). Windows' own high-performance choice is used instead, with
 * the most dedicated video memory as the fallback.
 */
namespace GpuAdapters
{
struct Choice
{
    int index = 0;            // DirectML device id
    unsigned long long dedicatedMemory = 0;
};

inline Choice fastest()
{
#ifdef _WIN32
    static const Choice choice = []() -> Choice {
        IDXGIFactory1 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
            factory == nullptr)
            return {};

        // Windows' high-performance adapter, by LUID.
        LUID preferred{};
        bool havePreferred = false;
        IDXGIFactory6 *factory6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&factory6))) &&
            factory6 != nullptr)
        {
            for (UINT i = 0;; ++i)
            {
                IDXGIAdapter1 *adapter = nullptr;
                if (FAILED(factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                __uuidof(IDXGIAdapter1),
                                                                reinterpret_cast<void **>(&adapter))) ||
                    adapter == nullptr)
                    break;
                DXGI_ADAPTER_DESC1 desc{};
                const bool hardware = SUCCEEDED(adapter->GetDesc1(&desc)) &&
                                      (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
                adapter->Release();
                if (hardware)
                {
                    preferred = desc.AdapterLuid;
                    havePreferred = true;
                    break;
                }
            }
            factory6->Release();
        }

        // Its place in EnumAdapters order (what DirectML's device id means);
        // without a preference, the adapter with the most video memory.
        Choice best;
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1 *adapter = nullptr;
            const HRESULT hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr) || adapter == nullptr)
                continue;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
            {
                if (havePreferred && desc.AdapterLuid.LowPart == preferred.LowPart &&
                    desc.AdapterLuid.HighPart == preferred.HighPart)
                {
                    const Choice found{static_cast<int>(i), desc.DedicatedVideoMemory};
                    adapter->Release();
                    factory->Release();
                    return found;
                }
                if (desc.DedicatedVideoMemory > best.dedicatedMemory)
                    best = {static_cast<int>(i), desc.DedicatedVideoMemory};
            }
            adapter->Release();
        }
        factory->Release();
        return best;
    }();
    return choice;
#else
    return {};
#endif
}

inline int fastestDirectMLIndex() { return fastest().index; }

/** A GPU with its own memory (not the integrated one drawing the screen):
    worth rendering on without being asked. */
inline bool hasDiscreteGpu() { return fastest().dedicatedMemory >= (1ull << 30); }
} // namespace GpuAdapters
