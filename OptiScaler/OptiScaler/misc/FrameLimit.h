#pragma once
#include "SysUtils.h"

class FrameLimit
{
  public:
    enum class Mode : int
    {
        FrontEdgeJit = 0,
        FrontEdgeSync = 1,
        BackEdge = 2,
    };

    struct SyncStats
    {
        double detectedRefreshHz = 0.0;
        bool vblankLocked = false;
        bool displayTimingActive = false;
        bool displayTimingScheduleActive = false;
        bool presentWaitActive = false;
        bool simStartActive = false;
        bool earlyAcquireActive = false;
        bool gamescopeLimiterDetected = false;
        bool fgSubframePacingActive = false;
        int activeMaxInFlight = 0;
        uint32_t fgSubframeMultiplier = 0;
        uint64_t lastCpuWorkUs = 0;
        uint64_t lastGpuWorkUs = 0;
        uint64_t lastPresentMarginUs = 0;
        uint64_t lastTimerOvershootUs = 0;
    };

    static int sleepDurationNs(int64_t ns);

    static void registerWaitableHandle(HANDLE handle);
    static bool isTrackedWaitable(HANDLE handle);

    static void onSimulationStart();
    static void onVulkanAcquire(uint64_t acquireDurationNs, bool dxgiBacked = false);
    static void recordPresentDuration(uint64_t presentDurationNs);

    static void onFgGamePresentStart();
    static void onFgOutputPresent(bool fromVulkan = false, bool hwVsync = false);
    static bool isSubframePacingActive();

    static void prePresent(bool fgActive);
    static void postPresent(bool fgActive);
    static void sleep(bool fgActive);

    // The presented frame rate to limit to, 0 = none: the FPS limit, or XeFG's Dynamic MFG target while it runs with
    // "Use target as cap" when that is lower. Every limiter path (Reflex, XeLL, the low latency outputs, this one)
    // uses it.
    static float targetFps();
    static bool isLimitingActive();
    static Mode currentMode();
    static int maxInFlightPresents();

    static void markVulkanPresent();
    static bool hasRecentVulkanPresent();
    static void markDxgiPresent();
    static bool hasRecentDxgiPresent();

    static void recordScanoutTimestamp(uint64_t submitNs, uint64_t scanoutNs, uint64_t reportedRefreshNs,
                                       uint64_t presentMarginNs, bool isVsyncMode, bool fromDisplayTiming);
    static void calibrateDisplayClock(uint64_t displayActualNs, uint64_t scanoutQpcNs);
    static uint64_t getDesiredDisplayTimeNs(bool useActiveDxgiFrame = false);
    static uint64_t alignDeadlineToVBlank(uint64_t deadlineNs, uint64_t intervalNs);
    static void getRecentFrameTimes(float* outMs, int maxCount, int& outCount, float& outMinMs, float& outMaxMs);
    static SyncStats getStats();
};
