#include "pch.h"
#include "VulkanFrameSync.h"

#include "FrameLimit.h"
#include "State.h"
#include "Util.h"
#include <proxies/KernelBase_Proxy.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <unordered_map>

namespace
{
struct DeviceSyncInfo
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR getSurfaceCaps2 = nullptr;
    PFN_vkWaitForPresentKHR waitForPresent = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE getRefreshCycleDuration = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE getPastPresentationTiming = nullptr;
    PFN_vkSetSwapchainPresentTimingQueueSizeEXT setPresentTimingQueueSize = nullptr;
    PFN_vkGetSwapchainTimingPropertiesEXT getSwapchainTimingProperties = nullptr;
    PFN_vkGetSwapchainTimeDomainPropertiesEXT getSwapchainTimeDomainProperties = nullptr;
    PFN_vkGetPastPresentationTimingEXT getPastPresentationTimingExt = nullptr;
    PFN_vkGetCalibratedTimestampsKHR getCalibratedTimestamps = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    bool hasPresentId = false;
    bool hasPresentWait = false;
    bool hasDisplayTiming = false;
    bool hasPresentTiming = false;
    bool hasPresentAtAbsoluteTime = false;
    bool hasCalibratedTimestamps = false;
    bool hasCalibratedQpc = false;
};

std::mutex g_registryMutex;
std::unordered_map<uint64_t, DeviceSyncInfo> g_devices;
std::vector<std::shared_ptr<VulkanFrameSync::SwapchainSyncState>> g_swapchains;

struct ObserverProbe
{
    std::shared_ptr<VulkanFrameSync::SwapchainSyncState> state;
    uint64_t presentId = 0;
    uint64_t submitNs = 0;
};

constexpr size_t kMaxPendingProbes = 8;
std::mutex g_observerMutex;
std::condition_variable g_observerCv;
bool g_observerStarted = false;
bool g_observerStop = false;
std::deque<ObserverProbe> g_pendingProbes;
std::thread g_observerThread;

bool IsVsyncPresentMode(VkPresentModeKHR mode)
{
    return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
           mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
}

bool HasExtensionName(const std::vector<const char*>& list, const char* target)
{
    return std::ranges::any_of(list, [target](const char* name) { return name != nullptr && std::strcmp(name, target) == 0; });
}

bool SupportsDeviceExtension(const std::vector<VkExtensionProperties>& supported, const char* target)
{
    return std::ranges::any_of(supported,
                               [target](const VkExtensionProperties& ext) { return std::strcmp(ext.extensionName, target) == 0; });
}

template <typename T> const T* FindStructInPNext(const void* pNext, VkStructureType sType)
{
    for (auto* node = static_cast<const VkBaseInStructure*>(pNext); node != nullptr; node = node->pNext)
    {
        if (node->sType == sType)
            return reinterpret_cast<const T*>(node);
    }
    return nullptr;
}

struct SwapchainWaitGuard
{
    std::shared_ptr<VulkanFrameSync::SwapchainSyncState> state;
    bool active = false;

    explicit SwapchainWaitGuard(const std::shared_ptr<VulkanFrameSync::SwapchainSyncState>& s) : state(s)
    {
        if (!state)
            return;
        std::scoped_lock waitLock(state->waitMutex);
        if (!state->destroying.load(std::memory_order_acquire))
        {
            state->inWait.fetch_add(1, std::memory_order_acq_rel);
            active = true;
        }
    }

    ~SwapchainWaitGuard()
    {
        if (!active || !state)
            return;
        {
            std::scoped_lock waitLock(state->waitMutex);
            state->inWait.fetch_sub(1, std::memory_order_acq_rel);
        }
        state->waitDoneCv.notify_all();
    }
};

void DestroySwapchainGpuFences(const std::shared_ptr<VulkanFrameSync::SwapchainSyncState>& entry)
{
    if (!entry || entry->destroyFence == nullptr || entry->device == VK_NULL_HANDLE ||
        State::Instance().isShuttingDown || entry->inWait.load(std::memory_order_acquire) != 0)
    {
        return;
    }

    std::scoped_lock flock(entry->gpuFenceMutex);
    for (auto& slot : entry->gpuFences)
    {
        if (slot.fence != VK_NULL_HANDLE)
        {
            if (slot.pending && entry->waitForFences != nullptr)
                entry->waitForFences(entry->device, 1, &slot.fence, VK_TRUE, 50'000'000ULL);
            entry->destroyFence(entry->device, slot.fence, nullptr);
            slot = {};
        }
    }
}

void WaitForSwapchainIdle(const std::shared_ptr<VulkanFrameSync::SwapchainSyncState>& entry)
{
    if (!entry)
        return;

    entry->destroying.store(true, std::memory_order_release);

    {
        std::scoped_lock obsLock(g_observerMutex);
        std::erase_if(g_pendingProbes, [&entry](const ObserverProbe& probe) { return probe.state == entry; });
    }

    if (State::Instance().isShuttingDown)
        return;

    {
        std::unique_lock waitLock(entry->waitMutex);
        entry->waitDoneCv.wait_for(waitLock, std::chrono::milliseconds(150),
                                   [&entry]() { return entry->inWait.load(std::memory_order_acquire) == 0; });
    }

    DestroySwapchainGpuFences(entry);
}

