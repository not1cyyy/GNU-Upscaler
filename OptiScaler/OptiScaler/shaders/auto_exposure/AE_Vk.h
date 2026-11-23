#pragma once

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

// Computes an FSR2 style auto exposure from the renderSize part of color into a 1x1 R32_SFLOAT texture
class AE_Vk : public Shader_Vk
{
  private:
    struct InternalConstants
    {
        uint32_t RenderWidth;
        uint32_t RenderHeight;
        uint32_t GroupCountX;
        uint32_t GroupCount;
        float DeltaTime;
        uint32_t Reset;
        uint32_t PassIndex;
        float PreExposure;
    };

    static constexpr uint32_t AE_THREADS = 256;
    static constexpr uint32_t AE_TILE = 32;

    // [0] smoothed log luma, [1..] per group sums
    VkBuffer _dataBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _dataMemory = VK_NULL_HANDLE;
    uint32_t _dataElements = 0;

    VkImageLayout _exposureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool _historyValid = false;

    bool EnsureDataBuffer(uint32_t InElements);

  public:
    AE_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~AE_Vk() override;

    // Leaves Exposure() in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    bool Dispatch(VkCommandBuffer InCmdList, VkImageView InColorView, uint32_t InRenderWidth, uint32_t InRenderHeight,
                  float InPreExposure, float InDeltaTimeSeconds, bool InReset);

    VkImage Exposure() const { return GetImage(); }
    VkImageView ExposureView() const { return GetImageView(); }
};
