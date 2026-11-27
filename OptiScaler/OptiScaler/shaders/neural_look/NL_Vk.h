#pragma once

#include "NL_Common.h"
#include "NL_Model.h"
#include "SysUtils.h"
#include <shaders/Shader_Vk.h>
#include <string>
#include <vector>

class NL_Vk : public Shader_Vk, public NL_Common
{
  private:
    struct VkPassObjects
    {
        VkBuffer constantBuffer = VK_NULL_HANDLE;
        VkDeviceMemory constantMemory = VK_NULL_HANDLE;
        void* mappedConstants = nullptr;
        VkDeviceSize constantStride = 0;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> descriptorSets;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    VkPassObjects _passPrep {};
    VkPassObjects _passPyramid {};
    VkPassObjects _passPrior {};
    VkPassObjects _passGi {};
    VkPassObjects _passDenoise {};

    struct Image2DRes
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        bool initialized = false;
    };

    struct BufferRes
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
    };

    Image2DRes _halfColorDepth {};
    Image2DRes _halfNormal[2] {};
    Image2DRes _gi[2] {};
    Image2DRes _ssr[2] {};
    Image2DRes _giMeta[2] {};
    Image2DRes _giDir[2] {};

    Image2DRes _denoisedGi {};
    Image2DRes _denoisedSsr {};
    Image2DRes _denoisedMeta {};
    Image2DRes _denoisedGiDir {};

    VkImage _colorPyramidImage = VK_NULL_HANDLE;
    VkImageView _colorPyramidView = VK_NULL_HANDLE;
    VkImageView _colorPyramidMipViews[5] = {};
    VkDeviceMemory _colorPyramidMemory = VK_NULL_HANDLE;
    bool _colorPyramidInit = false;

    Image2DRes _eighthNormal {};
    Image2DRes _eighthFeatures {};
    Image2DRes _material0[2] {};
    Image2DRes _material1[2] {};
    Image2DRes _material2[2] {};
    Image2DRes _bilateralGrid[2][3] {};

    BufferRes _sceneLightBuf {};
    BufferRes _resolvedSceneLightBuf {};
    BufferRes _worldHashBuf {};
    BufferRes _modelWeightsStaging {};
    BufferRes _modelWeightsBuf {};
    bool _modelWeightsDirty = false;
    std::string _loadedModelPack;

    uint32_t _giWidth = 0;
    uint32_t _giHeight = 0;
    uint32_t _mipDims[5][2] = {};
    uint32_t _eighthWidth = 0;
    uint32_t _eighthHeight = 0;
    uint32_t _gridWidth = 0;
    uint32_t _gridHeight = 0;
    int _giWriteIdx = 0;
    bool _resetGiHistory = true;

    bool CreateImage2D(uint32_t width, uint32_t height, VkFormat format, Image2DRes& outRes);
    bool CreateImage3D(uint32_t width, uint32_t height, uint32_t depth, VkFormat format, Image2DRes& outRes);
    void ReleaseImageRes(Image2DRes& res);
    bool CreatePyramidImage(uint32_t mip0W, uint32_t mip0H);
    void ReleasePyramidImage();

    bool CreateGiImages(uint32_t width, uint32_t height);
    void ReleaseGiImages();

    bool CreateStaticBuffers();
    void ReleaseStaticBuffers();
    void UpdateModelWeightsIfNeeded();

    bool CreatePassPipeline(VkPassObjects& pass, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                            const std::vector<VkDescriptorPoolSize>& poolSizes, VkDeviceSize constantSize,
                            const unsigned char* spvData, size_t spvSize);
    void DestroyPassPipeline(VkPassObjects& pass);

    void BatchImageBarriers(VkCommandBuffer cmdBuffer, const std::vector<VkImageMemoryBarrier>& barriers);

  public:
    NL_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice);
    ~NL_Vk();

    bool Dispatch(VkDevice InDevice, VkCommandBuffer InCmdList, NeuralLookConstants InConstants,
                  const VkImageInfo& InResourceInfo, const VkImageInfo& InMotionVectorsInfo,
                  const VkImageInfo& OutResourceInfo, VkImageInfo* InDepthInfo,
                  VkImageInfo* InGBufferNormals = nullptr, VkImageInfo* InGBufferAlbedo = nullptr,
                  VkImageInfo* InGBufferRoughness = nullptr, VkImageInfo* InGBufferMetallic = nullptr,
                  VkImageInfo* InGBufferSubsurface = nullptr, VkImageInfo* InReactiveMask = nullptr,
                  VkImageInfo* InTransparencyMask = nullptr, VkImageInfo* InExposure = nullptr);

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