void CalibrateClockFromSwapchain(const VulkanFrameSync::SwapchainSyncState& state)
{
    if (!state.hasCalibratedTimestamps || state.getCalibratedTimestamps == nullptr || state.device == VK_NULL_HANDLE)
        return;

    if (state.hasCalibratedQpc)
    {
        const VkCalibratedTimestampInfoKHR infos[2] = {
            { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR },
            { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR },
        };
        uint64_t timestamps[2] {};
        uint64_t maxDeviation = 0;
        if (state.getCalibratedTimestamps(state.device, 2, infos, timestamps, &maxDeviation) == VK_SUCCESS &&
            timestamps[0] > 0 && timestamps[1] > 0 && maxDeviation < 1'000'000ULL)
        {
            const uint64_t qpcNs = Util::QpcToNs(static_cast<int64_t>(timestamps[1]));
            if (qpcNs > 0)
            {
                FrameLimit::calibrateDisplayClock(timestamps[0], qpcNs, true);
                return;
            }
        }
    }

    const VkCalibratedTimestampInfoKHR info {
        VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR
    };
    uint64_t monoNs = 0;
    uint64_t maxDeviation = 0;
    const uint64_t beforeQpc = Util::GetTimestamp();
    const VkResult res = state.getCalibratedTimestamps(state.device, 1, &info, &monoNs, &maxDeviation);
    const uint64_t afterQpc = Util::GetTimestamp();
    if (res == VK_SUCCESS && monoNs > 0 && afterQpc >= beforeQpc && (afterQpc - beforeQpc) < 250'000ULL)
    {
        const uint64_t midQpc = beforeQpc + (afterQpc - beforeQpc) / 2ULL;
        FrameLimit::calibrateDisplayClock(monoNs, midQpc, true);
    }
}

void ObserverLoop()
{
    while (true)
    {
        ObserverProbe probe {};

        {
            std::unique_lock lock(g_observerMutex);
            g_observerCv.wait(lock, []() { return g_observerStop || !g_pendingProbes.empty(); });

            if (g_observerStop)
                break;

            probe = std::move(g_pendingProbes.front());
            g_pendingProbes.pop_front();
        }

        const auto& state = probe.state;
        const uint64_t presentId = probe.presentId;
        const uint64_t submitNs = probe.submitNs;

        if (state && state->waitForPresent != nullptr && presentId > 0 && !State::Instance().isShuttingDown &&
            !state->destroying.load(std::memory_order_acquire))
        {
            VkResult res = VK_ERROR_UNKNOWN;
            uint64_t scanoutNs = 0;
            {
                SwapchainWaitGuard waitGuard(state);
                if (!waitGuard.active)
                    continue;
                res = state->waitForPresent(state->device, state->swapchain, presentId, 100'000'000ULL);
                scanoutNs = Util::GetTimestamp();
            }

            if (res == VK_SUCCESS && !state->destroying.load(std::memory_order_acquire) &&
                !State::Instance().isShuttingDown)
            {
                {
                    std::scoped_lock tlock(state->timingRingMutex);
                    for (auto& link : state->timingRing)
                    {
                        if (link.presentId == presentId)
                        {
                            link.scanoutQpcNs = scanoutNs;
                            break;
                        }
                    }
                }

                FrameLimit::recordScanoutTimestamp(submitNs, scanoutNs, 0, 0,
                                                   IsVsyncPresentMode(state->presentMode), false);
            }
        }
    }
}

void EnsureObserverStarted()
{
    std::scoped_lock lock(g_observerMutex);
    if (g_observerStarted || g_observerStop || State::Instance().isShuttingDown)
        return;

    g_observerThread = std::thread(ObserverLoop);
    g_observerStarted = true;
}

void SubmitObserverProbe(const std::shared_ptr<VulkanFrameSync::SwapchainSyncState>& state, uint64_t presentId,
                         uint64_t submitNs)
{
    if (!state || state->waitForPresent == nullptr || presentId == 0 || state->destroying.load(std::memory_order_acquire) ||
        State::Instance().isShuttingDown)
        return;

    EnsureObserverStarted();

    {
        std::scoped_lock lock(g_observerMutex);
        if (g_observerStop)
            return;

        if (g_pendingProbes.size() >= kMaxPendingProbes)
            g_pendingProbes.pop_front();

        g_pendingProbes.push_back({ state, presentId, submitNs });
    }
    g_observerCv.notify_one();
}

std::shared_ptr<VulkanFrameSync::SwapchainSyncState> FindSwapchainLocked(VkSwapchainKHR swapchain)
{
    for (const auto& entry : g_swapchains)
    {
        if (entry->swapchain == swapchain && !entry->destroying.load(std::memory_order_acquire))
            return entry;
    }
    return nullptr;
}
uint64_t LookupTimingRingQpc(VulkanFrameSync::SwapchainSyncState& state, uint64_t presentId, uint32_t timingId)
{
    std::scoped_lock tlock(state.timingRingMutex);
    for (const auto& link : state.timingRing)
    {
        if (link.scanoutQpcNs == 0)
            continue;
        if ((timingId != 0 && link.timingId == timingId) || (presentId != 0 && link.presentId == presentId))
            return link.scanoutQpcNs;
    }
    return 0;
}

void AttachPresentId(VulkanFrameSync::SwapchainSyncState& state, VkPresentInfoKHR* pPresentInfo,
                     VulkanFrameSync::PresentScope& scope)
{
    if (!state.hasPresentId)
        return;

    if (const auto* existingId =
            FindStructInPNext<VkPresentIdKHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR))
    {
        if (existingId->swapchainCount > 0 && existingId->pPresentIds != nullptr && existingId->pPresentIds[0] != 0)
        {
            scope.usedPresentId = existingId->pPresentIds[0];
            if (scope.usedPresentId > state.nextPresentId)
                state.nextPresentId = scope.usedPresentId;
        }
        return;
    }

    if (const auto* existingId2 =
            FindStructInPNext<VkPresentId2KHR>(pPresentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR))
    {
        if (existingId2->swapchainCount > 0 && existingId2->pPresentIds != nullptr && existingId2->pPresentIds[0] != 0)
        {
            scope.usedPresentId = existingId2->pPresentIds[0];
            if (scope.usedPresentId > state.nextPresentId)
                state.nextPresentId = scope.usedPresentId;
        }
        return;
    }

    scope.assignedPresentId = ++state.nextPresentId;
    scope.usedPresentId = scope.assignedPresentId;
    scope.presentIdInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    scope.presentIdInfo.pNext = pPresentInfo->pNext;
    scope.presentIdInfo.swapchainCount = 1;
    scope.presentIdInfo.pPresentIds = &scope.assignedPresentId;
    pPresentInfo->pNext = &scope.presentIdInfo;
}

