#pragma once

#include "NL_Common.h"
#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

class NL_Vk : public Shader_Vk, public NL_Common
{
  private:
    VkBuffer _constantBufferGi = VK_NULL_HANDLE;
    VkDeviceMemory _constantBufferMemoryGi = VK_NULL_HANDLE;
    void* _mappedConstantBufferGi = nullptr;
    VkDeviceSize _constantStrideGi = 0;

    VkDescriptorPool _descriptorPoolGi = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSetsGi;
    VkDescriptorSetLayout _descriptorSetLayoutGi = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayoutGi = VK_NULL_HANDLE;
    VkPipeline _pipelineGi = VK_NULL_HANDLE;

    VkImage _giImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView _giImageViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory _giMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool _giImageInitialized[2] = { false, false };
    uint32_t _giWidth = 0;
    uint32_t _giHeight = 0;
    int _giWriteIdx = 0;
    bool _resetGiHistory = true;

    bool CreateGiImages(uint32_t width, uint32_t height);
    void ReleaseGiImages();

    void UpdateGiDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo, VkImageView colorView,
                               VkImageView depthView, VkImageView motionView, VkImageView prevGiView,
                               VkImageView normalsView, VkImageView outGiView);

    void UpdateCompositeDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                      VkImageView colorView, VkImageView depthView, VkImageView motionView,
                                      VkImageView giView, VkImageView normalsView, VkImageView albedoView,
                                      VkImageView roughnessView, VkImageView outColorView);

  public:
    NL_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~NL_Vk();

    bool Dispatch(VkDevice InDevice, VkCommandBuffer InCmdList, NeuralLookConstants InConstants,
                  const VkImageInfo& InResourceInfo, const VkImageInfo& InMotionVectorsInfo,
                  const VkImageInfo& OutResourceInfo, VkImageInfo* InDepthInfo,
                  VkImageInfo* InGBufferNormals = nullptr, VkImageInfo* InGBufferAlbedo = nullptr,
                  VkImageInfo* InGBufferRoughness = nullptr);

    bool CreateImageResource(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height,
                             VkFormat format, VkImageUsageFlags usage)
    {
        return Shader_Vk::CreateImageResource(width, height, format, usage);
    }

    void SetImageLayout(VkCommandBuffer cmdBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                        VkImageSubresourceRange subresourceRange)
    {
        Shader_Vk::SetImageLayout(cmdBuffer, image, oldLayout, newLayout, subresourceRange);
    }
};
