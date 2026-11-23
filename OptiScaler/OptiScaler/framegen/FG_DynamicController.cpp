#include "pch.h"
#include "FG_DynamicController.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <low_latency/input/input_common.h>

#include <algorithm>
#include <cmath>
#include <format>

bool FG_DynamicController::EvaluateBaseFpsGuard(bool enabled, float minBaseFps)
{
    auto& state = State::Instance();

    if (!enabled || minBaseFps <= 0.0f)
    {
        _adfgClamped = false;
        _baseFrameMsEma = 0.0;
        state.adfgClamped = false;
        return false;
    }

    const double baseFrameMs = state.lastFGFrameTime > 0.0
                                   ? state.lastFGFrameTime
                                   : (state.presentFrameTime > 0.0 ? state.presentFrameTime : 0.0);

    if (baseFrameMs > 0.0 && baseFrameMs <= 250.0)
        _baseFrameMsEma = _baseFrameMsEma > 0.0 ? (0.25 * baseFrameMs + 0.75 * _baseFrameMsEma) : baseFrameMs;

    const double currentBaseFps = _baseFrameMsEma > 0.0 ? (1000.0 / _baseFrameMsEma) : 60.0;

    // 8% hysteresis prevents rapid 2X <-> 1X toggling around the latency floor
    if (_adfgClamped)
    {
        if (currentBaseFps >= static_cast<double>(minBaseFps) * 1.08)
            _adfgClamped = false;
    }
    else if (currentBaseFps < static_cast<double>(minBaseFps))
    {
        _adfgClamped = true;
    }

    state.adfgClamped = _adfgClamped;
    return _adfgClamped;
}

FG_DynamicController::Result FG_DynamicController::Update(const Options& opt)
{
    auto* config = Config::Instance();
    auto& state = State::Instance();

    Result result;
    const int safeTargetCount = std::max(opt.targetCount, 1);
    result.dynamic = opt.enabled;
    result.ownChange = result.dynamic && _active;
    result.count = safeTargetCount;
    result.generationOff = false;

    if (result.dynamic)
    {
        const double now = Util::MillisecondsNow();
        const float target = EffectiveTargetFps(now, opt.window, opt.targetFps);
        const int count1 = opt.currentCount >= 1 ? std::min(opt.currentCount, safeTargetCount) : safeTargetCount;
        const int current = opt.generationOff ? 0 : count1;
        const float refresh = DisplayRefreshFps(now, opt.window);

        // Consume OptiScaler limiter/low-latency sleep even while unfocused so stale sleep doesn't accumulate
        const double sleepMs = InputCommon::take_sleep_ms();

        const int focus = Focused(opt.window);
        if (focus == 0 || (focus < 0 && _unfocused))
        {
            if (!_unfocused)
                LOG_INFO("Dynamic FG: game window lost focus, keeping {}X", current + 1);

            _unfocused = true;
            _active = true;
            result.generationOff = opt.generationOff;
            result.count = count1;
            return result;
        }

        if (_unfocused)
        {
            LOG_INFO("Dynamic FG: game window back in foreground, restarting at {}X", current + 1);
            _unfocused = false;
            _count.Reset();
            _cap.Reset();
            _capActive = false;
        }

        const double waitMs = sleepMs + std::max(0.0, opt.presentWaitMs);
        state.lastLimiterSleepMs = waitMs > 0.0 ? waitMs : 0.0;

        const int rounding = std::clamp(opt.rounding, 0, 2);
        const int minCount = (opt.allow1X || safeTargetCount == 1) ? 0 : 1;

        const bool cap = opt.useTargetAsCap && target > 0.0f && safeTargetCount > 1;
        if (opt.updateCapState)
            state.xefgDynamicCapFps = cap ? target : 0.0f;

        int count = current;

        if (cap)
        {
            if (!_capActive)
                _cap.Reset();

            count = _cap.Update({ now, state.lastFGFrameTime, sleepMs, target, safeTargetCount, count1 });

            if (count != current || now - _logMs >= 5000.0)
            {
                static constexpr const char* reasons[] = { "", "up", "down", "try down", "undo" };
                const auto reason = _cap.LastReason();
                const auto change = count != current ? std::format(" -> {}X", count + 1) : std::string();
                const auto why = reason != XeFG_DynamicCap::Reason::None ? std::format(" ({})", reasons[(int) reason])
                                                                         : std::string();

                spdlog::log(count != current ? spdlog::level::info : spdlog::level::debug,
                            __FUNCTION__ " Dynamic FG {}X{}{}: presented {:.1f} fps, target {:.1f} as cap, "
                                         "reachable {:.1f} fps, back-off {:.0f} s",
                            current + 1, change, why, _cap.PresentedFps(), target, _cap.ReachableFps(),
                            _cap.BackoffMs() / 1000.0);

                _logMs = now;
            }
        }
        else
        {
            if (_capActive)
                _count.Restart();

            count = _count.Update({ now, state.lastFGFrameTime, waitMs, target, refresh,
                                    config->FramerateLimit.value_or_default(), safeTargetCount, current, rounding,
                                    minCount });

            if (count != current || now - _logMs >= 5000.0)
            {
                static constexpr const char* reasons[] = { "", "up", "down", "try down", "undo", "no gain, back" };
                static constexpr const char* roundings[] = { "at most", "around", "at least" };
                const auto reason = _count.LastReason();
                const auto change = count != current ? std::format(" -> {}X", count + 1) : std::string();
                const auto why = reason != XeFG_DynamicCount::Reason::None
                                     ? std::format(" ({})", reasons[(int) reason])
                                     : std::string();

                spdlog::log(count != current ? spdlog::level::info : spdlog::level::debug,
                            __FUNCTION__ " Dynamic FG {}X{}{}: presented {:.1f} fps, target {:.1f} (set {:.1f}, "
                                         "rounding {}, {}, refresh {:.0f}), one step less {:.1f}, waits {:.0f}%, "
                                         "frame cost {:.2f} ms, back-off {:.0f} s",
                            current + 1, change, why, _count.PresentedFps(), _count.TargetFps(), target,
                            roundings[rounding], _count.Uncapped() ? "no cap" : "capped", refresh, _count.LowerFps(),
                            _count.WaitShare() * 100.0, _count.FrameCostMs(), _count.BackoffMs() / 1000.0);

                _logMs = now;
            }
        }

        _capActive = cap;

        // Latency Floor Guard: smoothly step down multiplier (and 2X -> 1X when allowed) if native FPS dips
        if (EvaluateBaseFpsGuard(true, opt.minBaseFps))
        {
            const double severeFloor = static_cast<double>(opt.minBaseFps) * 0.85;
            const double emaFps = _baseFrameMsEma > 0.0 ? (1000.0 / _baseFrameMsEma) : 60.0;

            if (safeTargetCount == 1 && opt.allow1X)
            {
                count = 0;
            }
            else if (opt.allow1X && emaFps < severeFloor)
            {
                count = 0;
            }
            else if (count > 1)
            {
                count = 1;
            }
        }

        result.generationOff = (count == 0);
        result.count = std::max(count, 1);
    }
    else
    {
        _adfgClamped = false;
        _baseFrameMsEma = 0.0;
        state.adfgClamped = false;

        if (opt.updateCapState)
            state.xefgDynamicCapFps = 0.0f;

        if (_active)
        {
            _count.Reset();
            _cap.Reset();
            _capActive = false;
        }
    }

    _active = result.dynamic;
    return result;
}