void HandleGoogleDisplayTiming(VulkanFrameSync::SwapchainSyncState& state, VkSwapchainKHR swapchain,
                               VkPresentInfoKHR* pPresentInfo, bool dxgiBacked, bool isVsync,
                               VulkanFrameSync::PresentScope& scope)
{
    if (!state.hasDisplayTiming || state.getPastPresentationTiming == nullptr)
        return;

    if (state.refreshDurationNs == 0 && state.getRefreshCycleDuration != nullptr)
    {
        VkRefreshCycleDurationGOOGLE cycle {};
        if (state.getRefreshCycleDuration(state.device, swapchain, &cycle) == VK_SUCCESS)
            state.refreshDurationNs = cycle.refreshDuration;
    }

    VkPastPresentationTimingGOOGLE timings[16] {};
    uint32_t timingCount = 16;
    const VkResult timingRes = state.getPastPresentationTiming(state.device, swapchain, &timingCount, timings);
    if (timingRes == VK_SUCCESS || timingRes == VK_INCOMPLETE)
    {
        const uint32_t validCount = std::min(timingCount, 16u);
        for (uint32_t i = 0; i < validCount; ++i)
        {
            if (timings[i].presentID != 0 && timings[i].actualPresentTime > 0)
            {
                if (const uint64_t matchedQpc = LookupTimingRingQpc(state, 0, timings[i].presentID); matchedQpc > 0)
                    FrameLimit::calibrateDisplayClock(timings[i].actualPresentTime, matchedQpc);
            }

            FrameLimit::recordScanoutTimestamp(0, timings[i].actualPresentTime, state.refreshDurationNs,
                                               timings[i].presentMargin, isVsync, true);
        }
    }

    if (FindStructInPNext<VkPresentTimesInfoGOOGLE>(pPresentInfo->pNext,
                                                    VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE) == nullptr)
    {
        uint32_t tid = ++state.nextTimingId;
        if (tid == 0)
            tid = ++state.nextTimingId;

        scope.usedTimingId = tid;
        scope.presentTime.presentID = tid;
        scope.presentTime.desiredPresentTime = FrameLimit::getDesiredDisplayTimeNs(dxgiBacked);
        scope.presentTimesInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        scope.presentTimesInfo.pNext = pPresentInfo->pNext;
        scope.presentTimesInfo.swapchainCount = 1;
        scope.presentTimesInfo.pTimes = &scope.presentTime;
        pPresentInfo->pNext = &scope.presentTimesInfo;
    }
}

void HandlePresentTimingExt(VulkanFrameSync::SwapchainSyncState& state, VkSwapchainKHR swapchain,
                            VkPresentInfoKHR* pPresentInfo, bool dxgiBacked, bool isVsync,
                            VulkanFrameSync::PresentScope& scope)
{
    if (!state.presentTimingExtEnabled || state.getPastPresentationTimingExt == nullptr)
        return;

    if (state.refreshDurationNs == 0 && state.getSwapchainTimingProperties != nullptr)
    {
        VkSwapchainTimingPropertiesEXT timingProps { VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT };
        if (state.getSwapchainTimingProperties(state.device, swapchain, &timingProps,
                                               &state.presentTimingPropsCounter) == VK_SUCCESS &&
            timingProps.refreshDuration > 0)
        {
            state.refreshDurationNs = timingProps.refreshDuration;
        }
    }

    VkPastPresentationTimingInfoEXT queryInfo {
        VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
        nullptr,
        VK_PAST_PRESENTATION_TIMING_ALLOW_PARTIAL_RESULTS_BIT_EXT |
            VK_PAST_PRESENTATION_TIMING_ALLOW_OUT_OF_ORDER_RESULTS_BIT_EXT,
        swapchain,
    };
    VkPresentStageTimeEXT stageBuffers[16][4] {};
    VkPastPresentationTimingEXT timings[16] {};
    for (uint32_t i = 0; i < 16u; ++i)
    {
        timings[i].sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
        timings[i].presentStageCount = 4;
        timings[i].pPresentStages = stageBuffers[i];
    }
    VkPastPresentationTimingPropertiesEXT props { VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT };
    props.presentationTimingCount = 16;
    props.pPresentationTimings = timings;

    const VkResult timingRes = state.getPastPresentationTimingExt(state.device, &queryInfo, &props);
    if (timingRes == VK_SUCCESS || timingRes == VK_INCOMPLETE)
    {
        const uint32_t validCount = std::min(props.presentationTimingCount, 16u);
        for (uint32_t i = 0; i < validCount; ++i)
        {
            uint64_t scanoutTime = 0;
            uint64_t visibleTime = 0;
            const uint32_t stageCount = std::min(timings[i].presentStageCount, 4u);
            for (uint32_t s = 0; s < stageCount; ++s)
            {
                if ((timings[i].pPresentStages[s].stage & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) != 0)
                    scanoutTime = timings[i].pPresentStages[s].time;
                else if ((timings[i].pPresentStages[s].stage &
                          VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) != 0)
                    visibleTime = timings[i].pPresentStages[s].time;
            }
            const uint64_t rawTime = scanoutTime > 0 ? scanoutTime : visibleTime;
            if (rawTime == 0)
                continue;

            if (timings[i].timeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
            {
                if (timings[i].presentId != 0)
                {
                    if (const uint64_t matchedQpc = LookupTimingRingQpc(state, timings[i].presentId, 0); matchedQpc > 0)
                        FrameLimit::calibrateDisplayClock(rawTime, matchedQpc);
                }
                FrameLimit::recordScanoutTimestamp(0, rawTime, state.refreshDurationNs, 0, isVsync, true, true);
            }
            else if (timings[i].timeDomain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR)
            {
                const uint64_t qpcNs = Util::QpcToNs(static_cast<int64_t>(rawTime));
                if (qpcNs > 0)
                    FrameLimit::recordScanoutTimestamp(0, qpcNs, state.refreshDurationNs, 0, isVsync, false, true);
            }
        }
    }

    const VkPresentStageFlagsEXT preferredStage =
        (state.presentTimingStageMask & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) != 0
            ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT
            : ((state.presentTimingStageMask & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT) != 0
                   ? VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT
                   : 0);

    if (preferredStage != 0 && state.presentTimingDomainValid && state.hasPresentId && scope.usedPresentId != 0 &&
        FindStructInPNext<VkPresentTimingsInfoEXT>(pPresentInfo->pNext,
                                                   VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT) == nullptr)
    {
        uint64_t targetTime = 0;
        if (state.hasPresentAtAbsoluteTime)
        {
            const bool isQpcDomain = (state.presentTimingDomain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR);
            const uint64_t desiredNs = FrameLimit::getDesiredDisplayTimeNs(dxgiBacked, isQpcDomain);
            if (desiredNs > 0)
                targetTime = isQpcDomain ? Util::NsToQpc(desiredNs) : desiredNs;
        }

        scope.presentTimingInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;
        scope.presentTimingInfo.pNext = nullptr;
        scope.presentTimingInfo.flags =
            (targetTime > 0 && isVsync) ? VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT : 0;
        scope.presentTimingInfo.targetTime = targetTime;
        scope.presentTimingInfo.timeDomainId = state.presentTimingDomainId;
        scope.presentTimingInfo.presentStageQueries =
            state.presentTimingStageMask &
            (VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_VISIBLE_BIT_EXT);
        scope.presentTimingInfo.targetTimeDomainPresentStage = targetTime > 0 ? preferredStage : 0;

        scope.presentTimingsInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;
        scope.presentTimingsInfo.pNext = pPresentInfo->pNext;
        scope.presentTimingsInfo.swapchainCount = 1;
        scope.presentTimingsInfo.pTimingInfos = &scope.presentTimingInfo;
        pPresentInfo->pNext = &scope.presentTimingsInfo;
    }
}
} // namespace

