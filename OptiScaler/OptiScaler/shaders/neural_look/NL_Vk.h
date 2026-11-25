#pragma once

#include "NL_Common.h"
#include "SysUtils.h"
#include <shaders/Shader_Vk.h>

class NL_Vk : public Shader_Vk, public NL_Common
{
  private:
    VkBuffer _constantBufferPrep = VK_NULL_HANDLE;
    VkDeviceMemory _constantBufferMemoryPrep = VK_NULL_HANDLE;
    void* _mappedConstantBufferPrep = nullptr;
    VkDeviceSize _constantStridePrep = 0;

    VkDescriptorPool _descriptorPoolPrep = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSetsPrep;
    VkDescriptorSetLayout _descriptorSetLayoutPrep = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayoutPrep = VK_NULL_HANDLE;
    VkPipeline _pipelinePrep = VK_NULL_HANDLE;

    VkBuffer _constantBufferGi = VK_NULL_HANDLE;
    VkDeviceMemory _constantBufferMemoryGi = VK_NULL_HANDLE;
    void* _mappedConstantBufferGi = nullptr;
    VkDeviceSize _constantStrideGi = 0;

    VkDescriptorPool _descriptorPoolGi = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> _descriptorSetsGi;
    VkDescriptorSetLayout _descriptorSetLayoutGi = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayoutGi = VK_NULL_HANDLE;
    VkPipeline _pipelineGi = VK_NULL_HANDLE;

    VkImage _halfColorDepthImage = VK_NULL_HANDLE;
    VkImageView _halfColorDepthView = VK_NULL_HANDLE;
    VkDeviceMemory _halfColorDepthMemory = VK_NULL_HANDLE;
    bool _halfColorDepthInit = false;

    VkImage _halfNormalImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView _halfNormalImageViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory _halfNormalMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool _halfNormalImageInitialized[2] = { false, false };

    VkImage _giImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView _giImageViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory _giMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool _giImageInitialized[2] = { false, false };

    VkImage _ssrImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView _ssrImageViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory _ssrMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool _ssrImageInitialized[2] = { false, false };

    uint32_t _giWidth = 0;
    uint32_t _giHeight = 0;
    int _giWriteIdx = 0;
    bool _resetGiHistory = true;

    bool CreateHalfResImage(uint32_t width, uint32_t height, VkFormat format, VkImage& outImg, VkImageView& outView,
                            VkDeviceMemory& outMem);
    bool CreateGiImages(uint32_t width, uint32_t height);
    void ReleaseGiImages();

    void UpdatePrepDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo, VkImageView colorView,
                                 VkImageView depthView, VkImageView normalsView, VkImageView motionView,
                                 VkImageView prevHalfNormalView, VkImageView outColorDepthView,
                                 VkImageView outNormalView);

    void UpdateGiDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                               VkImageView halfColorDepthView, VkImageView halfNormalView, VkImageView motionView,
                               VkImageView prevGiView, VkImageView prevSsrView, VkImageView outGiView,
                               VkImageView outSsrView);

    void UpdateCompositeDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                      VkImageView colorView, VkImageView halfColorDepthView, VkImageView motionView,
                                      VkImageView giView, VkImageView halfNormalView, VkImageView albedoView,
                                      VkImageView roughnessView, VkImageView ssrView, VkImageView outColorView);

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
