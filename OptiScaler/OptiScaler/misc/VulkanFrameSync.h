#pragma once
#include "SysUtils.h"

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

class VulkanFrameSync
{
  public:
    struct DeviceCaps
    {
        bool hasPresentId = false;
        bool hasPresentWait = false;
        bool hasDisplayTiming = false;
    };

    struct PreparedDeviceCreate
    {
        std::vector<const char*> extensions;
        VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR
        };
        VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR
        };
        DeviceCaps caps {};
    };

    struct SwapchainSyncState
    {
        VkDevice device = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

        PFN_vkWaitForPresentKHR waitForPresent = nullptr;
        PFN_vkGetRefreshCycleDurationGOOGLE getRefreshCycleDuration = nullptr;
        PFN_vkGetPastPresentationTimingGOOGLE getPastPresentationTiming = nullptr;

        bool hasPresentId = false;
        bool hasPresentWait = false;
        bool hasDisplayTiming = false;

        uint64_t nextPresentId = 0;
        uint64_t lastWaitedId = 0;
        uint64_t refreshDurationNs = 0;
        uint32_t nextTimingId = 0;

        std::atomic<bool> destroying { false };
        std::atomic<int> inWait { 0 };
        std::mutex waitMutex;
        std::condition_variable waitDoneCv;

        struct TimingLink
        {
            uint64_t presentId = 0;
            uint32_t timingId = 0;
            uint64_t scanoutQpcNs = 0;
        };
        std::mutex timingRingMutex;
        std::array<TimingLink, 16> timingRing {};
        size_t timingRingHead = 0;
    };

    struct PresentScope
    {
        std::shared_ptr<SwapchainSyncState> swapchainState;
        VkPresentIdKHR presentIdInfo {};
        uint64_t assignedPresentId = 0;
        uint64_t usedPresentId = 0;
        uint32_t usedTimingId = 0;
        VkPresentTimesInfoGOOGLE presentTimesInfo {};
        VkPresentTimeGOOGLE presentTime {};
    };

    static void PrepareDeviceExtensions(VkPhysicalDevice physicalDevice, VkInstance physicalDeviceInstance,
                                        PFN_vkGetInstanceProcAddr getInstanceProcAddr,
                                        PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2,
                                        VkDeviceCreateInfo* createInfo, PreparedDeviceCreate& outPrepared);

    static void OnDeviceCreated(VkDevice device, const DeviceCaps& caps, PFN_vkGetDeviceProcAddr getDeviceProc,
                                PFN_vkWaitForPresentKHR originalWaitForPresent);
    static void OnDeviceDestroyed(VkDevice device);

    static void OnSwapchainCreated(VkDevice device, VkSwapchainKHR swapchain, VkPresentModeKHR presentMode,
                                   VkSwapchainKHR oldSwapchain);
    static void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain);

    static void PreparePresent(VkPresentInfoKHR* pPresentInfo, bool fgPresent, bool dxgiBacked, PresentScope& scope);
    static void PostPresent(const PresentScope& scope, VkResult presentResult, uint64_t submitNs);

    static void Shutdown(bool canJoinThreads);
};