void VulkanFrameSync::PrepareDeviceExtensions(VkPhysicalDevice physicalDevice, VkInstance physicalDeviceInstance,
                                              PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                              PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2,
                                              VkDeviceCreateInfo* createInfo, PreparedDeviceCreate& outPrepared)
{
    outPrepared.caps = {};
    if (createInfo == nullptr || physicalDevice == VK_NULL_HANDLE)
        return;

    outPrepared.extensions.clear();
    if (createInfo->ppEnabledExtensionNames != nullptr && createInfo->enabledExtensionCount > 0)
    {
        outPrepared.extensions.assign(createInfo->ppEnabledExtensionNames,
                                      createInfo->ppEnabledExtensionNames + createInfo->enabledExtensionCount);
    }

    if (!HasExtensionName(outPrepared.extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return;

    bool hasDisplayTiming = HasExtensionName(outPrepared.extensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    bool hasPresentTiming = HasExtensionName(outPrepared.extensions, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
    bool hasPresentAtAbsoluteTime = false;
    bool hasPresentId = HasExtensionName(outPrepared.extensions, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    bool hasPresentWait = HasExtensionName(outPrepared.extensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    bool hasCalibratedTimestamps =
        HasExtensionName(outPrepared.extensions, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
        HasExtensionName(outPrepared.extensions, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    bool hasCalibratedQpc = false;

    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR getSurfaceCaps2 = nullptr;
    if (physicalDeviceInstance != VK_NULL_HANDLE && getInstanceProcAddr != nullptr)
    {
        getSurfaceCaps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            getInstanceProcAddr(physicalDeviceInstance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
        if (getSurfaceCaps2 == nullptr)
        {
            getSurfaceCaps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                getInstanceProcAddr(physicalDeviceInstance, "vkGetPhysicalDeviceSurfaceCapabilities2"));
        }
    }

    PFN_vkEnumerateDeviceExtensionProperties enumDeviceExtensions = nullptr;
    if (physicalDeviceInstance != VK_NULL_HANDLE && getInstanceProcAddr != nullptr)
    {
        enumDeviceExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            getInstanceProcAddr(physicalDeviceInstance, "vkEnumerateDeviceExtensionProperties"));
    }

    if (enumDeviceExtensions == nullptr)
    {
        if (vulkanModule == nullptr)
            vulkanModule = KernelBaseProxy::GetModuleHandleA_()("vulkan-1.dll");

        if (vulkanModule != nullptr)
        {
            enumDeviceExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                KernelBaseProxy::GetProcAddress_()(vulkanModule, "vkEnumerateDeviceExtensionProperties"));
        }
    }

    if (enumDeviceExtensions == nullptr)
        enumDeviceExtensions = vkEnumerateDeviceExtensionProperties;

    std::vector<VkExtensionProperties> supported;
    bool supportedValid = false;
    if (enumDeviceExtensions != nullptr)
    {
        while (true)
        {
            uint32_t count = 0;
            VkResult res = enumDeviceExtensions(physicalDevice, nullptr, &count, nullptr);
            if (res != VK_SUCCESS)
                break;

            supported.resize(count);
            if (count == 0)
            {
                supportedValid = true;
                break;
            }

            res = enumDeviceExtensions(physicalDevice, nullptr, &count, supported.data());
            if (res == VK_SUCCESS)
            {
                supported.resize(count);
                supportedValid = true;
                break;
            }
            if (res != VK_INCOMPLETE)
                break;
        }
    }

    if (supportedValid)
    {
        if (!hasDisplayTiming && SupportsDeviceExtension(supported, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME))
        {
            outPrepared.extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
            hasDisplayTiming = true;
        }

        const bool driverSupportsPresentId = SupportsDeviceExtension(supported, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        const bool driverSupportsPresentWait = SupportsDeviceExtension(supported, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        const bool driverSupportsPresentTiming = SupportsDeviceExtension(supported, VK_EXT_PRESENT_TIMING_EXTENSION_NAME);

        if (!hasDisplayTiming && (hasPresentTiming || (driverSupportsPresentTiming && getSurfaceCaps2 != nullptr)) &&
            (driverSupportsPresentId || hasPresentId) && getPhysicalDeviceFeatures2 != nullptr)
        {
            const auto* existingTiming = FindStructInPNext<VkPhysicalDevicePresentTimingFeaturesEXT>(
                createInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT);
            const bool appDisabledTiming = existingTiming != nullptr && existingTiming->presentTiming == VK_FALSE;
            if (!appDisabledTiming)
            {
                VkPhysicalDevicePresentTimingFeaturesEXT timingQuery {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT
                };
                VkPhysicalDeviceFeatures2 features2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timingQuery };
                getPhysicalDeviceFeatures2(physicalDevice, &features2);

                if (timingQuery.presentTiming == VK_TRUE)
                {
                    if (!hasPresentTiming)
                    {
                        outPrepared.extensions.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
                        hasPresentTiming = true;
                    }
                    hasPresentAtAbsoluteTime = (timingQuery.presentAtAbsoluteTime == VK_TRUE);
                    if (existingTiming == nullptr)
                    {
                        outPrepared.presentTimingFeatures.presentTiming = VK_TRUE;
                        outPrepared.presentTimingFeatures.presentAtAbsoluteTime = timingQuery.presentAtAbsoluteTime;
                        outPrepared.presentTimingFeatures.presentAtRelativeTime = timingQuery.presentAtRelativeTime;
                        outPrepared.presentTimingFeatures.pNext = const_cast<void*>(createInfo->pNext);
                        createInfo->pNext = &outPrepared.presentTimingFeatures;
                    }
                }
            }
        }

        if ((!hasPresentId || !hasPresentWait) && driverSupportsPresentId &&
            (driverSupportsPresentWait || hasPresentTiming) && getPhysicalDeviceFeatures2 != nullptr)
        {
            const auto* existingId = FindStructInPNext<VkPhysicalDevicePresentIdFeaturesKHR>(
                createInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
            const auto* existingWait = FindStructInPNext<VkPhysicalDevicePresentWaitFeaturesKHR>(
                createInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);

            const bool appDisabledId = existingId != nullptr && existingId->presentId == VK_FALSE;
            const bool appDisabledWait = existingWait != nullptr && existingWait->presentWait == VK_FALSE;

            if (!appDisabledId)
            {
                VkPhysicalDevicePresentWaitFeaturesKHR waitQuery {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR
                };
                VkPhysicalDevicePresentIdFeaturesKHR idQuery {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
                    (driverSupportsPresentWait && !appDisabledWait) ? static_cast<void*>(&waitQuery) : nullptr
                };
                VkPhysicalDeviceFeatures2 features2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &idQuery };
                getPhysicalDeviceFeatures2(physicalDevice, &features2);

                if (idQuery.presentId == VK_TRUE)
                {
                    if (!hasPresentId)
                    {
                        outPrepared.extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                        hasPresentId = true;
                    }
                    if (existingId == nullptr)
                    {
                        outPrepared.presentIdFeatures.presentId = VK_TRUE;
                        outPrepared.presentIdFeatures.pNext = const_cast<void*>(createInfo->pNext);
                        createInfo->pNext = &outPrepared.presentIdFeatures;
                    }

                    if (driverSupportsPresentWait && !appDisabledWait && waitQuery.presentWait == VK_TRUE)
                    {
                        if (!hasPresentWait)
                        {
                            outPrepared.extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                            hasPresentWait = true;
                        }
                        if (existingWait == nullptr)
                        {
                            outPrepared.presentWaitFeatures.presentWait = VK_TRUE;
                            outPrepared.presentWaitFeatures.pNext = const_cast<void*>(createInfo->pNext);
                            createInfo->pNext = &outPrepared.presentWaitFeatures;
                        }
                    }
                }
            }
        }

        const bool supportsCalibratedKhr =
            SupportsDeviceExtension(supported, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        const bool supportsCalibratedExt =
            SupportsDeviceExtension(supported, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        if (hasCalibratedTimestamps || supportsCalibratedKhr || supportsCalibratedExt)
        {
            PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR getTimeDomains = nullptr;
            if (physicalDeviceInstance != VK_NULL_HANDLE && getInstanceProcAddr != nullptr)
            {
                getTimeDomains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                    getInstanceProcAddr(physicalDeviceInstance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
                if (getTimeDomains == nullptr)
                {
                    getTimeDomains = reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(
                        getInstanceProcAddr(physicalDeviceInstance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
                }
            }

            bool hasMonotonicDomain = false;
            if (getTimeDomains != nullptr)
            {
                uint32_t domainCount = 0;
                if (getTimeDomains(physicalDevice, &domainCount, nullptr) == VK_SUCCESS && domainCount > 0)
                {
                    std::vector<VkTimeDomainKHR> domains(domainCount);
                    if (getTimeDomains(physicalDevice, &domainCount, domains.data()) == VK_SUCCESS)
                    {
                        for (uint32_t i = 0; i < domainCount; ++i)
                        {
                            if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
                                hasMonotonicDomain = true;
                            else if (domains[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR)
                                hasCalibratedQpc = true;
                        }
                    }
                }
            }

            if (hasMonotonicDomain)
            {
                if (!hasCalibratedTimestamps)
                {
                    if (supportsCalibratedKhr)
                    {
                        outPrepared.extensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
                        hasCalibratedTimestamps = true;
                    }
                    else if (supportsCalibratedExt)
                    {
                        outPrepared.extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
                        hasCalibratedTimestamps = true;
                    }
                }
            }
            else
            {
                hasCalibratedTimestamps = false;
                hasCalibratedQpc = false;
            }
        }
    }
    else
    {
        hasCalibratedTimestamps = false;
    }

    if (outPrepared.extensions.size() != createInfo->enabledExtensionCount)
    {
        createInfo->enabledExtensionCount = static_cast<uint32_t>(outPrepared.extensions.size());
        createInfo->ppEnabledExtensionNames = outPrepared.extensions.data();
    }

    outPrepared.caps.physicalDevice = physicalDevice;
    outPrepared.caps.getSurfaceCaps2 = getSurfaceCaps2;
    outPrepared.caps.hasPresentId = hasPresentId;
    outPrepared.caps.hasPresentWait = hasPresentWait;
    outPrepared.caps.hasDisplayTiming = hasDisplayTiming;
    outPrepared.caps.hasPresentTiming = hasPresentTiming;
    outPrepared.caps.hasPresentAtAbsoluteTime = hasPresentAtAbsoluteTime;
    outPrepared.caps.hasCalibratedTimestamps = hasCalibratedTimestamps;
    outPrepared.caps.hasCalibratedQpc = hasCalibratedQpc;
}

void VulkanFrameSync::OnDeviceCreated(VkDevice device, const DeviceCaps& caps, PFN_vkGetDeviceProcAddr getDeviceProc,
                                      PFN_vkWaitForPresentKHR originalWaitForPresent)
{
    if (device == VK_NULL_HANDLE || getDeviceProc == nullptr)
        return;

    DeviceSyncInfo info {};
    info.physicalDevice = caps.physicalDevice;
    info.getSurfaceCaps2 = caps.getSurfaceCaps2;
    info.hasPresentId = caps.hasPresentId;
    info.hasPresentWait = caps.hasPresentWait;
    info.hasDisplayTiming = caps.hasDisplayTiming;
    info.hasPresentTiming = caps.hasPresentTiming;
    info.hasPresentAtAbsoluteTime = caps.hasPresentAtAbsoluteTime;
    info.hasCalibratedTimestamps = caps.hasCalibratedTimestamps;
    info.hasCalibratedQpc = caps.hasCalibratedQpc;

    if (caps.hasPresentWait)
    {
        info.waitForPresent = originalWaitForPresent != nullptr
                                  ? originalWaitForPresent
                                  : reinterpret_cast<PFN_vkWaitForPresentKHR>(
                                        getDeviceProc(device, "vkWaitForPresentKHR"));
        if (info.waitForPresent == nullptr)
            info.hasPresentWait = false;
    }

    if (caps.hasDisplayTiming)
    {
        info.getRefreshCycleDuration = reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
            getDeviceProc(device, "vkGetRefreshCycleDurationGOOGLE"));
        info.getPastPresentationTiming = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
            getDeviceProc(device, "vkGetPastPresentationTimingGOOGLE"));

        if (info.getRefreshCycleDuration == nullptr || info.getPastPresentationTiming == nullptr)
            info.hasDisplayTiming = false;
    }

    if (caps.hasPresentTiming)
    {
        info.setPresentTimingQueueSize = reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(
            getDeviceProc(device, "vkSetSwapchainPresentTimingQueueSizeEXT"));
        info.getSwapchainTimingProperties = reinterpret_cast<PFN_vkGetSwapchainTimingPropertiesEXT>(
            getDeviceProc(device, "vkGetSwapchainTimingPropertiesEXT"));
        info.getSwapchainTimeDomainProperties = reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(
            getDeviceProc(device, "vkGetSwapchainTimeDomainPropertiesEXT"));
        info.getPastPresentationTimingExt = reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(
            getDeviceProc(device, "vkGetPastPresentationTimingEXT"));

        if (info.setPresentTimingQueueSize == nullptr || info.getPastPresentationTimingExt == nullptr)
            info.hasPresentTiming = false;
    }

    if (caps.hasCalibratedTimestamps)
    {
        info.getCalibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
            getDeviceProc(device, "vkGetCalibratedTimestampsKHR"));
        if (info.getCalibratedTimestamps == nullptr)
        {
            info.getCalibratedTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
                getDeviceProc(device, "vkGetCalibratedTimestampsEXT"));
        }
        if (info.getCalibratedTimestamps == nullptr)
            info.hasCalibratedTimestamps = false;
    }

    info.createFence = reinterpret_cast<PFN_vkCreateFence>(getDeviceProc(device, "vkCreateFence"));
    info.destroyFence = reinterpret_cast<PFN_vkDestroyFence>(getDeviceProc(device, "vkDestroyFence"));
    info.waitForFences = reinterpret_cast<PFN_vkWaitForFences>(getDeviceProc(device, "vkWaitForFences"));
    info.getFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(getDeviceProc(device, "vkGetFenceStatus"));
    info.resetFences = reinterpret_cast<PFN_vkResetFences>(getDeviceProc(device, "vkResetFences"));
    info.queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(getDeviceProc(device, "vkQueueSubmit"));

    if (info.hasPresentId || info.hasPresentWait || info.hasDisplayTiming || info.hasPresentTiming ||
        info.hasCalibratedTimestamps)
    {
        LOG_INFO("VulkanFrameSync device {:X}: present_id={}, present_wait={}, display_timing={}, present_timing={}, "
                 "calibrated_ts={}",
                 static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)), info.hasPresentId, info.hasPresentWait,
                 info.hasDisplayTiming, info.hasPresentTiming, info.hasCalibratedTimestamps);
    }

    std::scoped_lock lock(g_registryMutex);
    g_devices[static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device))] = info;
}

void VulkanFrameSync::OnDeviceDestroyed(VkDevice device)
{
    if (device == VK_NULL_HANDLE)
        return;

    std::vector<std::shared_ptr<SwapchainSyncState>> removed;
    {
        std::scoped_lock lock(g_registryMutex);
        g_devices.erase(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)));

        for (auto it = g_swapchains.begin(); it != g_swapchains.end();)
        {
            if ((*it)->device == device)
            {
                removed.push_back(*it);
                it = g_swapchains.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (const auto& entry : removed)
        WaitForSwapchainIdle(entry);
}

void VulkanFrameSync::PrepareSwapchainCreate(VkDevice device, VkSwapchainCreateInfoKHR& localCreateInfo,
                                             const VkSwapchainCreateInfoKHR*& pCreateInfo,
                                             bool& outAddedPresentTimingBit, uint32_t& outSupportedStageMask)
{
    outAddedPresentTimingBit = false;
    outSupportedStageMask = 0;

    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pCreateInfo->surface == VK_NULL_HANDLE)
        return;

    DeviceSyncInfo devInfo {};
    {
        std::scoped_lock lock(g_registryMutex);
        auto it = g_devices.find(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)));
        if (it == g_devices.end())
            return;
        devInfo = it->second;
    }

    if (!devInfo.hasPresentTiming || devInfo.hasDisplayTiming || devInfo.physicalDevice == VK_NULL_HANDLE ||
        devInfo.getSurfaceCaps2 == nullptr)
    {
        return;
    }

    VkPresentTimingSurfaceCapabilitiesEXT timingCaps {
        VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT
    };
    VkSurfaceCapabilities2KHR caps2 { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &timingCaps };
    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, nullptr, pCreateInfo->surface
    };

    if (devInfo.getSurfaceCaps2(devInfo.physicalDevice, &surfaceInfo, &caps2) == VK_SUCCESS &&
        timingCaps.presentTimingSupported == VK_TRUE)
    {
        outSupportedStageMask = timingCaps.presentStageQueries;
        if ((pCreateInfo->flags & VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT) == 0)
        {
            if (pCreateInfo != &localCreateInfo)
            {
                localCreateInfo = *pCreateInfo;
                pCreateInfo = &localCreateInfo;
            }
            localCreateInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT;
            outAddedPresentTimingBit = true;
        }
    }
}

void VulkanFrameSync::OnSwapchainCreated(VkDevice device, VkSwapchainKHR swapchain, VkPresentModeKHR presentMode,
                                         VkSwapchainKHR oldSwapchain, bool presentTimingEnabled,
                                         uint32_t supportedPresentStages)
{
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
        return;

    if (oldSwapchain != VK_NULL_HANDLE && oldSwapchain != swapchain)
        OnSwapchainDestroyed(device, oldSwapchain);

    DeviceSyncInfo devInfo {};
    {
        std::scoped_lock lock(g_registryMutex);
        auto it = g_devices.find(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device)));
        if (it == g_devices.end())
            return;
        devInfo = it->second;
    }

    auto state = std::make_shared<SwapchainSyncState>();
    state->device = device;
    state->swapchain = swapchain;
    state->presentMode = presentMode;
    state->waitForPresent = devInfo.waitForPresent;
    state->getRefreshCycleDuration = devInfo.getRefreshCycleDuration;
    state->getPastPresentationTiming = devInfo.getPastPresentationTiming;
    state->setPresentTimingQueueSize = devInfo.setPresentTimingQueueSize;
    state->getSwapchainTimingProperties = devInfo.getSwapchainTimingProperties;
    state->getSwapchainTimeDomainProperties = devInfo.getSwapchainTimeDomainProperties;
    state->getPastPresentationTimingExt = devInfo.getPastPresentationTimingExt;
    state->getCalibratedTimestamps = devInfo.getCalibratedTimestamps;
    state->createFence = devInfo.createFence;
    state->destroyFence = devInfo.destroyFence;
    state->waitForFences = devInfo.waitForFences;
    state->getFenceStatus = devInfo.getFenceStatus;
    state->resetFences = devInfo.resetFences;
    state->queueSubmit = devInfo.queueSubmit;
    state->hasPresentId = devInfo.hasPresentId;
    state->hasPresentWait = devInfo.hasPresentWait;
    state->hasDisplayTiming = devInfo.hasDisplayTiming;
    state->presentTimingExtEnabled = devInfo.hasPresentTiming && !devInfo.hasDisplayTiming && presentTimingEnabled;
    state->hasPresentAtAbsoluteTime = devInfo.hasPresentAtAbsoluteTime;
    state->presentTimingStageMask = supportedPresentStages;
    state->hasCalibratedTimestamps = devInfo.hasCalibratedTimestamps;
    state->hasCalibratedQpc = devInfo.hasCalibratedQpc;

    if (state->hasCalibratedTimestamps)
        CalibrateClockFromSwapchain(*state);

    if (state->hasDisplayTiming && state->getRefreshCycleDuration != nullptr)
    {
        VkRefreshCycleDurationGOOGLE cycle {};
        if (state->getRefreshCycleDuration(device, swapchain, &cycle) == VK_SUCCESS)
            state->refreshDurationNs = cycle.refreshDuration;
    }

    if (state->presentTimingExtEnabled)
    {
        if (state->setPresentTimingQueueSize != nullptr)
            state->setPresentTimingQueueSize(device, swapchain, 16);

        if (state->getSwapchainTimingProperties != nullptr)
        {
            VkSwapchainTimingPropertiesEXT timingProps { VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT };
            if (state->getSwapchainTimingProperties(device, swapchain, &timingProps,
                                                    &state->presentTimingPropsCounter) == VK_SUCCESS &&
                timingProps.refreshDuration > 0)
            {
                state->refreshDurationNs = timingProps.refreshDuration;
            }
        }

        if (state->getSwapchainTimeDomainProperties != nullptr)
        {
            VkSwapchainTimeDomainPropertiesEXT tdProps { VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT };
            uint64_t tdCounter = 0;
            if (state->getSwapchainTimeDomainProperties(device, swapchain, &tdProps, &tdCounter) == VK_SUCCESS &&
                tdProps.timeDomainCount > 0)
            {
                std::vector<VkTimeDomainKHR> domains(tdProps.timeDomainCount);
                std::vector<uint64_t> ids(tdProps.timeDomainCount);
                tdProps.pTimeDomains = domains.data();
                tdProps.pTimeDomainIds = ids.data();
                if (state->getSwapchainTimeDomainProperties(device, swapchain, &tdProps, &tdCounter) == VK_SUCCESS)
                {
                    for (uint32_t i = 0; i < tdProps.timeDomainCount; ++i)
                    {
                        if (domains[i] == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR)
                        {
                            state->presentTimingDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
                            state->presentTimingDomainId = ids[i];
                            state->presentTimingDomainValid = true;
                            break;
                        }
                        if (domains[i] == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR &&
                            !state->presentTimingDomainValid)
                        {
                            state->presentTimingDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
                            state->presentTimingDomainId = ids[i];
                            state->presentTimingDomainValid = true;
                        }
                    }
                }
            }
        }
    }

    std::scoped_lock lock(g_registryMutex);
    g_swapchains.push_back(std::move(state));
}

