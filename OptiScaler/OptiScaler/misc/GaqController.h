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

    static double MeasuredFrameTimeMs()
    {
        const auto& state = State::Instance();
        auto* fg = state.CurrentFG();
        const bool fgActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

        if (fgActive && state.lastFGFrameTime > 0.0)
            return state.lastFGFrameTime;
        if (state.presentFrameTime > 0.0)
            return state.presentFrameTime;
        if (state.lastFGFrameTime > 0.0)
            return state.lastFGFrameTime;

        return 0.0;
    }

    // Publishes the steady-state PID ratio in 0.05 steps gated by dwellMs = 500.0 while allowing transient
    // GaqMotionBoost offsets to react rapidly to fast camera turns without being frozen by PID dwell.
    float Update(double nowMs, double measuredFrameMs = 0.0)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        auto* config = Config::Instance();
        const float minRatio = std::clamp(config->GaqMinRatio.value_or_default(), 1.0f, 3.0f);
        const float maxRatio = std::clamp(config->GaqMaxRatio.value_or_default(), minRatio, 4.0f);

        UpdateRawLocked(nowMs, measuredFrameMs, minRatio, maxRatio);

        if (!config->GaqEnabled.value_or_default())
        {
            _publishedBaseRatio = 0.0f;
            _lastPublishedBase = 0.0f;
            _publishedRatio = 0.0f;
            _lastPublishMs = 0.0;
            _lastMotionPublishMs = 0.0;
            return _currentRatio;
        }

        constexpr float step = 0.05f;
        constexpr double dwellMs = 500.0;
        constexpr double motionCooldownMs = 45.0;

        const float baseSnapped = std::clamp(std::round(_currentRatio / step) * step, minRatio, maxRatio);
        if (_publishedBaseRatio <= 0.0f)
        {
            _publishedBaseRatio = baseSnapped;
            _lastPublishMs = nowMs;
        }
        else if (std::abs(_currentRatio - _publishedBaseRatio) >= step * 1.5f && (nowMs - _lastPublishMs) >= dwellMs)
        {
            _publishedBaseRatio = baseSnapped;
            _lastPublishMs = nowMs;
        }

        const float targetWithMotion = std::clamp(_publishedBaseRatio + _motionBoostOffset, minRatio, maxRatio);
        const float motionSnapped = std::clamp(std::round(targetWithMotion / step) * step, minRatio, maxRatio);

        if (_publishedRatio <= 0.0f)
        {
            _publishedRatio = motionSnapped;
            _lastPublishedBase = _publishedBaseRatio;
            _lastMotionPublishMs = nowMs;
        }
        else if (motionSnapped != _publishedRatio)
        {
            const bool baseChanged = (_publishedBaseRatio != _lastPublishedBase);
            const float delta = std::abs(targetWithMotion - _publishedRatio);
            if (baseChanged || delta >= step * 1.2f ||
                (motionSnapped > _publishedRatio && (nowMs - _lastMotionPublishMs) >= motionCooldownMs) ||
                (motionSnapped < _publishedRatio && (nowMs - _lastMotionPublishMs) >= motionCooldownMs * 2.0))
            {
                _publishedRatio = motionSnapped;
                _lastPublishedBase = _publishedBaseRatio;
                _lastMotionPublishMs = nowMs;
            }
        }

        return _publishedRatio;
    }

    float UpdateRaw(double nowMs, double measuredFrameMs = 0.0)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* config = Config::Instance();
        const float minRatio = std::clamp(config->GaqMinRatio.value_or_default(), 1.0f, 3.0f);
        const float maxRatio = std::clamp(config->GaqMaxRatio.value_or_default(), minRatio, 4.0f);
        UpdateRawLocked(nowMs, measuredFrameMs, minRatio, maxRatio);
        return std::clamp(_currentRatio + _motionBoostOffset, minRatio, maxRatio);
    }

    float CurrentRatio() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto* config = Config::Instance();
        const float minRatio = std::clamp(config->GaqMinRatio.value_or_default(), 1.0f, 3.0f);
        const float maxRatio = std::clamp(config->GaqMaxRatio.value_or_default(), minRatio, 4.0f);
        if (_publishedRatio > 0.0f && config->GaqEnabled.value_or_default())
            return std::clamp(_publishedRatio, minRatio, maxRatio);
        return std::clamp(_currentRatio + _motionBoostOffset, minRatio, maxRatio);
    }

  private:
    void UpdateRawLocked(double nowMs, double measuredFrameMs, float minRatio, float maxRatio)
    {
        auto* config = Config::Instance();
        auto& state = State::Instance();

        if (!config->GaqEnabled.value_or_default())
        {
            _currentRatio = minRatio;
            _integral = 0.0f;
            _lastError = 0.0f;
            _motionBoostOffset = 0.0f;
            _lastTimeMs = 0.0;
            _emaActiveMs = 0.0;
            _emaUpscalerMs = 0.0;
            _slowMs = 0.0;
            _probeBaseRatio = 0.0f;
            _cpuBound = false;
            _reprobePending = false;
            state.gaqCpuBound = false;
            return;
        }

        _currentRatio = std::clamp(_currentRatio, minRatio, maxRatio);

        const float targetFps = std::clamp(config->GaqTargetFps.value_or_default(), 20.0f, 360.0f);
        float targetFrameTime = 1000.0f / targetFps;

        auto* fg = state.CurrentFG();
        const bool fgActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();
        const bool dmfgActive = fgActive && config->FGXeFGDynamicMFG.value_or_default();

        const double fallbackFrameTime =
            fgActive && state.lastFGFrameTime > 0.0
                ? state.lastFGFrameTime
                : (state.presentFrameTime > 0.0 ? state.presentFrameTime : state.lastFGFrameTime);
        const double rawFrameTime = measuredFrameMs > 0.0 ? measuredFrameMs : fallbackFrameTime;
        const double frameTimeMs = rawFrameTime > 0.0 ? rawFrameTime : 16.6;

        // Discard extreme outlier frames (loading screens, pause transitions > 250ms) and rebase timer
        if (frameTimeMs <= 0.0 || frameTimeMs > 250.0)
        {
            _lastTimeMs = nowMs;
            return;
        }

        if (_lastTimeMs <= 0.0 || (nowMs - _lastTimeMs) > 500.0)
        {
            _lastTimeMs = nowMs;
            return;
        }

        const float dt = static_cast<float>((nowMs - _lastTimeMs) / 1000.0);

        // Rate-limit PID step so multiple queries within the same frame reuse the current ratio
        if (dt < 0.004f)
            return;

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

        // CPU-bound detection: lowering render resolution should cut frame time roughly in proportion to the
        // pixel count (1/ratio^2). If the ratio went up and frame time barely moved, the game is CPU-bound and
        // lower resolution only costs image quality, so GAQ returns to the minimum ratio and re-probes mildly.
        _slowMs = _slowMs > 0.0 ? (0.15 * activeFrameMs + 0.85 * _slowMs) : activeFrameMs;
        _probeAgeSec += dt;

        if (_cpuBound)
        {
            _cpuBoundAgeSec += dt;
            if (_cpuBoundAgeSec > 20.0f)
            {
                _cpuBound = false;
                _reprobePending = true;
                _probeBaseRatio = _currentRatio;
                _probeBaseMs = _slowMs;
                _probeAgeSec = 0.0f;
            }
            else
            {
                _currentRatio = std::max(minRatio, _currentRatio - 0.5f * dt);
                _integral = 0.0f;
                _lastError = 0.0f;
                _motionBoostOffset = 0.0f;
                state.gaqCpuBound = true;
                return;
            }
        }
        state.gaqCpuBound = false;

        if (_probeBaseRatio <= 0.0f || _currentRatio < _probeBaseRatio - 0.02f)
        {
            _probeBaseRatio = _currentRatio;
            _probeBaseMs = _slowMs;
            _probeAgeSec = 0.0f;
        }
        else if (_probeAgeSec >= 1.5f && _currentRatio - _probeBaseRatio >= 0.15f && _probeBaseMs > 0.0)
        {
            const float r0 = _probeBaseRatio;
            const float r1 = _currentRatio;
            const double expectedDrop = 1.0 - (r0 * r0) / (r1 * r1);
            const double actualDrop = (_probeBaseMs - _slowMs) / _probeBaseMs;

            if (actualDrop < expectedDrop * 0.3)
            {
                _cpuBound = true;
                _cpuBoundAgeSec = 0.0f;
            }
            else
            {
                _reprobePending = false;
            }

            _probeBaseRatio = r1;
            _probeBaseMs = _slowMs;
            _probeAgeSec = 0.0f;
        }
        else if (_probeAgeSec > 6.0f)
        {
            _probeBaseRatio = _currentRatio;
            _probeBaseMs = _slowMs;
            _probeAgeSec = 0.0f;
        }

        // Positive error means active frame time exceeded target -> increase ratio to reduce render pixels
        const float error = static_cast<float>(controlFrameMs) - targetFrameTime;

        // PID parameters calibrated for responsive resolution tracking
        constexpr float Kp = 0.015f;
        constexpr float Ki = 0.004f;
        constexpr float Kd = 0.002f;

        // During post-CPU-bound reprobe, cap excursion mildly (+0.20x above minRatio) until GPU scaling is proven
        const float effectiveMaxRatio = _reprobePending ? std::min(maxRatio, minRatio + 0.20f) : maxRatio;

        // Deadband of 0.3ms to prevent oscillation around target (bypassed if ADFG hit latency floor)
        if (std::abs(error) > 0.3f || (state.adfgClamped && error > 0.0f))
        {
            _integral = std::clamp(_integral + error * dt, -50.0f, 50.0f);
            const float derivative = (error - _lastError) / dt;
            _lastError = error;

            const float deltaRatio = gainScale * (Kp * error + Ki * _integral + Kd * derivative);
            _currentRatio = std::clamp(_currentRatio + deltaRatio, minRatio, effectiveMaxRatio);
        }
        else
        {
            // Within deadband: decay integral slowly
            _integral *= 0.95f;
            _lastError = 0.0f;
            _currentRatio = std::clamp(_currentRatio, minRatio, effectiveMaxRatio);
        }

        // Motion-Adaptive GAQ Boost: proactively raise upscale ratio during fast camera turns/swipes
        const float maxMotionBoost = std::clamp(config->GaqMotionBoost.value_or_default(), 0.0f, 0.6f);
        if (maxMotionBoost > 0.001f)
        {
            const int64_t mouseNow = state.mouseMotionCounter.load(std::memory_order_relaxed);
            const int64_t mouseStep = std::max<int64_t>(0, mouseNow - _lastMouseCounter);
            _lastMouseCounter = mouseNow;

            const float mouseRatePerSec = dt > 0.001f ? static_cast<float>(mouseStep) / dt : 0.0f;
            const float mouseSignal = std::clamp((mouseRatePerSec - 250.0f) / 1400.0f, 0.0f, 1.0f);
            const float camVel = static_cast<float>(state.cameraAngularSpeed.load(std::memory_order_relaxed));
            const float camSignal = std::clamp(camVel / 2.5f, 0.0f, 1.0f);

            const float targetBoost = std::max(mouseSignal, camSignal) * maxMotionBoost;
            const float alpha = targetBoost > _motionBoostOffset ? std::min(1.0f, dt * 12.0f) : std::min(1.0f, dt * 3.0f);
            _motionBoostOffset += (targetBoost - _motionBoostOffset) * alpha;
            if (_motionBoostOffset < 0.001f)
                _motionBoostOffset = 0.0f;
        }
        else
        {
            _motionBoostOffset = 0.0f;
        }
    }

    mutable std::mutex _mutex;
    float _currentRatio = 1.3f;
    float _integral = 0.0f;
    float _lastError = 0.0f;
    float _motionBoostOffset = 0.0f;
    int64_t _lastMouseCounter = 0;
    double _lastTimeMs = 0.0;
    double _emaActiveMs = 0.0;
    double _emaUpscalerMs = 0.0;
    double _slowMs = 0.0;
    float _publishedBaseRatio = 0.0f;
    float _lastPublishedBase = 0.0f;
    float _publishedRatio = 0.0f;
    double _lastPublishMs = 0.0;
    double _lastMotionPublishMs = 0.0;
    double _probeBaseMs = 0.0;
    float _probeBaseRatio = 0.0f;
    float _probeAgeSec = 0.0f;
    float _cpuBoundAgeSec = 0.0f;
    bool _cpuBound = false;
    bool _reprobePending = false;
};
