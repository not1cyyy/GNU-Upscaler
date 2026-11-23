#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include "State.h"
#include "Util.h"
#include <framegen/IFGFeature.h>
#include <low_latency/input/input_common.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define FL_CPU_PAUSE() _mm_pause()
#else
#define FL_CPU_PAUSE() YieldProcessor()
#endif

typedef NTSTATUS(NTAPI* PFN_NtDelayExecution)(BOOLEAN Alertable, PLARGE_INTEGER DelayInterval);
typedef NTSTATUS(NTAPI* PFN_NtSetTimerResolution)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef NTSTATUS(NTAPI* PFN_NtWaitForSingleObject)(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);

namespace
{
constexpr int64_t kBusywaitThresholdNs = 250'000; // 0.25ms hybrid tail absorbs Linux timer slack without core pinning
constexpr uint64_t kPauseRebaseGapNs = 1'000'000'000ULL;
constexpr uint64_t kMinValidRefreshNs = 1'000'000ULL;   // 1000 Hz
constexpr uint64_t kMaxValidRefreshNs = 100'000'000ULL; // 10 Hz
constexpr int64_t kMaxPllNudgePerFrameNs = 150'000;     // 0.15ms max phase slew per frame

std::atomic<int64_t> g_timerOvershootEmaNs { 200'000 }; // Adaptive OS wakeup slack reserve (100us..1.2ms)

inline void EnsureHighResTimerResolution()
{
    static std::once_flag s_timerResOnce;
    std::call_once(s_timerResOnce,
                   []()
                   {
                       if (HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll"))
                       {
                           if (auto pfnSetTimerRes = reinterpret_cast<PFN_NtSetTimerResolution>(
                                   GetProcAddress(hNtdll, "NtSetTimerResolution")))
                           {
                               ULONG actualResolution = 0;
                               pfnSetTimerRes(5000 /* 0.5ms */, TRUE, &actualResolution);
                           }
                       }
                   });
}

inline int TimerSleep(int64_t hundredNs)
{
    if (hundredNs <= 0)
        return 0;

    EnsureHighResTimerResolution();

    static PFN_NtDelayExecution pfnNtDelayExecution = []() -> PFN_NtDelayExecution
    {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        return hNtdll ? reinterpret_cast<PFN_NtDelayExecution>(GetProcAddress(hNtdll, "NtDelayExecution")) : nullptr;
    }();

    LARGE_INTEGER dueTime;
    dueTime.QuadPart = -hundredNs;

    // Under Wine/Proton, NtDelayExecution maps directly to Linux nanosleep/futex without wineserver timer objects
    if (pfnNtDelayExecution != nullptr)
    {
        if (pfnNtDelayExecution(FALSE, &dueTime) == 0)
            return 0;
    }

    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    if (!timer)
        return 1;

    if (!SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
}

inline int BusywaitSleep(int64_t ns)
{
    if (ns <= 0)
        return 0;

    const uint64_t waitUntil = Util::GetTimestamp() + static_cast<uint64_t>(ns);
    while (Util::GetTimestamp() < waitUntil)
        FL_CPU_PAUSE();

    return 0;
}

// Leaf lock: never acquires g_pacerMutex or g_pllMutex
std::mutex g_waitableMutex;
std::vector<HANDLE> g_waitables;
std::atomic<bool> g_hasWaitables { false };

// Leaf lock: never acquires g_pacerMutex
std::mutex g_pllMutex;

struct VBlankPllState
{
    uint64_t refreshPeriodNs = 0;
    uint64_t scanoutPeriodNs = 0;
    uint64_t anchorQpcNs = 0;
    uint64_t displayAnchorNs = 0;
    uint64_t lastWaitScanoutQpcNs = 0;
    uint64_t lastWaitSubmitQpcNs = 0;
    uint64_t lastDisplayTimingNs = 0;
    uint64_t lastPresentMarginNs = 0;

    double phaseResidualEmaNs = 0.0;
    double latencyVarianceEmaNs = 0.0;
    double lastLatencyNs = 0.0;
    double gpuLatencyEmaNs = 0.0;
    double gamescopeBlockEmaNs = 0.0;

    // QPC -> Display CLOCK_MONOTONIC calibration for VK_GOOGLE_display_timing desiredPresentTime
    int64_t qpcToDisplayOffsetNs = 0;
    uint32_t offsetStableCount = 0;
    bool displayClockCalibrated = false;
    bool displayTimingScheduleActive = false;

    std::array<uint64_t, 16> recentDeltas {};
    size_t deltaWriteIdx = 0;
    size_t deltaCount = 0;

    uint32_t stableSamples = 0;
    uint32_t missedTargetStreak = 0;
    uint32_t lowGpuStreak = 0;
    uint32_t maxInFlightCooldown = 0;
    uint32_t gamescopeExternalStreak = 0;

    bool vblankLocked = false;
    bool displayTimingActive = false;
    bool presentWaitActive = false;
} g_pll;

std::mutex g_pacerMutex;

struct PacerState
{
    uint64_t nextTargetNs = 0;
    uint64_t currentFrameTargetNs = 0;
    uint64_t lastFrontEdgeNs = 0;
    uint64_t lastPresentFinishNs = 0;
    uint64_t lastWakeNs = 0;
    uint64_t lastIntervalNs = 0;
    double cpuWorkEmaNs = 0.0;
    double cpuJitterEmaNs = 0.0;
    double simSleptMs = 0.0;
    double preSleptMs = 0.0;
    bool lastFgActive = false;
    int lastMode = -1;

    // Pass 2: true engine simulation-start & early-acquire state
    bool simStartActive = false;
    bool simStartPending = false;
    uint32_t simStartMissedFrames = 0;
    uint32_t earlyAcquireStreak = 0;
    bool earlyAcquireActive = false;

    // Pass 3: paced frametime ring buffer for overlay mini-graph
    std::array<float, 120> frameTimesMs {};
    size_t frameTimeHead = 0;
    size_t frameTimeCount = 0;
} g_pacer;

std::atomic<uint64_t> g_lastVulkanPresentNs { 0 };
std::atomic<uint64_t> g_lastDxgiPresentNs { 0 };
std::atomic<uint64_t> g_activeIntervalNs { 0 };
std::atomic<int> g_dynamicMaxInFlight { 2 };
std::atomic<bool> g_gamescopeLimiterDetected { false };
std::atomic<uint64_t> g_gamescopeScanoutPeriodNs { 0 };
std::atomic<uint64_t> g_lastSleepWholeUs { 0 };

// Leaf lock: never acquires g_pacerMutex or g_pllMutex
std::mutex g_fgSubMutex;

struct FgOpenLoopState
{
    uint64_t nextTargetNs = 0;
    uint64_t lastOutputEdgeNs = 0;
    uint32_t activeMult = 0;

    std::array<float, 120> frameTimesMs {};
    size_t frameTimeHead = 0;
    size_t frameTimeCount = 0;
} g_fgSub;

std::atomic<DWORD> g_fgGameThreadId { 0 };
std::atomic<uint64_t> g_fgEpoch { 0 };
std::atomic<uint64_t> g_lastFgVkOutputNs { 0 };
std::atomic<bool> g_fgSubframeActive { false };

uint32_t ActiveSubframeMultiplier()
{
    const int mode = std::clamp(Config::Instance()->FramerateLimitFgSubframeMode.value_or_default(), 0, 2);
    if (mode == 0)
        return 0;

    auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::XeFG)
        return 0;

    auto fg = state.CurrentFG();
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
        return 0;

    const uint32_t mult = static_cast<uint32_t>(fg->GetInterpolatedFrameCount()) + 1u;
    const uint32_t minMult = (mode >= 2) ? 2u : 3u;
    return (mult >= minMult) ? mult : 0u;
}

HANDLE GetFgWakeEvent()
{
    static HANDLE s_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return s_event;
}

void ResetFgSubframeLocked()
{
    g_fgSub.nextTargetNs = 0;
    g_fgSub.lastOutputEdgeNs = 0;
    g_fgSub.activeMult = 0;
    if (g_fgSubframeActive.exchange(false, std::memory_order_release))
    {
        if (HANDLE ev = GetFgWakeEvent())
            SetEvent(ev);
    }
}

void PushFrameTimeMs(std::array<float, 120>& ring, size_t& head, size_t& count, uint64_t prevNs, uint64_t curNs)
{
    if (prevNs == 0 || curNs <= prevNs)
        return;

    const float frameMs = static_cast<float>(curNs - prevNs) / 1e6f;
    if (frameMs > 0.1f && frameMs < 1000.0f)
    {
        ring[head] = frameMs;
        head = (head + 1) % ring.size();
        count = std::min(count + 1, ring.size());
    }
}

void CopyRingFrameTimes(const std::array<float, 120>& ring, size_t head, size_t count, float* outMs, int maxCount,
                        int& outCount, float& outMinMs, float& outMaxMs)
{
    const size_t available = std::min(count, static_cast<size_t>(maxCount));
    if (available == 0)
        return;

    const size_t cap = ring.size();
    const size_t start = (head + cap - available) % cap;

    float minV = FLT_MAX;
    float maxV = 0.0f;
    for (size_t i = 0; i < available; ++i)
    {
        const float v = ring[(start + i) % cap];
        outMs[i] = v;
        minV = std::min(minV, v);
        maxV = std::max(maxV, v);
    }

    outCount = static_cast<int>(available);
    outMinMs = minV;
    outMaxMs = maxV;
}

uint64_t ComputeIntervalNs(bool fgActive)
{
    const float fpsCap = FrameLimit::targetFps();
    if (fpsCap <= 0.0f)
        return 0;

    double intervalNs = std::clamp(1'000'000'000.0 / static_cast<double>(fpsCap), 100'000.0, 100'000'000'000.0);

    // The limit counts presented frames; native batch FG outputs multiple frames per real present
    if (fgActive)
    {
        auto fg = State::Instance().CurrentFG();
        intervalNs *= fg != nullptr ? static_cast<double>(fg->GetInterpolatedFrameCount() + 1) : 2.0;

        bool vblankLocked = false;
        {
            std::scoped_lock lock(g_pllMutex);
            vblankLocked = g_pll.vblankLocked;
        }

        // Only subtract Gamescope headroom when Gamescope's own limiter matches our interval
        const uint64_t gsPeriod = g_gamescopeScanoutPeriodNs.load(std::memory_order_relaxed);
        const bool gamescopeMatchesCap =
            g_gamescopeLimiterDetected.load(std::memory_order_relaxed) && gsPeriod > 0 &&
            std::abs(static_cast<double>(gsPeriod) - intervalNs) <= intervalNs * 0.03;

        if (State::Instance().isGamescopeActive && Config::Instance()->FGXeFGPaceOnSwapchain.value_or(true) &&
            !vblankLocked && gamescopeMatchesCap && intervalNs > 500'000.0)
        {
            intervalNs -= 500'000.0;
        }
    }

    return static_cast<uint64_t>(std::llround(intervalNs));
}

int64_t SignedPhaseError(uint64_t timestampNs, uint64_t anchorNs, uint64_t periodNs)
{
    if (periodNs == 0)
        return 0;

    int64_t diff = static_cast<int64_t>(timestampNs - anchorNs);
    const int64_t period = static_cast<int64_t>(periodNs);
    int64_t mod = diff % period;
    if (mod < 0)
        mod += period;
    if (mod > period / 2)
        mod -= period;

    return mod;
}

uint64_t EstimateStableRefreshFromDeltas(const VBlankPllState& pll)
{
    if (pll.deltaCount < 6)
        return 0;

    std::array<uint64_t, 16> sorted {};
    sorted.fill(UINT64_MAX);
    const size_t count = std::min(pll.deltaCount, pll.recentDeltas.size());
    for (size_t i = 0; i < count; ++i)
        sorted[i] = pll.recentDeltas[i];

    std::sort(sorted.begin(), sorted.end());

    // Use lower-quartile cluster to avoid missed-VBlank 2x spikes skewing the base period
    const uint64_t candidate = sorted[count / 4];
    if (candidate < kMinValidRefreshNs || candidate > kMaxValidRefreshNs)
        return 0;

    size_t consistent = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const double ratio = static_cast<double>(sorted[i]) / static_cast<double>(candidate);
        const double nearest = std::round(ratio);
        if (nearest >= 1.0 && std::abs(ratio - nearest) < 0.03)
            ++consistent;
    }

    return (consistent * 4 >= count * 3) ? candidate : 0;
}

uint64_t EstimateMedianDelta(const VBlankPllState& pll)
{
    if (pll.deltaCount < 6)
        return 0;

    std::array<uint64_t, 16> sorted {};
    sorted.fill(UINT64_MAX);
    const size_t count = std::min(pll.deltaCount, pll.recentDeltas.size());
    for (size_t i = 0; i < count; ++i)
        sorted[i] = pll.recentDeltas[i];

    std::sort(sorted.begin(), sorted.end());
    return sorted[count / 2];
}

uint64_t ApplyVBlankPhaseNudge(uint64_t deadlineNs, uint64_t intervalNs, bool snapHard)
{
    if (deadlineNs == 0 || intervalNs == 0)
        return deadlineNs;

    uint64_t refreshNs = 0;
    uint64_t anchorNs = 0;
    double gpuLatencyNs = 0.0;
    bool locked = false;

    {
        std::scoped_lock lock(g_pllMutex);
        refreshNs = g_pll.refreshPeriodNs;
        anchorNs = g_pll.anchorQpcNs;
        gpuLatencyNs = g_pll.gpuLatencyEmaNs;
        locked = g_pll.vblankLocked;
    }

    if (!locked || refreshNs == 0 || anchorNs == 0)
        return deadlineNs;

    // Only phase-lock when target interval is commensurate with display refresh (e.g. 60@60, 60@120, 40@120)
    const double ratio = static_cast<double>(intervalNs) / static_cast<double>(refreshNs);
    const double mult = std::round(ratio);
    if (mult < 1.0 || std::abs(ratio - mult) > 0.025)
        return deadlineNs;

    const float marginMs = std::clamp(Config::Instance()->FramerateLimitSyncMarginMs.value_or_default(), 0.0f, 5.0f);
    uint64_t marginNs = static_cast<uint64_t>(marginMs * 1'000'000.0);

    // Pass 3: When strictly 1-in-flight with ample GPU headroom, pull Present lead ahead by GPU render duration
    // so both CPU and GPU complete within the same refresh window
    if (FrameLimit::maxInFlightPresents() == 1 && gpuLatencyNs > 0.0 &&
        gpuLatencyNs < static_cast<double>(intervalNs) * 0.65)
    {
        marginNs += static_cast<uint64_t>(gpuLatencyNs);
    }

    const uint64_t desiredAnchorNs = (anchorNs > marginNs) ? (anchorNs - marginNs) : anchorNs;

    const int64_t err = SignedPhaseError(deadlineNs, desiredAnchorNs, refreshNs);
    if (snapHard)
    {
        if (err > 0 && static_cast<uint64_t>(err) < deadlineNs)
            return deadlineNs - static_cast<uint64_t>(err);
        if (err < 0)
            return deadlineNs + static_cast<uint64_t>(-err);
        return deadlineNs;
    }

    const int64_t nudge = std::clamp(err / 8, -kMaxPllNudgePerFrameNs, kMaxPllNudgePerFrameNs);
    if (nudge > 0 && static_cast<uint64_t>(nudge) < deadlineNs)
        return deadlineNs - static_cast<uint64_t>(nudge);
    if (nudge < 0)
        return deadlineNs + static_cast<uint64_t>(-nudge);

    return deadlineNs;
}

void UpdateCpuWorkEma(PacerState& pacer, uint64_t sampleNs, uint64_t intervalNs)
{
    if (sampleNs == 0 || intervalNs == 0 || sampleNs >= intervalNs * 2)
        return;

    const double sample = static_cast<double>(sampleNs);
    if (pacer.cpuWorkEmaNs <= 0.0)
    {
        pacer.cpuWorkEmaNs = sample;
        return;
    }

    // Fast attack so CPU spikes don't miss Present deadline, slow decay for smooth frame times
    const double alpha = (sample > pacer.cpuWorkEmaNs) ? 0.35 : 0.08;
    const double err = std::abs(sample - pacer.cpuWorkEmaNs);
    pacer.cpuWorkEmaNs = (1.0 - alpha) * pacer.cpuWorkEmaNs + alpha * sample;
    pacer.cpuJitterEmaNs = 0.85 * pacer.cpuJitterEmaNs + 0.15 * err;
}

void ResetPacer(PacerState& pacer)
{
    pacer.nextTargetNs = 0;
    pacer.currentFrameTargetNs = 0;
    pacer.lastFrontEdgeNs = 0;
    pacer.lastPresentFinishNs = 0;
    pacer.lastWakeNs = 0;
    pacer.lastIntervalNs = 0;
    pacer.cpuWorkEmaNs = 0.0;
    pacer.cpuJitterEmaNs = 0.0;
    pacer.simSleptMs = 0.0;
    pacer.preSleptMs = 0.0;
    pacer.lastFgActive = false;
    pacer.lastMode = -1;
    pacer.simStartActive = false;
    pacer.simStartPending = false;
    pacer.simStartMissedFrames = 0;
    pacer.earlyAcquireStreak = 0;
    pacer.earlyAcquireActive = false;
    g_activeIntervalNs.store(0, std::memory_order_relaxed);
    g_dynamicMaxInFlight.store(2, std::memory_order_relaxed);
    g_gamescopeLimiterDetected.store(false, std::memory_order_relaxed);
    g_gamescopeScanoutPeriodNs.store(0, std::memory_order_relaxed);
    g_lastSleepWholeUs.store(0, std::memory_order_relaxed);
}

bool ShouldYieldToGamescopeLimiter(uint64_t intervalNs)
{
    if (!State::Instance().isGamescopeActive || !g_gamescopeLimiterDetected.load(std::memory_order_relaxed))
        return false;

    const uint64_t gsPeriod = g_gamescopeScanoutPeriodNs.load(std::memory_order_relaxed);
    return gsPeriod > 0 && intervalNs > 0 && static_cast<double>(gsPeriod) > static_cast<double>(intervalNs) * 1.03;
}

uint64_t ComputeJitWakeupNs(const PacerState& pacer, uint64_t intervalNs)
{
    const double jitterMargin = std::clamp(pacer.cpuJitterEmaNs * 2.0, 300'000.0, 1'500'000.0);
    const double rawLead = pacer.cpuWorkEmaNs > 0.0 ? (pacer.cpuWorkEmaNs + jitterMargin) : 500'000.0;
    const uint64_t leadNs = static_cast<uint64_t>(std::clamp(rawLead, 0.0, static_cast<double>(intervalNs) * 0.85));
    return (pacer.nextTargetNs > leadNs) ? (pacer.nextTargetNs - leadNs) : 0ULL;
}

void UpdateGamescopeDetectorLocked(uint64_t blockNs)
{
    if (!State::Instance().isGamescopeActive || blockNs == 0)
        return;

    const double sample = static_cast<double>(std::min(blockNs, kMaxValidRefreshNs));
    g_pll.gamescopeBlockEmaNs =
        (g_pll.gamescopeBlockEmaNs <= 0.0) ? sample : (0.85 * g_pll.gamescopeBlockEmaNs + 0.15 * sample);

    const uint64_t sleptUs = g_lastSleepWholeUs.load(std::memory_order_relaxed);
    const uint64_t intervalNs = g_activeIntervalNs.load(std::memory_order_relaxed);

    if (intervalNs > 0 && g_pll.gamescopeBlockEmaNs > 2'000'000.0 &&
        (sleptUs < 1500 || g_pll.gamescopeBlockEmaNs > 4'000'000.0))
    {
        g_pll.gamescopeExternalStreak = std::min(g_pll.gamescopeExternalStreak + 1, 32u);
        if (g_pll.gamescopeExternalStreak >= 10)
            g_gamescopeLimiterDetected.store(true, std::memory_order_relaxed);
    }
    else if (g_pll.gamescopeBlockEmaNs < 1'000'000.0 || (sleptUs > 1500 && g_pll.gamescopeBlockEmaNs < 3'000'000.0))
    {
        if (g_pll.gamescopeExternalStreak > 0)
            --g_pll.gamescopeExternalStreak;
        if (g_pll.gamescopeExternalStreak == 0)
            g_gamescopeLimiterDetected.store(false, std::memory_order_relaxed);
    }
}

void UpdateTimerOvershootEma(int64_t requestedSleepNs, int64_t actualSleepNs, int64_t overshootReserve)
{
    const int64_t sampleOvershoot =
        (actualSleepNs > requestedSleepNs)
            ? std::clamp<int64_t>(actualSleepNs - requestedSleepNs, 0, 1'500'000)
            : 0;
    int64_t ema = overshootReserve;
    ema = (sampleOvershoot > ema) ? ((ema * 3 + sampleOvershoot) / 4) : ((ema * 19 + sampleOvershoot) / 20);
    g_timerOvershootEmaNs.store(std::clamp<int64_t>(ema, 80'000, 750'000), std::memory_order_relaxed);
}

int SleepStaggerUntilNs(uint64_t deadlineNs, uint64_t startEpoch)
{
    int status = 0;
    auto shouldAbort = [&]() -> bool
    {
        if (State::Instance().isShuttingDown || !g_fgSubframeActive.load(std::memory_order_acquire))
            return true;

        const uint64_t curEpoch = g_fgEpoch.load(std::memory_order_acquire);
        return curEpoch >= startEpoch && (curEpoch - startEpoch) >= 2ULL;
    };

    static PFN_NtWaitForSingleObject pfnNtWaitForSingleObject = []() -> PFN_NtWaitForSingleObject
    {
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        return hNtdll ? reinterpret_cast<PFN_NtWaitForSingleObject>(GetProcAddress(hNtdll, "NtWaitForSingleObject"))
                      : nullptr;
    }();
    const HANDLE wakeEvent = GetFgWakeEvent();

    while (!shouldAbort())
    {
        const uint64_t now = Util::GetTimestamp();
        if (now >= deadlineNs)
            break;

        const int64_t remaining = static_cast<int64_t>(deadlineNs - now);
        const int64_t overshootReserve = g_timerOvershootEmaNs.load(std::memory_order_relaxed);
        const int64_t tailNs = kBusywaitThresholdNs + overshootReserve;

        if (remaining <= tailNs + 1'000)
        {
            while (Util::GetTimestamp() < deadlineNs && !shouldAbort())
                FL_CPU_PAUSE();
            break;
        }

        const int64_t requestedSleepNs = remaining - tailNs;
        if (wakeEvent != nullptr && pfnNtWaitForSingleObject != nullptr)
        {
            ResetEvent(wakeEvent);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (shouldAbort())
                break;

            constexpr NTSTATUS kStatusTimeout = 0x00000102L;
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -(requestedSleepNs / 100);
            const uint64_t timerStart = Util::GetTimestamp();
            const NTSTATUS waitRes = pfnNtWaitForSingleObject(wakeEvent, FALSE, &dueTime);
            if (waitRes == kStatusTimeout)
            {
                const int64_t actualSleepNs = static_cast<int64_t>(Util::GetTimestamp() - timerStart);
                UpdateTimerOvershootEma(requestedSleepNs, actualSleepNs, overshootReserve);
            }
            else if (waitRes < 0)
            {
                status = 1;
                break;
            }
        }
        else
        {
            const int64_t chunkSleepNs = std::min<int64_t>(requestedSleepNs, 3'500'000);
            const uint64_t timerStart = Util::GetTimestamp();
            status = TimerSleep(chunkSleepNs / 100);
            if (status != 0)
                break;

            if (!shouldAbort())
            {
                const int64_t actualSleepNs = static_cast<int64_t>(Util::GetTimestamp() - timerStart);
                UpdateTimerOvershootEma(chunkSleepNs, actualSleepNs, overshootReserve);
            }
        }
    }

    return status;
}

void UpdatePllPhaseLocked(uint64_t& anchorNs, uint64_t sampleNs, uint64_t periodNs)
{
    if (periodNs == 0 || sampleNs == 0)
        return;

    if (anchorNs == 0 || sampleNs < anchorNs || (sampleNs - anchorNs) > kPauseRebaseGapNs)
    {
        anchorNs = sampleNs;
        g_pll.phaseResidualEmaNs = 0.0;
        g_pll.stableSamples = 0;
        return;
    }

    const int64_t err = SignedPhaseError(sampleNs, anchorNs, periodNs);
    g_pll.phaseResidualEmaNs = 0.85 * g_pll.phaseResidualEmaNs + 0.15 * static_cast<double>(std::abs(err));

    const int64_t step = err / 8;
    if (step > 0)
        anchorNs += static_cast<uint64_t>(step);
    else if (step < 0 && static_cast<uint64_t>(-step) < anchorNs)
        anchorNs -= static_cast<uint64_t>(-step);

    if (std::abs(err) < 800'000)
        g_pll.stableSamples = std::min(g_pll.stableSamples + 1, 64u);
    else if (g_pll.stableSamples > 0)
        --g_pll.stableSamples;
}

void UpdateMaxInFlightLocked(uint64_t dt, bool isVsyncMode)
{
    const uint64_t activeIntervalNs = g_activeIntervalNs.load(std::memory_order_relaxed);
    if (Config::Instance()->FramerateLimitMaxInFlight.value_or_default() != 0 || activeIntervalNs == 0)
        return;

    if (dt > activeIntervalNs + (activeIntervalNs / 12))
        g_pll.missedTargetStreak = std::min(g_pll.missedTargetStreak + 1, 16u);
    else
        g_pll.missedTargetStreak = 0;

    if (isVsyncMode && g_pll.vblankLocked)
    {
        // Keep 2 in-flight on fixed-refresh VSync so backpressure never skips a VBlank
        g_dynamicMaxInFlight.store(2, std::memory_order_relaxed);
        g_pll.lowGpuStreak = 0;
        return;
    }

    const bool headroomLow =
        (g_pll.displayTimingActive && g_pll.lastPresentMarginNs > 0)
            ? (g_pll.lastPresentMarginNs < activeIntervalNs / 5)
            : (g_pll.gpuLatencyEmaNs > static_cast<double>(activeIntervalNs) * 0.72);
    const bool headroomHigh =
        (g_pll.displayTimingActive && g_pll.lastPresentMarginNs > 0)
            ? (g_pll.lastPresentMarginNs > (activeIntervalNs * 2) / 5)
            : (g_pll.gpuLatencyEmaNs > 0.0 &&
               g_pll.gpuLatencyEmaNs < static_cast<double>(activeIntervalNs) * 0.55);

    const int currentMax = g_dynamicMaxInFlight.load(std::memory_order_relaxed);
    if (currentMax == 1)
    {
        if (headroomLow || g_pll.missedTargetStreak >= 2)
        {
            g_dynamicMaxInFlight.store(2, std::memory_order_relaxed);
            g_pll.maxInFlightCooldown = 60;
            g_pll.lowGpuStreak = 0;
        }
        return;
    }

    if (g_pll.maxInFlightCooldown > 0)
    {
        --g_pll.maxInFlightCooldown;
    }
    else if (headroomHigh && g_pll.missedTargetStreak == 0)
    {
        if (++g_pll.lowGpuStreak >= 24)
        {
            g_dynamicMaxInFlight.store(1, std::memory_order_relaxed);
            g_pll.lowGpuStreak = 0;
        }
    }
    else
    {
        g_pll.lowGpuStreak = 0;
    }
}
} // namespace

int FrameLimit::sleepDurationNs(int64_t ns)
{
    if (ns <= 0)
        return 0;

    int status = 0;
    const uint64_t start = Util::GetTimestamp();
    const int64_t overshootReserve = g_timerOvershootEmaNs.load(std::memory_order_relaxed);
    const int64_t tailNs = kBusywaitThresholdNs + overshootReserve;

    if (ns <= tailNs)
    {
        status = BusywaitSleep(ns);
    }
    else
    {
        const int64_t requestedSleepNs = ns - tailNs;
        const uint64_t timerStart = Util::GetTimestamp();
        status = TimerSleep(requestedSleepNs / 100);
        const int64_t actualSleepNs = static_cast<int64_t>(Util::GetTimestamp() - timerStart);

        if (status == 0)
            UpdateTimerOvershootEma(requestedSleepNs, actualSleepNs, overshootReserve);
    }

    // Fine-grained busy wait completes the remaining sub-millisecond precision
    const int64_t remaining = ns - static_cast<int64_t>(Util::GetTimestamp() - start);
    if (remaining > 0)
        BusywaitSleep(remaining);

    return status;
}

void FrameLimit::registerWaitableHandle(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return;

    std::scoped_lock lock(g_waitableMutex);
    if (std::ranges::find(g_waitables, handle) == g_waitables.end())
    {
        if (g_waitables.size() >= 16)
            g_waitables.erase(g_waitables.begin());
        g_waitables.push_back(handle);
    }
    g_hasWaitables.store(true, std::memory_order_release);
}

bool FrameLimit::isTrackedWaitable(HANDLE handle)
{
    if (!g_hasWaitables.load(std::memory_order_acquire) || handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;

    std::scoped_lock lock(g_waitableMutex);
    return std::ranges::find(g_waitables, handle) != g_waitables.end();
}

void FrameLimit::onSimulationStart()
{
    if (!isLimitingActive() || currentMode() != Mode::FrontEdgeJit)
        return;

    // When FG is active, let the FG presenter/batch limiter own pacing
    if (auto fg = State::Instance().CurrentFG(); fg != nullptr && fg->IsActive() && !fg->IsPaused())
        return;

    const uint64_t intervalNs = ComputeIntervalNs(false);
    if (intervalNs == 0)
        return;

    int64_t sleepNs = 0;
    {
        std::scoped_lock lock(g_pacerMutex);
        g_pacer.simStartActive = true;
        g_pacer.simStartMissedFrames = 0;

        if (!g_pacer.simStartPending || g_pacer.nextTargetNs == 0 || ShouldYieldToGamescopeLimiter(intervalNs))
        {
            g_pacer.simStartPending = false;
            g_pacer.lastWakeNs = Util::GetTimestamp();
            return;
        }

        g_pacer.simStartPending = false;
        const uint64_t now = Util::GetTimestamp();
        const uint64_t wakeupNs = ComputeJitWakeupNs(g_pacer, intervalNs);
        if (wakeupNs > now && (wakeupNs - now) <= intervalNs * 2)
            sleepNs = static_cast<int64_t>(wakeupNs - now);
    }

    double sleptMs = 0.0;
    if (sleepNs > 0)
    {
        const uint64_t sleepStart = Util::GetTimestamp();
        if (const int res = sleepDurationNs(sleepNs); res != 0)
        {
            static bool errorLogged = false;
            if (!errorLogged)
            {
                LOG_ERROR("Sleep command failed: {}", res);
                errorLogged = true;
            }
        }
        const double measuredMs = static_cast<double>(Util::GetTimestamp() - sleepStart) / 1e6;
        sleptMs = measuredMs > 0.0 ? measuredMs : 0.0;
    }

    {
        std::scoped_lock lock(g_pacerMutex);
        g_pacer.lastWakeNs = Util::GetTimestamp();
        g_pacer.simSleptMs += sleptMs;
    }
}

void FrameLimit::onVulkanAcquire(uint64_t acquireDurationNs, bool dxgiBacked)
{
    if (acquireDurationNs > 0 && State::Instance().isGamescopeActive)
    {
        std::scoped_lock lock(g_pllMutex);
        UpdateGamescopeDetectorLocked(acquireDurationNs);
    }

    // For DXVK/VKD3D-Proton, vkAcquireNextImageKHR runs on the translation backend thread, not the game's simulation thread
    if (dxgiBacked || hasRecentDxgiPresent())
        return;

    if (!isLimitingActive() || currentMode() != Mode::FrontEdgeJit)
        return;

    if (auto fg = State::Instance().CurrentFG(); fg != nullptr && fg->IsActive() && !fg->IsPaused())
        return;

    const uint64_t intervalNs = ComputeIntervalNs(false);
    if (intervalNs == 0)
        return;

    bool triggerSimStart = false;
    {
        std::scoped_lock lock(g_pacerMutex);
        if (g_pacer.lastPresentFinishNs == 0)
            return;

        const uint64_t now = Util::GetTimestamp();
        const uint64_t sincePresent = (now > g_pacer.lastPresentFinishNs) ? (now - g_pacer.lastPresentFinishNs) : 0ULL;

        // Early-acquire engines acquire the next swapchain image right after Present (~<35% of frame budget)
        if (sincePresent < (intervalNs * 35) / 100 && sincePresent < 6'000'000ULL)
        {
            g_pacer.earlyAcquireStreak = std::min(g_pacer.earlyAcquireStreak + 1, 16u);
            if (g_pacer.earlyAcquireStreak >= 6)
                g_pacer.earlyAcquireActive = true;
        }
        else if (sincePresent > (intervalNs * 45) / 100)
        {
            g_pacer.earlyAcquireStreak = 0;
            g_pacer.earlyAcquireActive = false;
        }

        triggerSimStart = g_pacer.earlyAcquireActive;
    }

    if (triggerSimStart)
        onSimulationStart();
}

void FrameLimit::recordPresentDuration(uint64_t presentDurationNs)
{
    if (presentDurationNs == 0 || !State::Instance().isGamescopeActive)
        return;

    std::scoped_lock lock(g_pllMutex);
    UpdateGamescopeDetectorLocked(presentDurationNs);
}

void FrameLimit::onFgGamePresentStart()
{
    g_fgGameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_fgEpoch.fetch_add(1ULL, std::memory_order_seq_cst);
    if (g_fgSubframeActive.load(std::memory_order_acquire))
    {
        if (HANDLE ev = GetFgWakeEvent())
            SetEvent(ev);
    }
}

void FrameLimit::onFgOutputPresent(bool fromVulkan, bool hwVsync)
{
    if (fromVulkan)
    {
        g_lastFgVkOutputNs.store(Util::GetTimestamp(), std::memory_order_relaxed);
    }
    else
    {
        const uint64_t lastVk = g_lastFgVkOutputNs.load(std::memory_order_relaxed);
        const uint64_t nowCheck = Util::GetTimestamp();
        if (lastVk > 0 && nowCheck >= lastVk && (nowCheck - lastVk) < 2'000'000'000ULL)
            return;
    }

    const uint32_t mult = ActiveSubframeMultiplier();
    const float capFps = targetFps();
    if (mult == 0 || capFps <= 0.0f)
    {
        if (g_fgSubframeActive.load(std::memory_order_relaxed))
        {
            std::scoped_lock lock(g_fgSubMutex);
            ResetFgSubframeLocked();
        }
        return;
    }

    g_fgSubframeActive.store(true, std::memory_order_release);

    const DWORD curTid = GetCurrentThreadId();
    const DWORD gameTid = g_fgGameThreadId.load(std::memory_order_relaxed);
    if (gameTid == 0 || curTid == gameTid)
    {
        // Never block the game's DX12 thread
        return;
    }

    uint64_t deadlineNs = 0;
    const uint64_t startEpoch = g_fgEpoch.load(std::memory_order_acquire);
    {
        std::scoped_lock lock(g_fgSubMutex);
        const uint64_t now = Util::GetTimestamp();
        const double capSubNs = std::clamp(1'000'000'000.0 / static_cast<double>(capFps), 100'000.0, 100'000'000.0);
        // Fixed open-loop Virtual VSync tick slightly ahead of target frame rate (0.995 * T_cap) to prevent
        // queue stalls without overshooting the FPS cap
        const uint64_t stepNs =
            std::min<uint64_t>(static_cast<uint64_t>(std::llround(capSubNs * 0.995)), 50'000'000ULL);
        g_fgSub.activeMult = mult;

        if (!hwVsync)
        {
            if (g_fgSub.nextTargetNs > now && (g_fgSub.nextTargetNs - now) <= stepNs * 2)
                deadlineNs = std::min(g_fgSub.nextTargetNs, now + stepNs);
            else
                g_fgSub.nextTargetNs = now;

            const uint64_t effectiveEdge = (deadlineNs > now) ? deadlineNs : now;
            g_fgSub.nextTargetNs = effectiveEdge + stepNs;
        }
        else
        {
            g_fgSub.nextTargetNs = 0;
        }
    }

    if (deadlineNs > 0)
    {
        if (const int res = SleepStaggerUntilNs(deadlineNs, startEpoch); res != 0)
        {
            static bool errorLogged = false;
            if (!errorLogged)
            {
                LOG_ERROR("XeFG virtual vsync sleep failed: {}", res);
                errorLogged = true;
            }
        }
    }

    {
        std::scoped_lock lock(g_fgSubMutex);
        const uint64_t edgeNow = Util::GetTimestamp();
        PushFrameTimeMs(g_fgSub.frameTimesMs, g_fgSub.frameTimeHead, g_fgSub.frameTimeCount, g_fgSub.lastOutputEdgeNs,
                        edgeNow);
        g_fgSub.lastOutputEdgeNs = edgeNow;
    }
}

bool FrameLimit::isSubframePacingActive()
{
    return ActiveSubframeMultiplier() > 0 && targetFps() > 0.0f &&
           g_fgSubframeActive.load(std::memory_order_relaxed);
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

bool FrameLimit::isLimitingActive()
{
    return targetFps() > 0.0f && !State::Instance().reflexLimitsFps && !InputCommon::can_limit_fps();
}

FrameLimit::Mode FrameLimit::currentMode()
{
    const int raw = std::clamp(Config::Instance()->FramerateLimitMode.value_or_default(), 0, 2);
    return static_cast<Mode>(raw);
}

int FrameLimit::maxInFlightPresents()
{
    // Never throttle swapchain queue depth while Frame Generation is actively buffering/interpolating frames
    if (auto fg = State::Instance().CurrentFG(); fg != nullptr && fg->IsActive() && !fg->IsPaused())
        return 0;

    const int configured = Config::Instance()->FramerateLimitMaxInFlight.value_or_default();
    if (configured < 0)
        return 0;

    if (configured > 0)
        return std::clamp(configured, 1, 4);

    if (!isLimitingActive())
        return 0;

    return g_dynamicMaxInFlight.load(std::memory_order_relaxed);
}

void FrameLimit::markVulkanPresent()
{
    g_lastVulkanPresentNs.store(Util::GetTimestamp(), std::memory_order_relaxed);
}

bool FrameLimit::hasRecentVulkanPresent()
{
    const uint64_t last = g_lastVulkanPresentNs.load(std::memory_order_relaxed);
    if (last == 0)
        return false;

    const uint64_t now = Util::GetTimestamp();
    return now >= last && (now - last) < 2'000'000'000ULL;
}

void FrameLimit::markDxgiPresent()
{
    g_lastDxgiPresentNs.store(Util::GetTimestamp(), std::memory_order_relaxed);
}

bool FrameLimit::hasRecentDxgiPresent()
{
    const uint64_t last = g_lastDxgiPresentNs.load(std::memory_order_relaxed);
    if (last == 0)
        return false;

    const uint64_t now = Util::GetTimestamp();
    return now >= last && (now - last) < 2'000'000'000ULL;
}

void FrameLimit::recordScanoutTimestamp(uint64_t submitNs, uint64_t scanoutNs, uint64_t reportedRefreshNs,
                                        uint64_t presentMarginNs, bool isVsyncMode, bool fromDisplayTiming)
{
    if (scanoutNs == 0)
        return;

    std::scoped_lock lock(g_pllMutex);

    if (fromDisplayTiming)
    {
        g_pll.displayTimingActive = true;
        if (presentMarginNs > 0 && presentMarginNs < kMaxValidRefreshNs)
            g_pll.lastPresentMarginNs = presentMarginNs;

        if (reportedRefreshNs >= kMinValidRefreshNs && reportedRefreshNs <= kMaxValidRefreshNs)
            g_pll.refreshPeriodNs = reportedRefreshNs;

        uint64_t dt = 0;
        if (g_pll.lastDisplayTimingNs > 0 && scanoutNs > g_pll.lastDisplayTimingNs)
        {
            dt = scanoutNs - g_pll.lastDisplayTimingNs;
            if (dt >= kMinValidRefreshNs && dt <= kMaxValidRefreshNs)
            {
                g_pll.recentDeltas[g_pll.deltaWriteIdx] = dt;
                g_pll.deltaWriteIdx = (g_pll.deltaWriteIdx + 1) % g_pll.recentDeltas.size();
                g_pll.deltaCount = std::min(g_pll.deltaCount + 1, g_pll.recentDeltas.size());
            }
        }
        g_pll.lastDisplayTimingNs = scanoutNs;

        if (g_pll.refreshPeriodNs == 0)
            g_pll.refreshPeriodNs = EstimateStableRefreshFromDeltas(g_pll);

        if (uint64_t med = EstimateMedianDelta(g_pll); med > 0)
        {
            g_pll.scanoutPeriodNs = med;
            g_gamescopeScanoutPeriodNs.store(med, std::memory_order_relaxed);
        }

        UpdatePllPhaseLocked(g_pll.displayAnchorNs, scanoutNs, g_pll.refreshPeriodNs);
        if (g_pll.displayClockCalibrated && g_pll.displayAnchorNs > 0)
        {
            const int64_t anchorQpc = static_cast<int64_t>(g_pll.displayAnchorNs) - g_pll.qpcToDisplayOffsetNs;
            if (anchorQpc > 0)
                g_pll.anchorQpcNs = static_cast<uint64_t>(anchorQpc);
        }

        const bool fixedCadenceLikely = isVsyncMode || State::Instance().isGamescopeActive;
        g_pll.vblankLocked = fixedCadenceLikely && g_pll.refreshPeriodNs > 0 && g_pll.stableSamples >= 8 &&
                             g_pll.phaseResidualEmaNs < 1'000'000.0;

        UpdateMaxInFlightLocked(dt, isVsyncMode);
        return;
    }

    // VK_KHR_present_wait or DXGI GetFrameStatistics scanout timestamp in QPC clock domain
    if (!State::Instance().isRunningOnLinux && !g_pll.displayTimingActive && reportedRefreshNs > 0)
    {
        if (reportedRefreshNs >= kMinValidRefreshNs && reportedRefreshNs <= kMaxValidRefreshNs)
            g_pll.refreshPeriodNs = reportedRefreshNs;
    }
    else
    {
        g_pll.presentWaitActive = true;
    }

    uint64_t dt = 0;
    if (g_pll.lastWaitScanoutQpcNs > 0 && scanoutNs > g_pll.lastWaitScanoutQpcNs)
    {
        dt = scanoutNs - g_pll.lastWaitScanoutQpcNs;
        if (dt >= kMinValidRefreshNs && dt <= kMaxValidRefreshNs && !g_pll.displayTimingActive)
        {
            g_pll.recentDeltas[g_pll.deltaWriteIdx] = dt;
            g_pll.deltaWriteIdx = (g_pll.deltaWriteIdx + 1) % g_pll.recentDeltas.size();
            g_pll.deltaCount = std::min(g_pll.deltaCount + 1, g_pll.recentDeltas.size());
        }
    }

    if (!g_pll.displayTimingActive && reportedRefreshNs == 0)
    {
        if (uint64_t est = EstimateStableRefreshFromDeltas(g_pll); est > 0)
            g_pll.refreshPeriodNs = est;
    }

    if (!g_pll.displayTimingActive)
    {
        if (uint64_t med = EstimateMedianDelta(g_pll); med > 0)
        {
            g_pll.scanoutPeriodNs = med;
            g_gamescopeScanoutPeriodNs.store(med, std::memory_order_relaxed);
        }
    }

    // Track whether present-to-scanout latency varies (VSync quantization) vs staying fixed (VRR/Immediate)
    if (submitNs > 0 && scanoutNs > submitNs)
    {
        const double latencyNs = static_cast<double>(scanoutNs - submitNs);
        if (latencyNs < static_cast<double>(kMaxValidRefreshNs))
        {
            if (g_pll.lastLatencyNs > 0.0)
            {
                const double diff = std::abs(latencyNs - g_pll.lastLatencyNs);
                g_pll.latencyVarianceEmaNs = 0.85 * g_pll.latencyVarianceEmaNs + 0.15 * diff;
            }
            g_pll.lastLatencyNs = latencyNs;
            g_pll.gpuLatencyEmaNs =
                (g_pll.gpuLatencyEmaNs <= 0.0) ? latencyNs : (0.85 * g_pll.gpuLatencyEmaNs + 0.15 * latencyNs);
        }
    }

    if (!g_pll.displayTimingActive)
    {
        const uint64_t period = g_pll.refreshPeriodNs;
        UpdatePllPhaseLocked(g_pll.anchorQpcNs, scanoutNs, period);

        const bool fixedCadenceLikely = isVsyncMode || State::Instance().isGamescopeActive;
        const bool varianceEvidence =
            isVsyncMode || !State::Instance().isRunningOnLinux ||
            (g_pll.vblankLocked ? (g_pll.latencyVarianceEmaNs > 50'000.0) : (g_pll.latencyVarianceEmaNs > 150'000.0));
        g_pll.vblankLocked = fixedCadenceLikely && period > 0 && g_pll.stableSamples >= 8 &&
                             g_pll.phaseResidualEmaNs < 1'000'000.0 && varianceEvidence;

        UpdateMaxInFlightLocked(dt, isVsyncMode);
    }

    g_pll.lastWaitScanoutQpcNs = scanoutNs;
    g_pll.lastWaitSubmitQpcNs = submitNs;
}

void FrameLimit::calibrateDisplayClock(uint64_t displayActualNs, uint64_t scanoutQpcNs)
{
    if (displayActualNs == 0 || scanoutQpcNs == 0)
        return;

    const int64_t sampleOffset = static_cast<int64_t>(displayActualNs) - static_cast<int64_t>(scanoutQpcNs);

    std::scoped_lock lock(g_pllMutex);
    if (g_pll.offsetStableCount == 0)
    {
        g_pll.qpcToDisplayOffsetNs = sampleOffset;
        g_pll.offsetStableCount = 1;
        g_pll.displayClockCalibrated = false;
        return;
    }

    const int64_t err = sampleOffset - g_pll.qpcToDisplayOffsetNs;
    if (std::llabs(err) < 2'000'000)
    {
        g_pll.qpcToDisplayOffsetNs += err / 4;
        g_pll.offsetStableCount = std::min(g_pll.offsetStableCount + 1, 32u);
        if (g_pll.offsetStableCount >= 4)
        {
            g_pll.displayClockCalibrated = true;
            if (g_pll.displayAnchorNs > 0)
            {
                const int64_t anchorQpc = static_cast<int64_t>(g_pll.displayAnchorNs) - g_pll.qpcToDisplayOffsetNs;
                if (anchorQpc > 0)
                    g_pll.anchorQpcNs = static_cast<uint64_t>(anchorQpc);
            }
        }
    }
    else
    {
        // Re-seed if a clock jump / outlier occurred
        g_pll.qpcToDisplayOffsetNs = sampleOffset;
        g_pll.offsetStableCount = 1;
        g_pll.displayClockCalibrated = false;
    }
}

uint64_t FrameLimit::getDesiredDisplayTimeNs(bool useActiveDxgiFrame)
{
    if (!Config::Instance()->FramerateLimitDisplayTimingSchedule.value_or_default() || !isLimitingActive() ||
        currentMode() == Mode::BackEdge)
    {
        std::scoped_lock lock(g_pllMutex);
        g_pll.displayTimingScheduleActive = false;
        return 0;
    }

    const uint64_t intervalNs = g_activeIntervalNs.load(std::memory_order_relaxed);
    if (intervalNs == 0 || ShouldYieldToGamescopeLimiter(intervalNs))
    {
        std::scoped_lock lock(g_pllMutex);
        g_pll.displayTimingScheduleActive = false;
        return 0;
    }

    std::scoped_lock pacerLock(g_pacerMutex);
    std::scoped_lock pllLock(g_pllMutex);

    const uint64_t frameTargetQpc = useActiveDxgiFrame ? g_pacer.currentFrameTargetNs : g_pacer.nextTargetNs;
    if (!g_pll.displayTimingActive || !g_pll.displayClockCalibrated || !g_pll.vblankLocked ||
        frameTargetQpc == 0 || g_pll.lastDisplayTimingNs == 0)
    {
        g_pll.displayTimingScheduleActive = false;
        return 0;
    }

    // Hold a 1.0ms safety cushion ahead of the deadline so wakeup jitter never misses a compositor VBlank
    constexpr uint64_t kScheduleCushionNs = 1'000'000ULL;
    const uint64_t targetQpc =
        (frameTargetQpc > kScheduleCushionNs) ? (frameTargetQpc - kScheduleCushionNs) : frameTargetQpc;

    const int64_t converted = static_cast<int64_t>(targetQpc) + g_pll.qpcToDisplayOffsetNs;
    if (converted <= 0)
    {
        g_pll.displayTimingScheduleActive = false;
        return 0;
    }

    const uint64_t desiredDisplay = static_cast<uint64_t>(converted);
    // Sanity-bound relative to latest known display timestamp
    if (desiredDisplay < g_pll.lastDisplayTimingNs ||
        (desiredDisplay - g_pll.lastDisplayTimingNs) > intervalNs * 3)
    {
        g_pll.displayTimingScheduleActive = false;
        return 0;
    }

    g_pll.displayTimingScheduleActive = true;
    return desiredDisplay;
}

uint64_t FrameLimit::alignDeadlineToVBlank(uint64_t deadlineNs, uint64_t intervalNs)
{
    return ApplyVBlankPhaseNudge(deadlineNs, intervalNs, false);
}

void FrameLimit::getRecentFrameTimes(float* outMs, int maxCount, int& outCount, float& outMinMs, float& outMaxMs)
{
    outCount = 0;
    outMinMs = 0.0f;
    outMaxMs = 0.0f;

    if (outMs == nullptr || maxCount <= 0)
        return;

    if (isSubframePacingActive())
    {
        std::scoped_lock lock(g_fgSubMutex);
        if (g_fgSub.frameTimeCount > 2)
        {
            CopyRingFrameTimes(g_fgSub.frameTimesMs, g_fgSub.frameTimeHead, g_fgSub.frameTimeCount, outMs, maxCount,
                               outCount, outMinMs, outMaxMs);
            return;
        }
    }

    std::scoped_lock lock(g_pacerMutex);
    CopyRingFrameTimes(g_pacer.frameTimesMs, g_pacer.frameTimeHead, g_pacer.frameTimeCount, outMs, maxCount, outCount,
                       outMinMs, outMaxMs);
}

FrameLimit::SyncStats FrameLimit::getStats()
{
    SyncStats stats {};
    {
        std::scoped_lock lock(g_pllMutex);
        stats.detectedRefreshHz =
            g_pll.refreshPeriodNs > 0 ? 1'000'000'000.0 / static_cast<double>(g_pll.refreshPeriodNs) : 0.0;
        stats.vblankLocked = g_pll.vblankLocked;
        stats.displayTimingActive = g_pll.displayTimingActive;
        stats.displayTimingScheduleActive = g_pll.displayTimingScheduleActive;
        stats.presentWaitActive = g_pll.presentWaitActive;
        stats.lastGpuWorkUs = static_cast<uint64_t>(g_pll.gpuLatencyEmaNs / 1000.0);
        stats.lastPresentMarginUs = g_pll.lastPresentMarginNs / 1000ULL;
    }
    {
        std::scoped_lock lock(g_pacerMutex);
        stats.lastCpuWorkUs = static_cast<uint64_t>(g_pacer.cpuWorkEmaNs / 1000.0);
        stats.simStartActive = g_pacer.simStartActive;
        stats.earlyAcquireActive = g_pacer.earlyAcquireActive;
    }
    {
        std::scoped_lock lock(g_fgSubMutex);
        const uint32_t liveMult = ActiveSubframeMultiplier();
        stats.fgSubframePacingActive = isSubframePacingActive();
        stats.fgSubframeMultiplier = liveMult > 0 ? liveMult : g_fgSub.activeMult;
    }
    if (stats.lastGpuWorkUs == 0 && State::Instance().lastUpscalerGpuMs > 0.0)
        stats.lastGpuWorkUs = static_cast<uint64_t>(State::Instance().lastUpscalerGpuMs * 1000.0);

    stats.gamescopeLimiterDetected = g_gamescopeLimiterDetected.load(std::memory_order_relaxed);
    stats.activeMaxInFlight = maxInFlightPresents();
    stats.lastTimerOvershootUs = static_cast<uint64_t>(g_timerOvershootEmaNs.load(std::memory_order_relaxed) / 1000);
    return stats;
}

void FrameLimit::prePresent(bool fgActive)
{
    const uint64_t intervalNs = ComputeIntervalNs(fgActive);
    if (intervalNs == 0)
    {
        std::scoped_lock lock(g_pacerMutex);
        ResetPacer(g_pacer);
        return;
    }

    g_activeIntervalNs.store(intervalNs, std::memory_order_relaxed);

    const int mode = static_cast<int>(currentMode());
    int64_t sleepNs = 0;

    {
        std::scoped_lock lock(g_pacerMutex);
        const uint64_t now = Util::GetTimestamp();

        if (g_pacer.lastWakeNs > 0 && now > g_pacer.lastWakeNs)
            UpdateCpuWorkEma(g_pacer, now - g_pacer.lastWakeNs, intervalNs);

        const bool modeOrFgChanged = g_pacer.lastMode != mode || g_pacer.lastFgActive != fgActive;
        const bool intervalChanged =
            g_pacer.lastIntervalNs > 0 &&
            std::llabs(static_cast<int64_t>(intervalNs) - static_cast<int64_t>(g_pacer.lastIntervalNs)) >
                static_cast<int64_t>(g_pacer.lastIntervalNs / 4);

        if (mode == static_cast<int>(Mode::BackEdge) || g_pacer.nextTargetNs == 0 || modeOrFgChanged ||
            intervalChanged || ShouldYieldToGamescopeLimiter(intervalNs))
        {
            g_pacer.currentFrameTargetNs = 0;
            g_pacer.lastFrontEdgeNs = 0;
            g_pacer.preSleptMs = 0.0;
            return;
        }

        g_pacer.currentFrameTargetNs = g_pacer.nextTargetNs;
        if (g_pacer.nextTargetNs > now)
        {
            const uint64_t delta = g_pacer.nextTargetNs - now;
            if (delta <= intervalNs * 2)
                sleepNs = static_cast<int64_t>(delta);
        }
    }

    double preSleptMs = 0.0;
    if (sleepNs > 0)
    {
        const uint64_t sleepStart = Util::GetTimestamp();
        if (const int res = sleepDurationNs(sleepNs); res != 0)
        {
            static bool errorLogged = false;
            if (!errorLogged)
            {
                LOG_ERROR("Sleep command failed: {}", res);
                errorLogged = true;
            }
        }
        const double measuredMs = static_cast<double>(Util::GetTimestamp() - sleepStart) / 1e6;
        if (measuredMs > 0.0)
            preSleptMs = measuredMs;
    }

    {
        std::scoped_lock lock(g_pacerMutex);
        const uint64_t edgeNow = Util::GetTimestamp();
        if (g_pacer.lastFrontEdgeNs > 0 && edgeNow > g_pacer.lastFrontEdgeNs &&
            (edgeNow - g_pacer.lastFrontEdgeNs) <= kPauseRebaseGapNs)
        {
            PushFrameTimeMs(g_pacer.frameTimesMs, g_pacer.frameTimeHead, g_pacer.frameTimeCount,
                            g_pacer.lastFrontEdgeNs, edgeNow);
        }
        g_pacer.lastFrontEdgeNs = edgeNow;
        g_pacer.preSleptMs = preSleptMs;
    }
}

void FrameLimit::postPresent(bool fgActive)
{
    const uint64_t intervalNs = ComputeIntervalNs(fgActive);
    if (intervalNs == 0)
    {
        std::scoped_lock lock(g_pacerMutex);
        ResetPacer(g_pacer);
        State::Instance().lastLimiterSleepMs = 0.0;
        return;
    }

    g_activeIntervalNs.store(intervalNs, std::memory_order_relaxed);

    const int mode = static_cast<int>(currentMode());
    int64_t sleepNs = 0;
    double simSleptMs = 0.0;
    double preSleptMs = 0.0;
    bool deferToSimStart = false;

    {
        std::scoped_lock lock(g_pacerMutex);
        const uint64_t now = Util::GetTimestamp();

        const bool pauseOrWrap =
            g_pacer.lastPresentFinishNs > 0 &&
            (now <= g_pacer.lastPresentFinishNs || (now - g_pacer.lastPresentFinishNs) > kPauseRebaseGapNs);
        const bool modeOrFgChanged = g_pacer.lastMode != mode || g_pacer.lastFgActive != fgActive;
        const bool intervalChanged =
            g_pacer.lastIntervalNs > 0 &&
            std::llabs(static_cast<int64_t>(intervalNs) - static_cast<int64_t>(g_pacer.lastIntervalNs)) >
                static_cast<int64_t>(g_pacer.lastIntervalNs / 4);

        const bool isBackEdge = mode == static_cast<int>(Mode::BackEdge);
        if (isBackEdge || g_pacer.lastFrontEdgeNs == 0)
        {
            PushFrameTimeMs(g_pacer.frameTimesMs, g_pacer.frameTimeHead, g_pacer.frameTimeCount,
                            g_pacer.lastPresentFinishNs, now);
        }

        bool skipSleepThisFrame = false;

        if (g_pacer.nextTargetNs == 0 || pauseOrWrap || modeOrFgChanged || intervalChanged)
        {
            g_pacer.nextTargetNs = ApplyVBlankPhaseNudge(now + intervalNs, intervalNs, true);
            g_pacer.simStartPending = false;
            skipSleepThisFrame = true;
        }
        else if (isBackEdge)
        {
            g_pacer.simStartPending = false;
            if (g_pacer.nextTargetNs > now && (g_pacer.nextTargetNs - now) <= intervalNs * 2)
            {
                sleepNs = static_cast<int64_t>(g_pacer.nextTargetNs - now);
                g_pacer.nextTargetNs = ApplyVBlankPhaseNudge(g_pacer.nextTargetNs + intervalNs, intervalNs, false);
            }
            else
            {
                // Frame exceeded its budget; start next frame immediately without a full-interval penalty sleep
                g_pacer.nextTargetNs = ApplyVBlankPhaseNudge(now + intervalNs, intervalNs, true);
                skipSleepThisFrame = true;
            }
        }
        else
        {
            g_pacer.nextTargetNs += intervalNs;
            g_pacer.nextTargetNs = ApplyVBlankPhaseNudge(g_pacer.nextTargetNs, intervalNs, false);

            // Prevent catch-up bursts after bottlenecks/stalls without penalizing Present() duration or snapping phase hard
            if (g_pacer.nextTargetNs <= now || (g_pacer.nextTargetNs - now) > intervalNs * 2)
            {
                const uint64_t rebaseBase =
                    (g_pacer.lastFrontEdgeNs > 0 && g_pacer.lastFrontEdgeNs + intervalNs > now)
                        ? (g_pacer.lastFrontEdgeNs + intervalNs)
                        : (now + intervalNs);
                g_pacer.nextTargetNs = ApplyVBlankPhaseNudge(rebaseBase, intervalNs, false);
                skipSleepThisFrame = true;
            }
        }

        g_pacer.lastPresentFinishNs = now;
        g_pacer.lastIntervalNs = intervalNs;
        g_pacer.lastMode = mode;
        g_pacer.lastFgActive = fgActive;

        simSleptMs = g_pacer.simSleptMs;
        preSleptMs = g_pacer.preSleptMs;
        g_pacer.simSleptMs = 0.0;
        g_pacer.preSleptMs = 0.0;

        // Self-healing fallback: if simStart wasn't consumed for 3 frames, fall back to postPresent JIT
        if (g_pacer.simStartActive && g_pacer.simStartPending)
        {
            if (++g_pacer.simStartMissedFrames >= 3)
            {
                g_pacer.simStartActive = false;
                g_pacer.simStartPending = false;
            }
        }

        const bool yieldGamescope = ShouldYieldToGamescopeLimiter(intervalNs);
        if (skipSleepThisFrame || yieldGamescope || mode == static_cast<int>(Mode::FrontEdgeSync))
        {
            if (yieldGamescope || mode == static_cast<int>(Mode::FrontEdgeSync))
                g_pacer.simStartPending = false;
            sleepNs = 0;
        }
        else if (isBackEdge)
        {
            // sleepNs already computed above against current frame's target
        }
        else if (!fgActive && g_pacer.simStartActive && !g_pacer.simStartPending)
        {
            // Defer wakeup sleep to the game engine's true Simulation Start (Waitable / early Acquire)
            g_pacer.simStartPending = true;
            deferToSimStart = true;
            sleepNs = 0;
        }
        else
        {
            const uint64_t wakeupNs = ComputeJitWakeupNs(g_pacer, intervalNs);
            if (wakeupNs > now)
                sleepNs = static_cast<int64_t>(wakeupNs - now);
        }
    }

    double postSleptMs = 0.0;
    if (sleepNs > 0)
    {
        const uint64_t sleepStart = Util::GetTimestamp();
        if (const int res = sleepDurationNs(sleepNs); res != 0)
        {
            static bool errorLogged = false;
            if (!errorLogged)
            {
                LOG_ERROR("Sleep command failed: {}", res);
                errorLogged = true;
            }
        }
        const double measuredMs = static_cast<double>(Util::GetTimestamp() - sleepStart) / 1e6;
        postSleptMs = measuredMs > 0.0 ? measuredMs : 0.0;
    }

    if (!deferToSimStart)
    {
        std::scoped_lock lock(g_pacerMutex);
        g_pacer.lastWakeNs = Util::GetTimestamp();
    }

    const double totalSleptMs = simSleptMs + preSleptMs + postSleptMs;
    g_lastSleepWholeUs.store(static_cast<uint64_t>(totalSleptMs * 1000.0), std::memory_order_relaxed);
    if (totalSleptMs > 0.0)
        InputCommon::add_sleep_ms(totalSleptMs);

    State::Instance().lastLimiterSleepMs = totalSleptMs;
}

void FrameLimit::sleep(bool fgActive) { postPresent(fgActive); }