void FG_DynamicController::Restart()
{
    _count.Restart();
    _cap.Reset();
    _adfgClamped = false;
    _baseFrameMsEma = 0.0;
}

int FG_DynamicController::Focused(HWND window)
{
    if (window == nullptr || !IsWindow(window))
        return 1;

    if (IsIconic(GetAncestor(window, GA_ROOT)))
        return 0;

    const auto foreground = GetForegroundWindow();
    if (foreground == nullptr)
        return -1;

    if (GetAncestor(foreground, GA_ROOT) == GetAncestor(window, GA_ROOT))
        return 1;

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}

float FG_DynamicController::DisplayRefreshFps(double nowMs, HWND window)
{
    if (window != nullptr && (_refreshRate <= 0 || nowMs - _refreshCheckedMs >= 2000.0))
    {
        _refreshCheckedMs = nowMs;
        _refreshRate = Util::GetActiveRefreshRate(window);
    }

    return static_cast<float>(std::max(_refreshRate, 0));
}

float FG_DynamicController::EffectiveTargetFps(double nowMs, HWND window, float configuredTargetFps)
{
    const auto limit = Config::Instance()->FramerateLimit.value_or_default();

    if (configuredTargetFps > 0.0f)
        return limit > 0.0f ? std::min(configuredTargetFps, limit) : configuredTargetFps;

    if (limit > 0.0f)
        return limit;

    // Just under the refresh rate (116 at 120 Hz), matching VRR headroom
    const auto refresh = DisplayRefreshFps(nowMs, window);
    return refresh > 0.0f ? std::floor(refresh - refresh * refresh / 3600.0f) : 0.0f;
}
