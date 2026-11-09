// Part of GNU-Upscaler (GPL-3.0)
#pragma once

#include <cstdint>

namespace MenuInput
{
inline constexpr int kMaxToggleDebounceMs = 1000;

inline uint32_t ToggleDebounceMsFromIni(int iniValue)
{
    return static_cast<uint32_t>(iniValue < 0 ? 0 : (iniValue > kMaxToggleDebounceMs ? kMaxToggleDebounceMs : iniValue));
}

// One gate per toggle key (menu, overlay). Not thread-safe: used from the menu thread only.
struct ToggleGate
{
    uint64_t lastAcceptedMs = 0; // time of the last accepted edge
    bool anyAccepted = false;    // false until the first accepted edge

    bool Accept(uint64_t nowMs, uint32_t debounceMs)
    {
        if (debounceMs != 0 && anyAccepted && nowMs >= lastAcceptedMs && nowMs - lastAcceptedMs < debounceMs)
            return false;
        lastAcceptedMs = nowMs;
        anyAccepted = true;
        return true;
    }
};
} // namespace MenuInput
