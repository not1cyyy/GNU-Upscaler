// Part of GNU-Upscaler (GPL-3.0)
#pragma once

#include <cstdint>

namespace MenuInput
{
inline bool TickBefore(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

enum class LatchVerdict : uint8_t
{
    Fire,     // toggle the menu
    Stale,    // the press is older than the frame that armed the latch
    Disarmed, // the key has not been seen up since the last fire
    Repeat,   // an autorepeat key-down
    Gated,    // shortcuts are off this frame
};

inline const char* LatchVerdictName(LatchVerdict v)
{
    switch (v)
    {
    case LatchVerdict::Fire:     return "fire";
    case LatchVerdict::Stale:    return "stale";
    case LatchVerdict::Disarmed: return "disarmed";
    case LatchVerdict::Repeat:   return "repeat";
    case LatchVerdict::Gated:    return "gated";
    }
    return "?";
}

// One latch per key (the menu key). Not thread-safe: used from the menu thread only.
struct KeyPressLatch
{
    static constexpr uint32_t kSlackMs = 20;

    bool armed = false;     // false until the key has been seen up
    uint32_t armedAtMs = 0; // tick of the first frame the key was up since the last fire

    LatchVerdict OnPress(uint32_t pressTimeMs, bool repeat, bool gated)
    {
        if (repeat)
            return LatchVerdict::Repeat;
        if (!armed)
            return LatchVerdict::Disarmed;
        if (TickBefore(pressTimeMs + kSlackMs, armedAtMs))
            return LatchVerdict::Stale;
        if (gated)
            return LatchVerdict::Gated;
        armed = false;
        return LatchVerdict::Fire;
    }

    void OnFrame(bool keyUp, uint32_t nowMs)
    {
        if (keyUp && !armed)
        {
            armed = true;
            armedAtMs = nowMs;
        }
    }

    void Reset()
    {
        armed = false;
        armedAtMs = 0;
    }
};
} // namespace MenuInput