void VulkanFrameSync::OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain)
{
    if (swapchain == VK_NULL_HANDLE)
        return;

    std::shared_ptr<SwapchainSyncState> removed;
    {
        std::scoped_lock lock(g_registryMutex);
        for (auto it = g_swapchains.begin(); it != g_swapchains.end(); ++it)
        {
            if ((*it)->swapchain == swapchain && (device == VK_NULL_HANDLE || (*it)->device == device))
            {
                removed = std::move(*it);
                g_swapchains.erase(it);
                break;
            }
        }
    }

    WaitForSwapchainIdle(removed);
}

void VulkanFrameSync::SyncGpu(VkQueue queue, const VkPresentInfoKHR* pPresentInfo, int syncDepth)
{
    if (syncDepth < 0 || queue == VK_NULL_HANDLE || pPresentInfo == nullptr || pPresentInfo->swapchainCount != 1 ||
        pPresentInfo->pSwapchains == nullptr || State::Instance().isShuttingDown)
    {
        return;
    }

    const VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    std::shared_ptr<SwapchainSyncState> state;
    {
        std::scoped_lock lock(g_registryMutex);
        state = FindSwapchainLocked(swapchain);
    }

    if (!state || state->createFence == nullptr || state->waitForFences == nullptr ||
        state->getFenceStatus == nullptr || state->resetFences == nullptr || state->queueSubmit == nullptr)
    {
        return;
    }

    SwapchainWaitGuard waitGuard(state);
    if (!waitGuard.active)
        return;

    std::scoped_lock flock(state->gpuFenceMutex);
    auto& ring = state->gpuFences;
    auto& cur = ring[state->gpuFenceHead];

    if (cur.fence == VK_NULL_HANDLE)
    {
        const VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (state->createFence(state->device, &fci, nullptr, &cur.fence) != VK_SUCCESS || cur.fence == VK_NULL_HANDLE)
            return;
    }

    if (cur.pending)
    {
        state->waitForFences(state->device, 1, &cur.fence, VK_TRUE, 100'000'000ULL);
        cur.pending = false;
    }

    if (state->resetFences(state->device, 1, &cur.fence) != VK_SUCCESS)
        return;

    const uint64_t submitNs = Util::GetTimestamp();
    if (state->queueSubmit(queue, 0, nullptr, cur.fence) != VK_SUCCESS)
        return;

    const size_t curIdx = state->gpuFenceHead;
    state->gpuFenceHead = (state->gpuFenceHead + 1) % ring.size();

    if (syncDepth == 0)
    {
        const bool wasBusy = (state->getFenceStatus(state->device, cur.fence) == VK_NOT_READY);
        const VkResult wres = state->waitForFences(state->device, 1, &cur.fence, VK_TRUE, 100'000'000ULL);
        const uint64_t completeNs = Util::GetTimestamp();
        if (wres == VK_SUCCESS)
            FrameLimit::recordGpuCompletion(submitNs, completeNs, 0, wasBusy);
        return;
    }

    cur.pending = true;
    cur.submitNs = submitNs;

    const size_t prevIdx = (curIdx + ring.size() - 1) % ring.size();
    auto& prev = ring[prevIdx];
    if (prev.pending && prev.fence != VK_NULL_HANDLE)
    {
        const bool wasBusy = (state->getFenceStatus(state->device, prev.fence) == VK_NOT_READY);
        const VkResult wres = state->waitForFences(state->device, 1, &prev.fence, VK_TRUE, 100'000'000ULL);
        const uint64_t completeNs = Util::GetTimestamp();
        prev.pending = false;
        if (wres == VK_SUCCESS)
            FrameLimit::recordGpuCompletion(prev.submitNs, completeNs, 1, wasBusy);
    }
    else
    {
        FrameLimit::recordGpuCompletion(submitNs, submitNs, 1, false);
    }
}

