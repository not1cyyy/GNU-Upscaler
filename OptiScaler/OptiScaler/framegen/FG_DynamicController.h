#pragma once

#include <framegen/xefg/XeFG_DynamicCount.h>
#include <framegen/xefg/XeFG_DynamicCap.h>

#include <Windows.h>

// Shared Dynamic Frame Generation controller for XeFG, FSR-FG (DX12/Vulkan), and DLSSG.
// Modulates active frame generation multipliers and 2X <-> 1X fallback with hysteresis around
// FPS target, present/limiter waits, and the native Base FPS latency floor (MinBaseFps).
class FG_DynamicController
{
  public:
    struct Options
    {
        bool enabled = false;        // Dynamic FG active this frame
        bool useTargetAsCap = false; // Target FPS also caps framerate (XeFG_DynamicCap)
        bool allow1X = true;         // Allows dropping to 1X (generationOff = true)
        int targetCount = 1;         // Max/user interpolated frame count (1 = 2X)
        int currentCount = 1;        // Currently active interpolated count (>= 1 when on)
        bool generationOff = false;  // True when currently in 1X passthrough
        float targetFps = 0.0f;      // 0 = FramerateLimit or just under display refresh rate
        float minBaseFps = 45.0f;    // Native Base FPS latency floor (<= 0 disables)
        int rounding = 1;            // 0 = down, 1 = nearest, 2 = up
        double presentWaitMs = 0.0;  // Last measured Wait/Present stall
        HWND window = nullptr;
        bool updateCapState = false; // Publishes State::xefgDynamicCapFps when true
    };

    struct Result
    {
        bool dynamic = false;       // Dynamic controller ran this frame
        bool ownChange = false;     // Ran last frame too, so changes are automatic
        int count = 1;              // Interpolated frames to request (>= 1)
        bool generationOff = false; // True for 1X passthrough (FG suppressed cleanly)
    };

    Result Update(const Options& opt);
    bool EvaluateBaseFpsGuard(bool enabled, float minBaseFps);
    void Restart();

  protected:
    static int Focused(HWND window);
    float DisplayRefreshFps(double nowMs, HWND window);
    float EffectiveTargetFps(double nowMs, HWND window, float configuredTargetFps);

  private:
    XeFG_DynamicCount _count;
    XeFG_DynamicCap _cap;
    bool _active = false;
    bool _capActive = false;
    bool _adfgClamped = false;
    double _baseFrameMsEma = 0.0;
    double _logMs = 0.0;
    int _refreshRate = 0;
    double _refreshCheckedMs = 0.0;
    bool _unfocused = false;
};
