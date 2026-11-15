#pragma once

#include <Config.h>
#include <State.h>
#include <framegen/IFGFeature.h>
#include <low_latency/input/input_common.h>
#include <algorithm>
#include <cmath>
#include <mutex>

// GNU Adaptive Quality (GAQ): GPU-timestamp & sleep-aware PID Dynamic Resolution Scaling (DRS) Controller
class GaqController
{
  public:
    static GaqController& Instance()
    {
        static GaqController instance;
        return instance;
    }

    float Update(double nowMs)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto config = Config::Instance();
        auto& state = State::Instance();

        const float minRatio = std::clamp(config->GaqMinRatio.value_or_default(), 1.0f, 3.0f);
        const float maxRatio = std::clamp(config->GaqMaxRatio.value_or_default(), minRatio, 4.0f);

        if (!config->GaqEnabled.value_or_default())
        {
            _currentRatio = minRatio;
            _integral = 0.0f;
            _lastError = 0.0f;
            _lastTimeMs = 0.0;
            _emaActiveMs = 0.0;
            _emaUpscalerMs = 0.0;
            return _currentRatio;
        }

        _currentRatio = std::clamp(_currentRatio, minRatio, maxRatio);

        const float targetFps = std::clamp(config->GaqTargetFps.value_or_default(), 20.0f, 360.0f);
        float targetFrameTime = 1000.0f / targetFps;

        const bool fgActive = state.currentFG != nullptr && state.currentFG->IsActive() && !state.currentFG->IsPaused();
        const bool dmfgActive = fgActive && config->FGXeFGDynamicMFG.value_or_default();

        const double rawFrameTime = fgActive && state.lastFGFrameTime > 0.0
                                        ? state.lastFGFrameTime
                                        : (state.presentFrameTime > 0.0 ? state.presentFrameTime : state.lastFGFrameTime);
        const double frameTimeMs = rawFrameTime > 0.0 ? rawFrameTime : 16.6;

        // Discard extreme outlier frames (loading screens, pause transitions > 250ms) and rebase timer
        if (frameTimeMs <= 0.0 || frameTimeMs > 250.0)
        {
            _lastTimeMs = nowMs;
            return _currentRatio;
        }

        if (_lastTimeMs <= 0.0 || (nowMs - _lastTimeMs) > 500.0)
        {
            _lastTimeMs = nowMs;
            return _currentRatio;
        }

        const float dt = static_cast<float>((nowMs - _lastTimeMs) / 1000.0);

        // Rate-limit PID step so multiple queries within the same frame reuse the current ratio
        if (dt < 0.004f)
            return _currentRatio;

        _lastTimeMs = nowMs;

        // Subtract limiter & low-latency sleep so capped frames do not look GPU-bound to GAQ.
        // When Dynamic MFG is active it already stores (sleepMs + presentWaitMs) in lastLimiterSleepMs.
        const double sleepMs = dmfgActive ? state.lastLimiterSleepMs
                                          : (state.lastLimiterSleepMs + InputCommon::peek_sleep_ms());
        const double effectiveSleepMs = std::clamp(sleepMs, 0.0, std::max(0.0, frameTimeMs - 1.0));
        const double activeFrameMs = frameTimeMs - effectiveSleepMs;
        state.lastActiveFrameMs = activeFrameMs;

        // Incorporate actual upscaler GPU pass duration when available so heavy compute shaders react early
        const double upscalerGpuMs = state.lastUpscalerGpuMs;
        if (upscalerGpuMs > 0.0 && upscalerGpuMs < 100.0)
            _emaUpscalerMs = _emaUpscalerMs > 0.0 ? (0.25 * upscalerGpuMs + 0.75 * _emaUpscalerMs) : upscalerGpuMs;

        double controlFrameMs = activeFrameMs;
        if (dmfgActive)
        {
            // Smooth active frame time with EMA during ADFG to prevent resolution & multiplier oscillation
            _emaActiveMs = _emaActiveMs > 0.0 ? (0.25 * activeFrameMs + 0.75 * _emaActiveMs) : activeFrameMs;
            controlFrameMs = _emaActiveMs;
        }
        else
        {
            _emaActiveMs = activeFrameMs;
        }

        if (_emaUpscalerMs > targetFrameTime * 0.35)
            controlFrameMs += (_emaUpscalerMs - targetFrameTime * 0.35) * 0.25;

        // Coordinate with ADFG latency floor: if base FPS dropped below FGXeFGMinBaseFps, tighten budget
        // and boost upward ratio recovery so resolution drops instead of hitting FG latency limits
        float gainScale = dmfgActive ? 0.7f : 1.0f;
        if (state.adfgClamped)
        {
            const float minBaseFps = config->FGXeFGMinBaseFps.value_or_default();
            if (minBaseFps > 0.0f)
                targetFrameTime = std::min(targetFrameTime, 1000.0f / minBaseFps);
            gainScale = 1.5f;
        }

        // Positive error means active frame time exceeded target -> increase ratio to reduce render pixels
        const float error = static_cast<float>(controlFrameMs) - targetFrameTime;

        // PID parameters calibrated for responsive resolution tracking
        constexpr float Kp = 0.015f;
        constexpr float Ki = 0.004f;
        constexpr float Kd = 0.002f;

        // Deadband of 0.3ms to prevent oscillation around target (bypassed if ADFG hit latency floor)
        if (std::abs(error) > 0.3f || (state.adfgClamped && error > 0.0f))
        {
            _integral = std::clamp(_integral + error * dt, -50.0f, 50.0f);
            const float derivative = (error - _lastError) / dt;
            _lastError = error;

            const float deltaRatio = gainScale * (Kp * error + Ki * _integral + Kd * derivative);
            _currentRatio = std::clamp(_currentRatio + deltaRatio, minRatio, maxRatio);
        }
        else
        {
            // Within deadband: decay integral slowly
            _integral *= 0.95f;
            _lastError = 0.0f;
        }

        return _currentRatio;
    }

    float Update(double nowMs, double frameTimeMs)
    {
        return Update(nowMs);
    }

    float CurrentRatio() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _currentRatio;
    }

  private:
    mutable std::mutex _mutex;
    float _currentRatio = 1.3f;
    float _integral = 0.0f;
    float _lastError = 0.0f;
    double _lastTimeMs = 0.0;
    double _emaActiveMs = 0.0;
    double _emaUpscalerMs = 0.0;
};