void VulkanFrameSync::PreparePresent(VkPresentInfoKHR* pPresentInfo, bool fgPresent, bool dxgiBacked,
                                     PresentScope& scope)
{
    scope = {};
    if (fgPresent || pPresentInfo == nullptr || pPresentInfo->swapchainCount != 1 || pPresentInfo->pSwapchains == nullptr)
        return;

    const VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    std::shared_ptr<SwapchainSyncState> state;
    {
        std::scoped_lock lock(g_registryMutex);
        state = FindSwapchainLocked(swapchain);
    }

    if (!state || state->destroying.load(std::memory_order_acquire))
        return;

    if (state->hasCalibratedTimestamps && (state->calibratedSampleCounter++ % 120u) == 0u)
        CalibrateClockFromSwapchain(*state);

    // Update live present mode if VK_EXT_swapchain_maintenance1 changes it per-present
    if (const auto* modeInfo = FindStructInPNext<VkSwapchainPresentModeInfoEXT>(
            pPresentInfo->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT))
    {
        if (modeInfo->swapchainCount == 1 && modeInfo->pPresentModes != nullptr)
            state->presentMode = modeInfo->pPresentModes[0];
    }

    const bool isVsync = IsVsyncPresentMode(state->presentMode);

    AttachPresentId(*state, pPresentInfo, scope);
    HandleGoogleDisplayTiming(*state, swapchain, pPresentInfo, dxgiBacked, isVsync, scope);
    if (!state->hasDisplayTiming)
        HandlePresentTimingExt(*state, swapchain, pPresentInfo, dxgiBacked, isVsync, scope);

    const int maxInFlight = FrameLimit::maxInFlightPresents();
    if (maxInFlight > 0 && state->hasPresentWait && state->waitForPresent != nullptr &&
        scope.usedPresentId > static_cast<uint64_t>(maxInFlight))
    {
        const uint64_t waitTargetId = scope.usedPresentId - static_cast<uint64_t>(maxInFlight);
        if (state->lastWaitedId < waitTargetId)
        {
            SwapchainWaitGuard waitGuard(state);
            if (waitGuard.active)
            {
                state->waitForPresent(state->device, swapchain, waitTargetId, 50'000'000ULL);
                state->lastWaitedId = waitTargetId;
            }
        }
    }

    scope.swapchainState = std::move(state);
}

