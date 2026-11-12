#pragma once

#include <Config.h>
#include <State.h>
#include <algorithm>
#include <cmath>

// GNU Adaptive Quality (GAQ): Closed-loop PID Dynamic Resolution Scaling (DRS) Controller
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
        auto config = Config::Instance();
        auto& state = State::Instance();

        if (!config->GaqEnabled.value_or_default())
        {
            _currentRatio = config->GaqMinRatio.value_or_default();
            _integral = 0.0f;
            _lastError = 0.0f;
            return _currentRatio;
        }

        const float targetFps = std::clamp(config->GaqTargetFps.value_or_default(), 20.0f, 360.0f);
        const float targetFrameTime = 1000.0f / targetFps;

        // Use presentFrameTime first (updated every present regardless of FG state), then fall back to lastFGFrameTime
        const double frameTimeMs = state.presentFrameTime > 0.0 ? state.presentFrameTime
                                                               : (state.lastFGFrameTime > 0.0 ? state.lastFGFrameTime : 16.6);

        // Discard extreme outlier frames (loading screens, pause transitions > 250ms)
        if (frameTimeMs <= 0.0 || frameTimeMs > 250.0)
            return _currentRatio;

        const float dt = (_lastTimeMs > 0.0) ? static_cast<float>((nowMs - _lastTimeMs) / 1000.0) : 0.016f;
        _lastTimeMs = nowMs;

        if (dt <= 0.0001f || dt > 0.5f)
            return _currentRatio;

        // Positive error means the frame took longer than target (framerate is too low) -> increase ratio to reduce pixels
        const float error = static_cast<float>(frameTimeMs) - targetFrameTime;

        // PID parameters calibrated for responsive resolution tracking
        constexpr float Kp = 0.015f;
        constexpr float Ki = 0.004f;
        constexpr float Kd = 0.002f;

        // Deadband of 0.3ms to prevent oscillation around target
        if (std::abs(error) > 0.3f)
        {
            _integral = std::clamp(_integral + error * dt, -50.0f, 50.0f);
            const float derivative = (error - _lastError) / dt;
            _lastError = error;

            const float deltaRatio = Kp * error + Ki * _integral + Kd * derivative;

            const float minRatio = std::clamp(config->GaqMinRatio.value_or_default(), 1.0f, 3.0f);
            const float maxRatio = std::clamp(config->GaqMaxRatio.value_or_default(), minRatio, 4.0f);

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

    float CurrentRatio() const { return _currentRatio; }

  private:
    float _currentRatio = 1.3f;
    float _integral = 0.0f;
    float _lastError = 0.0f;
    double _lastTimeMs = 0.0;
};
