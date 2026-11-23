#pragma once

#include <shaders/hud_copy/HudCopy_Vk.h>

#include "precompile/HCC_Shader_Vk.h"

// Tints what differs between hudless and present pink, same bindings as HudCopy
class HCC_Vk : public HudCopy_Vk
{
    struct alignas(256) InternalCompareParams
    {
        float DiffThreshold = 0.03f;
        float PinkAmount = 0.6f;
        uint32_t HudlessEncodeMode = 0;
        uint32_t PresentEncodeMode = 0;
    };

  public:
    HCC_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
        : HudCopy_Vk(InName, InDevice, InPhysicalDevice, HCC_spv, sizeof(InternalCompareParams))
    {
    }

    bool Dispatch(VkCommandBuffer InCmdList, const VkImageInfo& InHudlessInfo, VkImageLayout HudlessState,
                  const VkImageInfo& InPresentInfo, VkImageLayout PresentState, bool isHdrPQ = false)
    {
        const bool hdrPQ = isHdrPQ || IsHdrPQActive();
        const auto targetSpace = GetTargetStorageColorSpace(InPresentInfo.Format, hdrPQ);

        InternalCompareParams constants {};
        constants.DiffThreshold = GetUiCompareDiffThreshold();
        constants.PinkAmount = 0.6f;
        constants.HudlessEncodeMode = GetColorEncodeMode(GetSrvColorSpace(InHudlessInfo.Format, hdrPQ), targetSpace);
        constants.PresentEncodeMode = GetColorEncodeMode(GetSrvColorSpace(InPresentInfo.Format, hdrPQ), targetSpace);
        return DispatchShader(InCmdList, InHudlessInfo, HudlessState, InPresentInfo, PresentState, &constants,
                              sizeof(constants));
    }
};