void VulkanFrameSync::PostPresent(const PresentScope& scope, VkResult presentResult, uint64_t submitNs)
{
    if (!scope.swapchainState || scope.usedPresentId == 0)
        return;

    if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR)
        return;

    if (scope.usedTimingId != 0 || (scope.swapchainState->presentTimingExtEnabled && scope.usedPresentId != 0))
    {
        std::scoped_lock tlock(scope.swapchainState->timingRingMutex);
        auto& ring = scope.swapchainState->timingRing;
        ring[scope.swapchainState->timingRingHead] = { scope.usedPresentId, scope.usedTimingId, 0 };
        scope.swapchainState->timingRingHead = (scope.swapchainState->timingRingHead + 1) % ring.size();
    }

    if (scope.swapchainState->hasPresentWait)
        SubmitObserverProbe(scope.swapchainState, scope.usedPresentId, submitNs);
}

void VulkanFrameSync::Shutdown(bool canJoinThreads)
{
    std::vector<std::shared_ptr<SwapchainSyncState>> remaining;
    {
        std::scoped_lock lock(g_registryMutex);
        remaining.swap(g_swapchains);
        g_devices.clear();
    }

    for (const auto& entry : remaining)
        entry->destroying.store(true, std::memory_order_release);

    std::thread threadToJoin;
    {
        std::scoped_lock lock(g_observerMutex);
        g_observerStop = true;
        g_pendingProbes.clear();
        if (g_observerStarted && g_observerThread.joinable())
        {
            threadToJoin = std::move(g_observerThread);
            g_observerStarted = false;
        }
    }
    g_observerCv.notify_all();

    if (threadToJoin.joinable())
    {
        if (canJoinThreads)
            threadToJoin.join();
        else
            threadToJoin.detach();
    }
}
