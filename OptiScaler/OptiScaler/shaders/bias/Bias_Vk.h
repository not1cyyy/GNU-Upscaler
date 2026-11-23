#pragma once

#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

class Bias_Vk : public Shader_Vk
{
  private:
    struct InternalConstants
    {
        float Bias;
        uint32_t Width;
        uint32_t Height;
    };

    static constexpr uint32_t InNumThreadsX = 16;
    static constexpr uint32_t InNumThreadsY = 16;

  public:
    Bias_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~Bias_Vk() = default;

    // Scales reactive channel (.r) into an R16G16B16A16_SFLOAT intermediate image (GetImage()),
    // left in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    bool Dispatch(VkCommandBuffer InCmdList, VkImageView InResourceView, VkExtent2D InExtent, float InBias);
};
