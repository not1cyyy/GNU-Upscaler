#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include "State.h"
#include <framegen/IFGFeature.h>
#include <low_latency/input/input_common.h>
// #include "hooks/D3D11Hooks.h"

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = Util::GetTimestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        current_time = Util::GetTimestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns)
{
    // Reduced from 2ms to 250us (0.25ms) to prevent pinning a CPU core at 100% in a busy spin-loop
    constexpr int64_t busywait_threshold = 250'000; // 0.25ms
    int status {};
    auto current_time = Util::GetTimestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
        status = timer_sleep((ns - busywait_threshold) / 100);

    // Fine-grained busy wait completes the remaining sub-millisecond precision
    if (int64_t sleep_deviation = ns - (Util::GetTimestamp() - current_time); sleep_deviation > 0)
        busywait_sleep(sleep_deviation);

    return status;
}

float FrameLimit::targetFps()
{
    auto& state = State::Instance();
    auto fpsLimit = Config::Instance()->FramerateLimit.value_or_default();

    if (Config::Instance()->FGXeFGDynamicMFG.value_or_default() &&
        Config::Instance()->FGXeFGTargetAsCapDMFG.value_or_default())
    {
        if (auto fg = state.currentFG; fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
                                       state.activeFgOutput == FGOutput::XeFG && state.xefgDynamicCapFps > 0.0f &&
                                       (fpsLimit <= 0.0f || state.xefgDynamicCapFps < fpsLimit))
        {
            return state.xefgDynamicCapFps;
        }
    }

    return fpsLimit;
}

void FrameLimit::sleep(bool fgActive)
{
    if (auto fpsCap = targetFps(); fpsCap > 0.0f)
    {
        uint64_t min_interval_us = std::clamp((uint64_t) (1'000'000 / fpsCap), 0ULL, 100'000'000ULL);

        // The limit counts presented frames, the real frames come once per batch
        if (fgActive)
        {
            auto fg = State::Instance().CurrentFG();
            min_interval_us *= fg != nullptr ? fg->GetInterpolatedFrameCount() + 1 : 2;
        }

        static uint64_t previous_frame_time = 0;
        uint64_t current_time = Util::GetTimestamp();
        uint64_t frame_time = current_time - previous_frame_time;
        if (frame_time < 1000 * min_interval_us)
        {
            static bool errorLogged = false;

            const auto sleepStart = Util::GetTimestamp();
            if (auto res = combined_sleep(min_interval_us * 1000 - frame_time); res && !errorLogged)
            {
                LOG_ERROR("Sleep command failed: {}", res);
                errorLogged = true;
            }

            // Dynamic MFG only sees the low latency output's sleep, its "reachable fps" needs the
            // limiter's wait too, else it underestimates what the game can reach and mis-ratchets
            InputCommon::add_sleep_ms((Util::GetTimestamp() - sleepStart) / 1e6);
        }
        previous_frame_time = Util::GetTimestamp();
    }
}
