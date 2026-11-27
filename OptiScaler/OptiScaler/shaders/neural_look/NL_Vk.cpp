#include "pch.h"

#include "NL_Vk.h"
#include "precompile/nl_composite_Shader_Vk.h"
#include "precompile/nl_denoise_Shader_Vk.h"
#include "precompile/nl_gi_temporal_Shader_Vk.h"
#include "precompile/nl_neural_prior_Shader_Vk.h"
#include "precompile/nl_prep_Shader_Vk.h"
#include "precompile/nl_pyramid_Shader_Vk.h"

#include <State.h>
#include <Util.h>
#include <array>
#include <misc/DeferredRelease.h>

namespace
{
inline VkImageMemoryBarrier MakeImgBarrier(VkImage img, VkImageLayout oldLayout, VkImageLayout newLayout,
                                           uint32_t baseMip = 0, uint32_t mipCount = 1)
{
    VkImageMemoryBarrier b {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1 };

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED)
        b.srcAccessMask = 0;
    else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL)
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    else
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;

    if (newLayout == VK_IMAGE_LAYOUT_GENERAL)
        b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    else
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    return b;
}

inline VkBufferMemoryBarrier MakeBufBarrier(VkBuffer buf, VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkBufferMemoryBarrier b {};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    return b;
}

inline VkWriteDescriptorSet WriteUbo(VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* info)
{
    return { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
             nullptr, info, nullptr };
}

inline VkWriteDescriptorSet WriteSampledImg(VkDescriptorSet set, uint32_t binding, const VkDescriptorImageInfo* info)
{
    return { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             info, nullptr, nullptr };
}

inline VkWriteDescriptorSet WriteCombinedImg(VkDescriptorSet set, uint32_t binding, const VkDescriptorImageInfo* info)
{
    return { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1,
             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info, nullptr, nullptr };
}

inline VkWriteDescriptorSet WriteStorageImg(VkDescriptorSet set, uint32_t binding, const VkDescriptorImageInfo* info)
{
    return { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
             info, nullptr, nullptr };
}

inline VkWriteDescriptorSet WriteStorageBuf(VkDescriptorSet set, uint32_t binding, const VkDescriptorBufferInfo* info)
{
    return { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             nullptr, info, nullptr };
}
} // namespace

void NL_Vk::BatchImageBarriers(VkCommandBuffer cmdBuffer, const std::vector<VkImageMemoryBarrier>& barriers)
{
    if (!barriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
    }
}

bool NL_Vk::CreateImage2D(uint32_t width, uint32_t height, VkFormat format, Image2DRes& outRes)
{
    VkImageCreateInfo imageInfo {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(_device, &imageInfo, nullptr, &outRes.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq {};
    vkGetImageMemoryRequirements(_device, outRes.image, &memReq);

    VkMemoryAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &allocInfo, nullptr, &outRes.memory) != VK_SUCCESS ||
        vkBindImageMemory(_device, outRes.image, outRes.memory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outRes.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    outRes.initialized = false;
    return vkCreateImageView(_device, &viewInfo, nullptr, &outRes.view) == VK_SUCCESS;
}

bool NL_Vk::CreateImage3D(uint32_t width, uint32_t height, uint32_t depth, VkFormat format, Image2DRes& outRes)
{
    VkImageCreateInfo imageInfo {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent = { width, height, depth };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(_device, &imageInfo, nullptr, &outRes.image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq {};
    vkGetImageMemoryRequirements(_device, outRes.image, &memReq);

    VkMemoryAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &allocInfo, nullptr, &outRes.memory) != VK_SUCCESS ||
        vkBindImageMemory(_device, outRes.image, outRes.memory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outRes.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    outRes.initialized = false;
    return vkCreateImageView(_device, &viewInfo, nullptr, &outRes.view) == VK_SUCCESS;
}

void NL_Vk::ReleaseImageRes(Image2DRes& res)
{
    if (res.view != VK_NULL_HANDLE || res.image != VK_NULL_HANDLE || res.memory != VK_NULL_HANDLE)
    {
        DeferredRelease::Enqueue(
            [device = _device, view = res.view, img = res.image, mem = res.memory]()
            {
                if (view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, view, nullptr);
                if (img != VK_NULL_HANDLE)
                    vkDestroyImage(device, img, nullptr);
                if (mem != VK_NULL_HANDLE)
                    vkFreeMemory(device, mem, nullptr);
            },
            (uint64_t) _device);
    }
    res = {};
}

bool NL_Vk::CreatePyramidImage(uint32_t mip0W, uint32_t mip0H)
{
    VkImageCreateInfo imageInfo {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { mip0W, mip0H, 1 };
    imageInfo.mipLevels = 5;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(_device, &imageInfo, nullptr, &_colorPyramidImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq {};
    vkGetImageMemoryRequirements(_device, _colorPyramidImage, &memReq);

    VkMemoryAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &allocInfo, nullptr, &_colorPyramidMemory) != VK_SUCCESS ||
        vkBindImageMemory(_device, _colorPyramidImage, _colorPyramidMemory, 0) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = _colorPyramidImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 5, 0, 1 };
    if (vkCreateImageView(_device, &viewInfo, nullptr, &_colorPyramidView) != VK_SUCCESS)
        return false;

    for (uint32_t m = 0; m < 5; ++m)
    {
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1 };
        if (vkCreateImageView(_device, &viewInfo, nullptr, &_colorPyramidMipViews[m]) != VK_SUCCESS)
            return false;
    }

    _colorPyramidInit = false;
    return true;
}

void NL_Vk::ReleasePyramidImage()
{
    if (_colorPyramidView != VK_NULL_HANDLE || _colorPyramidImage != VK_NULL_HANDLE ||
        _colorPyramidMemory != VK_NULL_HANDLE)
    {
        std::array<VkImageView, 5> mipViews {};
        for (int m = 0; m < 5; ++m)
        {
            mipViews[m] = _colorPyramidMipViews[m];
            _colorPyramidMipViews[m] = VK_NULL_HANDLE;
        }
        DeferredRelease::Enqueue(
            [device = _device, fullView = _colorPyramidView, mipViews, img = _colorPyramidImage,
             mem = _colorPyramidMemory]()
            {
                for (auto v : mipViews)
                {
                    if (v != VK_NULL_HANDLE)
                        vkDestroyImageView(device, v, nullptr);
                }
                if (fullView != VK_NULL_HANDLE)
                    vkDestroyImageView(device, fullView, nullptr);
                if (img != VK_NULL_HANDLE)
                    vkDestroyImage(device, img, nullptr);
                if (mem != VK_NULL_HANDLE)
                    vkFreeMemory(device, mem, nullptr);
            },
            (uint64_t) _device);
    }
    _colorPyramidView = VK_NULL_HANDLE;
    _colorPyramidImage = VK_NULL_HANDLE;
    _colorPyramidMemory = VK_NULL_HANDLE;
    _colorPyramidInit = false;
}

bool NL_Vk::CreateStaticBuffers()
{
    if (_sceneLightBuf.buffer != VK_NULL_HANDLE && _resolvedSceneLightBuf.buffer != VK_NULL_HANDLE &&
        _worldHashBuf.buffer != VK_NULL_HANDLE)
        return true;

    _sceneLightBuf.size = kSceneLightUints * sizeof(uint32_t);
    if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_sceneLightBuf.buffer, &_sceneLightBuf.memory,
                                         _sceneLightBuf.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return false;

    _resolvedSceneLightBuf.size = kResolvedSceneLightFloat4s * 16u;
    if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_resolvedSceneLightBuf.buffer,
                                         &_resolvedSceneLightBuf.memory, _resolvedSceneLightBuf.size,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return false;

    _worldHashBuf.size = kWorldHashCapacity * 2u * sizeof(uint32_t);
    if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_worldHashBuf.buffer, &_worldHashBuf.memory,
                                         _worldHashBuf.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return false;

    return true;
}

void NL_Vk::ReleaseStaticBuffers()
{
    auto relBuf = [device = _device](BufferRes& b)
    {
        if (b.buffer != VK_NULL_HANDLE || b.memory != VK_NULL_HANDLE)
        {
            DeferredRelease::Enqueue(
                [device, buf = b.buffer, mem = b.memory]()
                {
                    if (buf != VK_NULL_HANDLE)
                        vkDestroyBuffer(device, buf, nullptr);
                    if (mem != VK_NULL_HANDLE)
                        vkFreeMemory(device, mem, nullptr);
                },
                (uint64_t) device);
        }
        b = {};
    };

    relBuf(_sceneLightBuf);
    relBuf(_resolvedSceneLightBuf);
    relBuf(_worldHashBuf);
    relBuf(_modelWeightsStaging);
    relBuf(_modelWeightsBuf);
}

void NL_Vk::UpdateModelWeightsIfNeeded()
{
    const std::string desired = Config::Instance()->NeuralLookModelPack.value_or_default();
    if (_modelWeightsBuf.buffer != VK_NULL_HANDLE && _loadedModelPack == desired)
        return;

    if (_modelWeightsStaging.buffer == VK_NULL_HANDLE)
    {
        _modelWeightsStaging.size = NLModel::kFloatCount * sizeof(float);
        if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_modelWeightsStaging.buffer,
                                             &_modelWeightsStaging.memory, _modelWeightsStaging.size,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            LOG_ERROR("[{0}] Failed to create Vk ModelWeightsStaging", _name);
            return;
        }
        vkMapMemory(_device, _modelWeightsStaging.memory, 0, _modelWeightsStaging.size, 0,
                    &_modelWeightsStaging.mapped);
    }

    if (_modelWeightsBuf.buffer == VK_NULL_HANDLE)
    {
        _modelWeightsBuf.size = NLModel::kFloatCount * sizeof(float);
        if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_modelWeightsBuf.buffer, &_modelWeightsBuf.memory,
                                             _modelWeightsBuf.size,
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            LOG_ERROR("[{0}] Failed to create Vk ModelWeightsBuf", _name);
            return;
        }
    }

    if (_modelWeightsStaging.mapped != nullptr)
    {
        std::vector<float> weights;
        NLModel::ResolveModelWeights(desired, Util::DllPath().parent_path(), weights);
        std::memcpy(_modelWeightsStaging.mapped, weights.data(), NLModel::kFloatCount * sizeof(float));
        _loadedModelPack = desired;
        _modelWeightsDirty = true;
        LOG_INFO("[{0}] Staged Neural Look v2 model pack '{1}' for VRAM upload", _name, desired);
    }
}

bool NL_Vk::CreateGiImages(uint32_t width, uint32_t height)
{
    const uint32_t targetW = std::max(1u, width / 2u);
    const uint32_t targetH = std::max(1u, height / 2u);

    if (_halfColorDepth.image != VK_NULL_HANDLE && _colorPyramidImage != VK_NULL_HANDLE &&
        _bilateralGrid[0][0].image != VK_NULL_HANDLE && _denoisedGi.image != VK_NULL_HANDLE && _giWidth == targetW &&
        _giHeight == targetH)
    {
        return CreateStaticBuffers();
    }

    ReleaseGiImages();
    if (!CreateStaticBuffers())
        return false;

    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!CreateImage2D(targetW, targetH, fmt, _halfColorDepth) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedGiScratch) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedSsrScratch) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedMetaScratch) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedGiDirScratch) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedGi) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedSsr) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedMeta) ||
        !CreateImage2D(targetW, targetH, fmt, _denoisedGiDir))
    {
        ReleaseGiImages();
        return false;
    }

    for (int i = 0; i < 2; ++i)
    {
        if (!CreateImage2D(targetW, targetH, fmt, _halfNormal[i]) ||
            !CreateImage2D(targetW, targetH, fmt, _gi[i]) ||
            !CreateImage2D(targetW, targetH, fmt, _ssr[i]) ||
            !CreateImage2D(targetW, targetH, fmt, _giMeta[i]) ||
            !CreateImage2D(targetW, targetH, fmt, _giDir[i]))
        {
            ReleaseGiImages();
            return false;
        }
    }

    uint32_t mw = std::max(1u, (targetW + 1u) / 2u);
    uint32_t mh = std::max(1u, (targetH + 1u) / 2u);
    for (int m = 0; m < 5; ++m)
    {
        _mipDims[m][0] = std::max(1u, mw >> m);
        _mipDims[m][1] = std::max(1u, mh >> m);
    }

    if (!CreatePyramidImage(_mipDims[0][0], _mipDims[0][1]))
    {
        ReleaseGiImages();
        return false;
    }

    _eighthWidth = _mipDims[1][0];
    _eighthHeight = _mipDims[1][1];
    _gridWidth = std::max(1u, (_eighthWidth + 7u) / 8u);
    _gridHeight = std::max(1u, (_eighthHeight + 7u) / 8u);

    if (!CreateImage2D(_eighthWidth, _eighthHeight, fmt, _eighthNormal) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _eighthFeatures) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material0[0]) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material0[1]) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material1[0]) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material1[1]) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material2[0]) ||
        !CreateImage2D(_eighthWidth, _eighthHeight, fmt, _material2[1]))
    {
        ReleaseGiImages();
        return false;
    }

    for (int b = 0; b < 2; ++b)
    {
        for (int g = 0; g < 3; ++g)
        {
            if (!CreateImage3D(_gridWidth, _gridHeight, kBilateralLumaBins, fmt, _bilateralGrid[b][g]))
            {
                ReleaseGiImages();
                return false;
            }
        }
    }

    _giWidth = targetW;
    _giHeight = targetH;
    _resetGiHistory = true;
    return true;
}

void NL_Vk::ReleaseGiImages()
{
    ReleaseImageRes(_halfColorDepth);
    ReleaseImageRes(_denoisedGiScratch);
    ReleaseImageRes(_denoisedSsrScratch);
    ReleaseImageRes(_denoisedMetaScratch);
    ReleaseImageRes(_denoisedGiDirScratch);
    ReleaseImageRes(_denoisedGi);
    ReleaseImageRes(_denoisedSsr);
    ReleaseImageRes(_denoisedMeta);
    ReleaseImageRes(_denoisedGiDir);
    ReleasePyramidImage();
    ReleaseImageRes(_eighthNormal);
    ReleaseImageRes(_eighthFeatures);

    for (int i = 0; i < 2; ++i)
    {
        ReleaseImageRes(_halfNormal[i]);
        ReleaseImageRes(_gi[i]);
        ReleaseImageRes(_ssr[i]);
        ReleaseImageRes(_giMeta[i]);
        ReleaseImageRes(_giDir[i]);
        ReleaseImageRes(_material0[i]);
        ReleaseImageRes(_material1[i]);
        ReleaseImageRes(_material2[i]);
        for (int g = 0; g < 3; ++g)
            ReleaseImageRes(_bilateralGrid[i][g]);
    }

    _giWidth = 0;
    _giHeight = 0;
    _eighthWidth = 0;
    _eighthHeight = 0;
    _gridWidth = 0;
    _gridHeight = 0;
}

bool NL_Vk::CreatePassPipeline(VkPassObjects& pass, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                               const std::vector<VkDescriptorPoolSize>& poolSizes, VkDeviceSize constantSize,
                               const unsigned char* spvData, size_t spvSize)
{
    VkDescriptorSetLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &pass.descriptorSetLayout) != VK_SUCCESS)
        return false;

    VkPipelineLayoutCreateInfo pipeLayoutInfo {};
    pipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutInfo.setLayoutCount = 1;
    pipeLayoutInfo.pSetLayouts = &pass.descriptorSetLayout;
    if (vkCreatePipelineLayout(_device, &pipeLayoutInfo, nullptr, &pass.pipelineLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = _maxFramesInFlight;
    if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &pass.descriptorPool) != VK_SUCCESS)
        return false;

    Shader_Vk::CreateDescriptorSets(pass.descriptorSetLayout, pass.descriptorPool, pass.descriptorSets);

    pass.constantStride = UniformStride(_physicalDevice, constantSize);
    const VkDeviceSize totalBufSize = pass.constantStride * _maxFramesInFlight;
    if (!Shader_Vk::CreateBufferResource(_device, _physicalDevice, &pass.constantBuffer, &pass.constantMemory,
                                         totalBufSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return false;

    vkMapMemory(_device, pass.constantMemory, 0, totalBufSize, 0, &pass.mappedConstants);

    std::vector<char> code(spvData, spvData + spvSize);
    return CreateComputePipeline(_device, pass.pipelineLayout, &pass.pipeline, code);
}

void NL_Vk::DestroyPassPipeline(VkPassObjects& pass)
{
    DeferredRelease::Enqueue(
        [device = _device, p = pass]()
        {
            if (p.pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, p.pipeline, nullptr);
            if (p.descriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, p.descriptorPool, nullptr);
            if (p.descriptorSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, p.descriptorSetLayout, nullptr);
            if (p.pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, p.pipelineLayout, nullptr);
            if (p.constantBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device, p.constantBuffer, nullptr);
            if (p.constantMemory != VK_NULL_HANDLE)
                vkFreeMemory(device, p.constantMemory, nullptr);
        },
        (uint64_t) _device);
    pass = {};
}

NL_Vk::NL_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    CreateSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CreateConstantBuffer(sizeof(NLCompositeInternalConstants));

    // Pass 5: Composite (base Shader_Vk pipeline)
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(19, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(22, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        };
        CreateLayouts(b);

        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 15 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight }
        };
        CreateDescriptorPool(ps, _maxFramesInFlight);
        CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

        std::vector<char> code(nl_composite_spv, nl_composite_spv + sizeof(nl_composite_spv));
        if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, code))
        {
            LOG_ERROR("[{0}] Failed to create Composite pipeline", _name);
            return;
        }
    }

    // Pass 0: Prep
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        };
        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * _maxFramesInFlight }
        };
        if (!CreatePassPipeline(_passPrep, b, ps, sizeof(NLPrepInternalConstants), nl_prep_spv, sizeof(nl_prep_spv)))
            return;
    }

    // Pass 1: Pyramid + EighthFeatures + SH Light Field
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        };
        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, _maxFramesInFlight }
        };
        if (!CreatePassPipeline(_passPyramid, b, ps, sizeof(NLPyramidInternalConstants), nl_pyramid_spv,
                                sizeof(nl_pyramid_spv)))
            return;
    }

    // Pass 2: v2 Neural Material/Semantic Prior + Temporally Smoothed 3D Bilateral Grid
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(19, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
        };
        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 * _maxFramesInFlight }
        };
        if (!CreatePassPipeline(_passPrior, b, ps, sizeof(NLNeuralPriorInternalConstants), nl_neural_prior_spv,
                                sizeof(nl_neural_prior_spv)))
            return;
    }

    // Pass 3: Visibility-Bitmask SSGI + Bent Normal + Contact Shadows + Hi-Z GGX SSR
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            CreateBinding(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(17, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        };
        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * _maxFramesInFlight }
        };
        if (!CreatePassPipeline(_passGi, b, ps, sizeof(NLGiInternalConstants), nl_gi_temporal_spv,
                                sizeof(nl_gi_temporal_spv)))
            return;
    }

    // Pass 4a & 4b: Decoupled 2-Pass À-Trous Variance Denoise
    {
        std::vector<VkDescriptorSetLayoutBinding> b = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        };
        std::vector<VkDescriptorPoolSize> ps = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 6 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * _maxFramesInFlight }
        };
        if (!CreatePassPipeline(_passDenoise, b, ps, sizeof(NLDenoiseInternalConstants), nl_denoise_spv,
                                sizeof(nl_denoise_spv)) ||
            !CreatePassPipeline(_passDenoise2, b, ps, sizeof(NLDenoiseInternalConstants), nl_denoise_spv,
                                sizeof(nl_denoise_spv)))
        {
            return;
        }
    }

    _init = true;
}

NL_Vk::~NL_Vk()
{
    ReleaseGiImages();
    ReleaseStaticBuffers();
    DestroyPassPipeline(_passPrep);
    DestroyPassPipeline(_passPyramid);
    DestroyPassPipeline(_passPrior);
    DestroyPassPipeline(_passGi);
    DestroyPassPipeline(_passDenoise);
    DestroyPassPipeline(_passDenoise2);
}

bool NL_Vk::Dispatch(VkDevice InDevice, VkCommandBuffer InCmdList, NeuralLookConstants InConstants,
                     const VkImageInfo& InResourceInfo, const VkImageInfo& InMotionVectorsInfo,
                     const VkImageInfo& OutResourceInfo, VkImageInfo* InDepthInfo, VkImageInfo* InGBufferNormals,
                     VkImageInfo* InGBufferAlbedo, VkImageInfo* InGBufferRoughness, VkImageInfo* InGBufferMetallic,
                     VkImageInfo* InGBufferSubsurface, VkImageInfo* InReactiveMask, VkImageInfo* InTransparencyMask,
                     VkImageInfo* InExposure)
{
    if (!_init || InDevice == VK_NULL_HANDLE || InCmdList == VK_NULL_HANDLE || InDepthInfo == nullptr ||
        InDepthInfo->ImageView == VK_NULL_HANDLE || InResourceInfo.ImageView == VK_NULL_HANDLE ||
        OutResourceInfo.ImageView == VK_NULL_HANDLE)
    {
        return false;
    }

    if (!CreateGiImages(OutResourceInfo.Width, OutResourceInfo.Height))
        return false;

    UpdateModelWeightsIfNeeded();
    if (_modelWeightsBuf.buffer == VK_NULL_HANDLE)
        return false;

    ScopedGpuTime_Vk scopedGpuTime(GpuTime.get(), InCmdList);

    _currentSetIndex = (_currentSetIndex + 1) % _maxFramesInFlight;
    _frameIndex++;

    const bool hdrPQ = Shader_Vk::IsHdrPQActive();
    const auto srcSpace = Shader_Vk::GetSrvColorSpace(InResourceInfo.Format, hdrPQ);
    const auto dstSpace = Shader_Vk::GetTargetStorageColorSpace(OutResourceInfo.Format, hdrPQ);
    const int32_t inEncodeMode = static_cast<int32_t>(GetColorEncodeMode(srcSpace, ShaderColorSpace::Linear));
    const int32_t outEncodeMode = static_cast<int32_t>(GetColorEncodeMode(ShaderColorSpace::Linear, dstSpace));

    const bool useGBuffers = Config::Instance()->NeuralLookUseGBuffers.value_or_default();
    const bool hasNormals = useGBuffers && InGBufferNormals != nullptr && InGBufferNormals->ImageView != VK_NULL_HANDLE;
    const bool hasAlbedo = useGBuffers && InGBufferAlbedo != nullptr && InGBufferAlbedo->ImageView != VK_NULL_HANDLE;
    const bool hasRoughness =
        useGBuffers && InGBufferRoughness != nullptr && InGBufferRoughness->ImageView != VK_NULL_HANDLE;
    const bool hasMetallic =
        useGBuffers && InGBufferMetallic != nullptr && InGBufferMetallic->ImageView != VK_NULL_HANDLE;
    const bool hasSubsurface =
        useGBuffers && InGBufferSubsurface != nullptr && InGBufferSubsurface->ImageView != VK_NULL_HANDLE;
    const bool hasReactive = InReactiveMask != nullptr && InReactiveMask->ImageView != VK_NULL_HANDLE;
    const bool hasTransparency = InTransparencyMask != nullptr && InTransparencyMask->ImageView != VK_NULL_HANDLE;
    const bool hasExposure = InExposure != nullptr && InExposure->ImageView != VK_NULL_HANDLE;

    const auto* cfg = Config::Instance();
    const float giIntensity = cfg->NeuralLookGiIntensity.value_or_default();
    const float aoIntensity = cfg->NeuralLookAoIntensity.value_or_default();
    const float ssrIntensity = cfg->NeuralLookSsrIntensity.value_or_default();
    const float contactIntensity = cfg->NeuralLookContactShadowIntensity.value_or_default();
    const bool worldHashEnabled = cfg->NeuralLookWorldHashEnabled.value_or_default();
    const int debugView = cfg->NeuralLookDebugView.value_or_default();

    const bool ssrEnabled = (_activeTier > 0) && (ssrIntensity > 1e-4f || debugView == 5);
    const bool runGiPass = (giIntensity > 1e-4f || aoIntensity > 1e-4f || contactIntensity > 1e-4f || ssrEnabled ||
                            debugView == 1 || debugView == 2 || debugView == 7);

    _giWriteIdx = (_giWriteIdx + 1) & 1;
    const int prevIdx = _giWriteIdx ^ 1;

    NeuralLookConstants temporalConstIn = InConstants;
    if (_resetGiHistory)
    {
        temporalConstIn.ResetHistory = true;
        _resetGiHistory = false;
    }

    // Upload Neural Look weights into device-local VRAM when dirty
    if (_modelWeightsDirty && _modelWeightsStaging.buffer != VK_NULL_HANDLE)
    {
        VkBufferCopy copyRegion { 0, 0, NLModel::kFloatCount * sizeof(float) };
        vkCmdCopyBuffer(InCmdList, _modelWeightsStaging.buffer, _modelWeightsBuf.buffer, 1, &copyRegion);
        VkBufferMemoryBarrier wBar = MakeBufBarrier(_modelWeightsBuf.buffer, VK_ACCESS_TRANSFER_WRITE_BIT,
                                                    VK_ACCESS_SHADER_READ_BIT);
        vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &wBar, 0, nullptr);
        _modelWeightsDirty = false;
    }

    const VkImageView mvView =
        (InMotionVectorsInfo.ImageView != VK_NULL_HANDLE) ? InMotionVectorsInfo.ImageView : InResourceInfo.ImageView;

    // --- Pass 0: Prep ---
    {
        std::vector<VkImageMemoryBarrier> bar;
        bar.push_back(MakeImgBarrier(_halfColorDepth.image,
                                     _halfColorDepth.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_GENERAL));
        _halfColorDepth.initialized = true;

        bar.push_back(MakeImgBarrier(_halfNormal[_giWriteIdx].image,
                                     _halfNormal[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                          : VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_GENERAL));
        _halfNormal[_giWriteIdx].initialized = true;

        if (!_halfNormal[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_halfNormal[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _halfNormal[prevIdx].initialized = true;
        }
        if (!_gi[prevIdx].initialized)
        {
            bar.push_back(
                MakeImgBarrier(_gi[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _gi[prevIdx].initialized = true;
        }
        BatchImageBarriers(InCmdList, bar);

        NLPrepInternalConstants prepConst {};
        prepConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        prepConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        prepConst.HalfWidth = static_cast<int32_t>(_giWidth);
        prepConst.HalfHeight = static_cast<int32_t>(_giHeight);
        prepConst.DepthWidth = static_cast<int32_t>(InDepthInfo->Width);
        prepConst.DepthHeight = static_cast<int32_t>(InDepthInfo->Height);
        prepConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        prepConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);
        FillPrepConstants(prepConst, temporalConstIn, hasNormals, hasReactive, hasTransparency, hasExposure,
                          worldHashEnabled, inEncodeMode);

        const VkDescriptorBufferInfo uboInfo { _passPrep.constantBuffer, _passPrep.constantStride * _currentSetIndex,
                                               sizeof(NLPrepInternalConstants) };
        if (_passPrep.mappedConstants != nullptr)
            std::memcpy((uint8_t*) _passPrep.mappedConstants + uboInfo.offset, &prepConst, sizeof(prepConst));

        const VkDescriptorImageInfo colI { VK_NULL_HANDLE, InResourceInfo.ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo depI { VK_NULL_HANDLE, InDepthInfo->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo nrmI { VK_NULL_HANDLE,
                                           hasNormals ? InGBufferNormals->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mvI { VK_NULL_HANDLE, mvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pNormI { _textureSampler, _halfNormal[prevIdx].view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo reactI { VK_NULL_HANDLE,
                                             hasReactive ? InReactiveMask->ImageView : InResourceInfo.ImageView,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo transI { VK_NULL_HANDLE,
                                             hasTransparency ? InTransparencyMask->ImageView : InResourceInfo.ImageView,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo expI { VK_NULL_HANDLE,
                                           hasExposure ? InExposure->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pGiI { _textureSampler, _gi[prevIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo outCdI { VK_NULL_HANDLE, _halfColorDepth.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo outNI { VK_NULL_HANDLE, _halfNormal[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo slBufI { _sceneLightBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo whBufI { _worldHashBuf.buffer, 0, VK_WHOLE_SIZE };

        VkDescriptorSet ds = _passPrep.descriptorSets[_currentSetIndex];
        std::vector<VkWriteDescriptorSet> writes = {
            WriteUbo(ds, 0, &uboInfo),       WriteSampledImg(ds, 1, &colI),   WriteSampledImg(ds, 2, &depI),
            WriteSampledImg(ds, 3, &nrmI),   WriteSampledImg(ds, 4, &mvI),    WriteCombinedImg(ds, 5, &pNormI),
            WriteSampledImg(ds, 6, &reactI), WriteSampledImg(ds, 7, &transI), WriteSampledImg(ds, 8, &expI),
            WriteCombinedImg(ds, 9, &pGiI),  WriteStorageImg(ds, 10, &outCdI), WriteStorageImg(ds, 11, &outNI),
            WriteStorageBuf(ds, 12, &slBufI), WriteStorageBuf(ds, 13, &whBufI)
        };
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPrep.pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPrep.pipelineLayout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);
    }

    // --- Pass 1: Pyramid + EighthFeatures + SH Light Field ---
    {
        std::vector<VkImageMemoryBarrier> bar = {
            MakeImgBarrier(_halfColorDepth.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_halfNormal[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_colorPyramidImage,
                           _colorPyramidInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL, 0, 5),
            MakeImgBarrier(_eighthNormal.image,
                           _eighthNormal.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_eighthFeatures.image,
                           _eighthFeatures.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL)
        };
        _colorPyramidInit = true;
        _eighthNormal.initialized = true;
        _eighthFeatures.initialized = true;
        VkBufferMemoryBarrier bufB = MakeBufBarrier(_sceneLightBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT,
                                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 1, &bufB, static_cast<uint32_t>(bar.size()), bar.data());

        NLPyramidInternalConstants pyrConst {};
        pyrConst.HalfWidth = static_cast<int32_t>(_giWidth);
        pyrConst.HalfHeight = static_cast<int32_t>(_giHeight);
        pyrConst.Mip0Width = static_cast<int32_t>(_mipDims[0][0]);
        pyrConst.Mip0Height = static_cast<int32_t>(_mipDims[0][1]);
        pyrConst.Mip1Width = static_cast<int32_t>(_mipDims[1][0]);
        pyrConst.Mip1Height = static_cast<int32_t>(_mipDims[1][1]);
        pyrConst.Mip2Width = static_cast<int32_t>(_mipDims[2][0]);
        pyrConst.Mip2Height = static_cast<int32_t>(_mipDims[2][1]);
        pyrConst.Mip3Width = static_cast<int32_t>(_mipDims[3][0]);
        pyrConst.Mip3Height = static_cast<int32_t>(_mipDims[3][1]);
        pyrConst.Mip4Width = static_cast<int32_t>(_mipDims[4][0]);
        pyrConst.Mip4Height = static_cast<int32_t>(_mipDims[4][1]);
        FillPyramidConstants(pyrConst, temporalConstIn, static_cast<int32_t>(OutResourceInfo.Width),
                             static_cast<int32_t>(OutResourceInfo.Height));

        const VkDescriptorBufferInfo uboInfo { _passPyramid.constantBuffer,
                                               _passPyramid.constantStride * _currentSetIndex,
                                               sizeof(NLPyramidInternalConstants) };
        if (_passPyramid.mappedConstants != nullptr)
            std::memcpy((uint8_t*) _passPyramid.mappedConstants + uboInfo.offset, &pyrConst, sizeof(pyrConst));

        const VkDescriptorImageInfo cdI { VK_NULL_HANDLE, _halfColorDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo nrmI { VK_NULL_HANDLE, _halfNormal[_giWriteIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo mipUav[5];
        for (int m = 0; m < 5; ++m)
            mipUav[m] = { VK_NULL_HANDLE, _colorPyramidMipViews[m], VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo eNrmI { VK_NULL_HANDLE, _eighthNormal.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo eFeatI { VK_NULL_HANDLE, _eighthFeatures.view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo slBufI { _sceneLightBuf.buffer, 0, VK_WHOLE_SIZE };

        VkDescriptorSet ds = _passPyramid.descriptorSets[_currentSetIndex];
        std::vector<VkWriteDescriptorSet> writes = {
            WriteUbo(ds, 0, &uboInfo),          WriteSampledImg(ds, 1, &cdI),       WriteSampledImg(ds, 2, &nrmI),
            WriteStorageImg(ds, 3, &mipUav[0]), WriteStorageImg(ds, 4, &mipUav[1]), WriteStorageImg(ds, 5, &mipUav[2]),
            WriteStorageImg(ds, 6, &mipUav[3]), WriteStorageImg(ds, 7, &mipUav[4]), WriteStorageImg(ds, 8, &eNrmI),
            WriteStorageImg(ds, 9, &eFeatI),    WriteStorageBuf(ds, 10, &slBufI)
        };
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPyramid.pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPyramid.pipelineLayout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdDispatch(InCmdList, (_mipDims[0][0] + 15u) / 16u, (_mipDims[0][1] + 15u) / 16u, 1);
    }

    // --- Pass 2: v2 Neural Material/Semantic Prior + Temporally Smoothed 3D Bilateral Grid ---
    {
        std::vector<VkImageMemoryBarrier> bar = {
            MakeImgBarrier(_colorPyramidImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 5),
            MakeImgBarrier(_eighthNormal.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_eighthFeatures.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_material0[_giWriteIdx].image,
                           _material0[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                               : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_material1[_giWriteIdx].image,
                           _material1[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                               : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_material2[_giWriteIdx].image,
                           _material2[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                               : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL)
        };
        _material0[_giWriteIdx].initialized = true;
        _material1[_giWriteIdx].initialized = true;
        _material2[_giWriteIdx].initialized = true;

        if (!_material0[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_material0[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _material0[prevIdx].initialized = true;
        }
        if (!_material1[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_material1[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _material1[prevIdx].initialized = true;
        }
        if (!_material2[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_material2[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _material2[prevIdx].initialized = true;
        }
        for (int g = 0; g < 3; ++g)
        {
            if (!_bilateralGrid[prevIdx][g].initialized)
            {
                bar.push_back(MakeImgBarrier(_bilateralGrid[prevIdx][g].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
                _bilateralGrid[prevIdx][g].initialized = true;
            }
            bar.push_back(MakeImgBarrier(_bilateralGrid[_giWriteIdx][g].image,
                                         _bilateralGrid[_giWriteIdx][g].initialized
                                             ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_GENERAL));
            _bilateralGrid[_giWriteIdx][g].initialized = true;
        }

        VkBufferMemoryBarrier bufBars[2] = {
            MakeBufBarrier(_sceneLightBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            MakeBufBarrier(_resolvedSceneLightBuf.buffer, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
        };
        vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 2, bufBars, static_cast<uint32_t>(bar.size()), bar.data());

        NLNeuralPriorInternalConstants priorConst {};
        priorConst.EighthWidth = static_cast<int32_t>(_eighthWidth);
        priorConst.EighthHeight = static_cast<int32_t>(_eighthHeight);
        priorConst.Mip3Width = static_cast<int32_t>(_mipDims[3][0]);
        priorConst.Mip3Height = static_cast<int32_t>(_mipDims[3][1]);
        priorConst.GridWidth = static_cast<int32_t>(_gridWidth);
        priorConst.GridHeight = static_cast<int32_t>(_gridHeight);
        priorConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        priorConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        priorConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        priorConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);
        FillNeuralPriorConstants(priorConst, temporalConstIn);

        const VkDescriptorBufferInfo uboInfo { _passPrior.constantBuffer,
                                               _passPrior.constantStride * _currentSetIndex,
                                               sizeof(NLNeuralPriorInternalConstants) };
        if (_passPrior.mappedConstants != nullptr)
            std::memcpy((uint8_t*) _passPrior.mappedConstants + uboInfo.offset, &priorConst, sizeof(priorConst));

        const VkDescriptorImageInfo m1I { VK_NULL_HANDLE, _colorPyramidMipViews[1],
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo m3I { VK_NULL_HANDLE, _colorPyramidMipViews[3],
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo eNrmI { VK_NULL_HANDLE, _eighthNormal.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo eFeatI { VK_NULL_HANDLE, _eighthFeatures.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mvI { VK_NULL_HANDLE, mvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pm0I { _textureSampler, _material0[prevIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pm1I { _textureSampler, _material1[prevIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pm2I { _textureSampler, _material2[prevIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pbg0I { VK_NULL_HANDLE, _bilateralGrid[prevIdx][0].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pbg1I { VK_NULL_HANDLE, _bilateralGrid[prevIdx][1].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pbg2I { VK_NULL_HANDLE, _bilateralGrid[prevIdx][2].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorBufferInfo slI { _sceneLightBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo mwI { _modelWeightsBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo om0I { VK_NULL_HANDLE, _material0[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo om1I { VK_NULL_HANDLE, _material1[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo om2I { VK_NULL_HANDLE, _material2[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo bg0I { VK_NULL_HANDLE, _bilateralGrid[_giWriteIdx][0].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo bg1I { VK_NULL_HANDLE, _bilateralGrid[_giWriteIdx][1].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo bg2I { VK_NULL_HANDLE, _bilateralGrid[_giWriteIdx][2].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo rslI { _resolvedSceneLightBuf.buffer, 0, VK_WHOLE_SIZE };

        VkDescriptorSet ds = _passPrior.descriptorSets[_currentSetIndex];
        std::vector<VkWriteDescriptorSet> writes = {
            WriteUbo(ds, 0, &uboInfo),       WriteSampledImg(ds, 1, &m1I),     WriteSampledImg(ds, 2, &m3I),
            WriteSampledImg(ds, 3, &eNrmI),  WriteSampledImg(ds, 4, &eFeatI),  WriteSampledImg(ds, 5, &mvI),
            WriteCombinedImg(ds, 6, &pm0I),  WriteCombinedImg(ds, 7, &pm1I),   WriteCombinedImg(ds, 8, &pm2I),
            WriteSampledImg(ds, 9, &pbg0I),  WriteSampledImg(ds, 10, &pbg1I),  WriteSampledImg(ds, 11, &pbg2I),
            WriteStorageBuf(ds, 12, &slI),   WriteStorageBuf(ds, 13, &mwI),
            WriteStorageImg(ds, 14, &om0I),  WriteStorageImg(ds, 15, &om1I),   WriteStorageImg(ds, 16, &om2I),
            WriteStorageImg(ds, 17, &bg0I),  WriteStorageImg(ds, 18, &bg1I),   WriteStorageImg(ds, 19, &bg2I),
            WriteStorageBuf(ds, 20, &rslI)
        };
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPrior.pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passPrior.pipelineLayout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdDispatch(InCmdList, _gridWidth, _gridHeight, 1);
    }

    // --- Pass 3 & Pass 4 (2-Pass Decoupled À-Trous Denoiser) ---
    if (runGiPass)
    {
        std::vector<VkImageMemoryBarrier> bar = {
            MakeImgBarrier(_material1[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_material2[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImgBarrier(_gi[_giWriteIdx].image,
                           _gi[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                        : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_ssr[_giWriteIdx].image,
                           _ssr[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                         : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_giMeta[_giWriteIdx].image,
                           _giMeta[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL),
            MakeImgBarrier(_giDir[_giWriteIdx].image,
                           _giDir[_giWriteIdx].initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                           : VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_GENERAL)
        };
        _gi[_giWriteIdx].initialized = true;
        _ssr[_giWriteIdx].initialized = true;
        _giMeta[_giWriteIdx].initialized = true;
        _giDir[_giWriteIdx].initialized = true;

        if (!_ssr[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_ssr[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _ssr[prevIdx].initialized = true;
        }
        if (!_giMeta[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_giMeta[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _giMeta[prevIdx].initialized = true;
        }
        if (!_giDir[prevIdx].initialized)
        {
            bar.push_back(MakeImgBarrier(_giDir[prevIdx].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _giDir[prevIdx].initialized = true;
        }

        VkBufferMemoryBarrier bufBars[2] = {
            MakeBufBarrier(_resolvedSceneLightBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
            MakeBufBarrier(_worldHashBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
        };
        vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 2, bufBars, static_cast<uint32_t>(bar.size()), bar.data());

        NLGiInternalConstants giConst {};
        giConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        giConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        giConst.GiWidth = static_cast<int32_t>(_giWidth);
        giConst.GiHeight = static_cast<int32_t>(_giHeight);
        giConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        giConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);
        FillGiConstants(giConst, temporalConstIn, ssrEnabled, worldHashEnabled);

        const VkDescriptorBufferInfo uboInfo { _passGi.constantBuffer, _passGi.constantStride * _currentSetIndex,
                                               sizeof(NLGiInternalConstants) };
        if (_passGi.mappedConstants != nullptr)
            std::memcpy((uint8_t*) _passGi.mappedConstants + uboInfo.offset, &giConst, sizeof(giConst));

        const VkDescriptorImageInfo pyrI { _textureSampler, _colorPyramidView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo cdI { VK_NULL_HANDLE, _halfColorDepth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo nrmI { VK_NULL_HANDLE, _halfNormal[_giWriteIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pnrmI { VK_NULL_HANDLE, _halfNormal[prevIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mvI { VK_NULL_HANDLE, mvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pgiI { VK_NULL_HANDLE, _gi[prevIdx].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pssrI { _textureSampler, _ssr[prevIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pmetaI { VK_NULL_HANDLE, _giMeta[prevIdx].view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pdirI { VK_NULL_HANDLE, _giDir[prevIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mat1I { _textureSampler, _material1[_giWriteIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mat2I { _textureSampler, _material2[_giWriteIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorBufferInfo rslI { _resolvedSceneLightBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo whI { _worldHashBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo outGiI { VK_NULL_HANDLE, _gi[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo outSsrI { VK_NULL_HANDLE, _ssr[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo outMetaI { VK_NULL_HANDLE, _giMeta[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorImageInfo outDirI { VK_NULL_HANDLE, _giDir[_giWriteIdx].view, VK_IMAGE_LAYOUT_GENERAL };

        VkDescriptorSet ds = _passGi.descriptorSets[_currentSetIndex];
        std::vector<VkWriteDescriptorSet> writes = {
            WriteUbo(ds, 0, &uboInfo),       WriteCombinedImg(ds, 1, &pyrI),   WriteSampledImg(ds, 2, &cdI),
            WriteSampledImg(ds, 3, &nrmI),   WriteSampledImg(ds, 4, &pnrmI),   WriteSampledImg(ds, 5, &mvI),
            WriteSampledImg(ds, 6, &pgiI),   WriteCombinedImg(ds, 7, &pssrI),  WriteSampledImg(ds, 8, &pmetaI),
            WriteSampledImg(ds, 9, &pdirI),  WriteCombinedImg(ds, 10, &mat1I), WriteCombinedImg(ds, 11, &mat2I),
            WriteStorageBuf(ds, 12, &rslI),  WriteStorageBuf(ds, 13, &whI),    WriteStorageImg(ds, 14, &outGiI),
            WriteStorageImg(ds, 15, &outSsrI), WriteStorageImg(ds, 16, &outMetaI), WriteStorageImg(ds, 17, &outDirI)
        };
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passGi.pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passGi.pipelineLayout, 0, 1, &ds, 0,
                                nullptr);
        vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);

        const bool twoPassDenoise = (_activeTier >= 1);

        // Pass 4a: À-Trous Denoise (step = 1)
        {
            Image2DRes& outGi = twoPassDenoise ? _denoisedGiScratch : _denoisedGi;
            Image2DRes& outSsr = twoPassDenoise ? _denoisedSsrScratch : _denoisedSsr;
            Image2DRes& outMeta = twoPassDenoise ? _denoisedMetaScratch : _denoisedMeta;
            Image2DRes& outDir = twoPassDenoise ? _denoisedGiDirScratch : _denoisedGiDir;

            std::vector<VkImageMemoryBarrier> dnBar = {
                MakeImgBarrier(_gi[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_ssr[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_giMeta[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_giDir[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(outGi.image,
                               outGi.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(outSsr.image,
                               outSsr.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(outMeta.image,
                               outMeta.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(outDir.image,
                               outDir.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL)
            };
            outGi.initialized = true;
            outSsr.initialized = true;
            outMeta.initialized = true;
            outDir.initialized = true;
            BatchImageBarriers(InCmdList, dnBar);

            NLDenoiseInternalConstants dnConst {};
            FillDenoiseConstants(dnConst, temporalConstIn, static_cast<int32_t>(_giWidth),
                                 static_cast<int32_t>(_giHeight), ssrEnabled, 1, 0);

            const VkDescriptorBufferInfo dnUbo { _passDenoise.constantBuffer,
                                                 _passDenoise.constantStride * _currentSetIndex,
                                                 sizeof(NLDenoiseInternalConstants) };
            if (_passDenoise.mappedConstants != nullptr)
                std::memcpy((uint8_t*) _passDenoise.mappedConstants + dnUbo.offset, &dnConst, sizeof(dnConst));

            const VkDescriptorImageInfo inGiI { VK_NULL_HANDLE, _gi[_giWriteIdx].view,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inSsrI { VK_NULL_HANDLE, _ssr[_giWriteIdx].view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inMetaI { VK_NULL_HANDLE, _giMeta[_giWriteIdx].view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inDirI { VK_NULL_HANDLE, _giDir[_giWriteIdx].view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo outDGi { VK_NULL_HANDLE, outGi.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDSsr { VK_NULL_HANDLE, outSsr.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDMeta { VK_NULL_HANDLE, outMeta.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDDir { VK_NULL_HANDLE, outDir.view, VK_IMAGE_LAYOUT_GENERAL };

            VkDescriptorSet dnDs = _passDenoise.descriptorSets[_currentSetIndex];
            std::vector<VkWriteDescriptorSet> dnWrites = {
                WriteUbo(dnDs, 0, &dnUbo),        WriteSampledImg(dnDs, 1, &cdI),     WriteSampledImg(dnDs, 2, &nrmI),
                WriteSampledImg(dnDs, 3, &inGiI), WriteSampledImg(dnDs, 4, &inSsrI),  WriteSampledImg(dnDs, 5, &inMetaI),
                WriteSampledImg(dnDs, 6, &inDirI), WriteCombinedImg(dnDs, 7, &mat1I), WriteStorageImg(dnDs, 8, &outDGi),
                WriteStorageImg(dnDs, 9, &outDSsr), WriteStorageImg(dnDs, 10, &outDMeta), WriteStorageImg(dnDs, 11, &outDDir)
            };
            vkUpdateDescriptorSets(_device, static_cast<uint32_t>(dnWrites.size()), dnWrites.data(), 0, nullptr);

            vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passDenoise.pipeline);
            vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passDenoise.pipelineLayout, 0, 1, &dnDs,
                                    0, nullptr);
            vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);
        }

        // Pass 4b: À-Trous Wide Pass (step = 3, Medium/Full tier)
        if (twoPassDenoise)
        {
            std::vector<VkImageMemoryBarrier> dnBar2 = {
                MakeImgBarrier(_denoisedGiScratch.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_denoisedSsrScratch.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_denoisedMetaScratch.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_denoisedGiDirScratch.image, VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                MakeImgBarrier(_denoisedGi.image,
                               _denoisedGi.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(_denoisedSsr.image,
                               _denoisedSsr.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                        : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(_denoisedMeta.image,
                               _denoisedMeta.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                         : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL),
                MakeImgBarrier(_denoisedGiDir.image,
                               _denoisedGiDir.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                          : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL)
            };
            _denoisedGi.initialized = true;
            _denoisedSsr.initialized = true;
            _denoisedMeta.initialized = true;
            _denoisedGiDir.initialized = true;
            BatchImageBarriers(InCmdList, dnBar2);

            NLDenoiseInternalConstants dnConst2 {};
            FillDenoiseConstants(dnConst2, temporalConstIn, static_cast<int32_t>(_giWidth),
                                 static_cast<int32_t>(_giHeight), ssrEnabled, 3, 1);

            const VkDescriptorBufferInfo dnUbo2 { _passDenoise2.constantBuffer,
                                                  _passDenoise2.constantStride * _currentSetIndex,
                                                  sizeof(NLDenoiseInternalConstants) };
            if (_passDenoise2.mappedConstants != nullptr)
                std::memcpy((uint8_t*) _passDenoise2.mappedConstants + dnUbo2.offset, &dnConst2, sizeof(dnConst2));

            const VkDescriptorImageInfo inGiI { VK_NULL_HANDLE, _denoisedGiScratch.view,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inSsrI { VK_NULL_HANDLE, _denoisedSsrScratch.view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inMetaI { VK_NULL_HANDLE, _denoisedMetaScratch.view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo inDirI { VK_NULL_HANDLE, _denoisedGiDirScratch.view,
                                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo outDGi { VK_NULL_HANDLE, _denoisedGi.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDSsr { VK_NULL_HANDLE, _denoisedSsr.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDMeta { VK_NULL_HANDLE, _denoisedMeta.view, VK_IMAGE_LAYOUT_GENERAL };
            const VkDescriptorImageInfo outDDir { VK_NULL_HANDLE, _denoisedGiDir.view, VK_IMAGE_LAYOUT_GENERAL };

            VkDescriptorSet dnDs2 = _passDenoise2.descriptorSets[_currentSetIndex];
            std::vector<VkWriteDescriptorSet> dnWrites2 = {
                WriteUbo(dnDs2, 0, &dnUbo2),        WriteSampledImg(dnDs2, 1, &cdI),     WriteSampledImg(dnDs2, 2, &nrmI),
                WriteSampledImg(dnDs2, 3, &inGiI),  WriteSampledImg(dnDs2, 4, &inSsrI),  WriteSampledImg(dnDs2, 5, &inMetaI),
                WriteSampledImg(dnDs2, 6, &inDirI), WriteCombinedImg(dnDs2, 7, &mat1I),  WriteStorageImg(dnDs2, 8, &outDGi),
                WriteStorageImg(dnDs2, 9, &outDSsr), WriteStorageImg(dnDs2, 10, &outDMeta), WriteStorageImg(dnDs2, 11, &outDDir)
            };
            vkUpdateDescriptorSets(_device, static_cast<uint32_t>(dnWrites2.size()), dnWrites2.data(), 0, nullptr);

            vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passDenoise2.pipeline);
            vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _passDenoise2.pipelineLayout, 0, 1,
                                    &dnDs2, 0, nullptr);
            vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);
        }
    }

    // --- Pass 5: Composite ---
    {
        std::vector<VkImageMemoryBarrier> bar;
        bar.push_back(MakeImgBarrier(_material0[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        if (!runGiPass)
        {
            bar.push_back(MakeImgBarrier(_material1[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_material2[_giWriteIdx].image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedGi.image,
                                         _denoisedGi.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                 : VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedSsr.image,
                                         _denoisedSsr.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedMeta.image,
                                         _denoisedMeta.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                   : VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedGiDir.image,
                                         _denoisedGiDir.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                    : VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            _denoisedGi.initialized = true;
            _denoisedSsr.initialized = true;
            _denoisedMeta.initialized = true;
            _denoisedGiDir.initialized = true;
        }
        else
        {
            bar.push_back(MakeImgBarrier(_denoisedGi.image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedSsr.image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedMeta.image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
            bar.push_back(MakeImgBarrier(_denoisedGiDir.image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }

        for (int g = 0; g < 3; ++g)
            bar.push_back(MakeImgBarrier(_bilateralGrid[_giWriteIdx][g].image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

        if (!runGiPass)
        {
            VkBufferMemoryBarrier bufBars[2] = {
                MakeBufBarrier(_resolvedSceneLightBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
                MakeBufBarrier(_worldHashBuf.buffer, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
            };
            vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 2, bufBars, static_cast<uint32_t>(bar.size()), bar.data());
        }
        else
        {
            BatchImageBarriers(InCmdList, bar);
        }

        NLCompositeInternalConstants compConst {};
        compConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        compConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        compConst.GiWidth = static_cast<int32_t>(_giWidth);
        compConst.GiHeight = static_cast<int32_t>(_giHeight);
        compConst.DepthWidth = static_cast<int32_t>(InDepthInfo->Width);
        compConst.DepthHeight = static_cast<int32_t>(InDepthInfo->Height);
        compConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        compConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);
        compConst.EighthWidth = static_cast<int32_t>(_eighthWidth);
        compConst.EighthHeight = static_cast<int32_t>(_eighthHeight);

        FillCompositeConstants(compConst, temporalConstIn, runGiPass, ssrEnabled, hasNormals, hasAlbedo, hasRoughness,
                               hasMetallic, hasSubsurface, worldHashEnabled, inEncodeMode, outEncodeMode);

        const VkDescriptorBufferInfo uboInfo = WriteConstants(&compConst, sizeof(NLCompositeInternalConstants));
        const VkDescriptorImageInfo colI { VK_NULL_HANDLE, InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo depI { VK_NULL_HANDLE, InDepthInfo->ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mvI { VK_NULL_HANDLE, mvView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo dGiI { VK_NULL_HANDLE, _denoisedGi.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo nrmI { VK_NULL_HANDLE, _halfNormal[_giWriteIdx].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo dSsrI { VK_NULL_HANDLE, _denoisedSsr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo dMetaI { VK_NULL_HANDLE, _denoisedMeta.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo dDirI { VK_NULL_HANDLE, _denoisedGiDir.view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo pyrI { _textureSampler, _colorPyramidView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mat0I { VK_NULL_HANDLE, _material0[_giWriteIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mat1I { VK_NULL_HANDLE, _material1[_giWriteIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo mat2I { VK_NULL_HANDLE, _material2[_giWriteIdx].view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo bg0I { _textureSampler, _bilateralGrid[_giWriteIdx][0].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo bg1I { _textureSampler, _bilateralGrid[_giWriteIdx][1].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo bg2I { _textureSampler, _bilateralGrid[_giWriteIdx][2].view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo albI { VK_NULL_HANDLE,
                                           hasAlbedo ? InGBufferAlbedo->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo rghI { VK_NULL_HANDLE,
                                           hasRoughness ? InGBufferRoughness->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo metI { VK_NULL_HANDLE,
                                           hasMetallic ? InGBufferMetallic->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo sssI { VK_NULL_HANDLE,
                                           hasSubsurface ? InGBufferSubsurface->ImageView : InResourceInfo.ImageView,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorBufferInfo rslI { _resolvedSceneLightBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorBufferInfo whI { _worldHashBuf.buffer, 0, VK_WHOLE_SIZE };
        const VkDescriptorImageInfo outI { VK_NULL_HANDLE, OutResourceInfo.ImageView, VK_IMAGE_LAYOUT_GENERAL };

        VkDescriptorSet ds = _descriptorSets[_currentSetIndex];
        std::vector<VkWriteDescriptorSet> writes = {
            WriteUbo(ds, 0, &uboInfo),       WriteSampledImg(ds, 1, &colI),    WriteSampledImg(ds, 2, &depI),
            WriteSampledImg(ds, 3, &mvI),    WriteSampledImg(ds, 4, &dGiI),    WriteSampledImg(ds, 5, &nrmI),
            WriteSampledImg(ds, 6, &dSsrI),  WriteSampledImg(ds, 7, &dMetaI),  WriteSampledImg(ds, 8, &dDirI),
            WriteCombinedImg(ds, 9, &pyrI),  WriteSampledImg(ds, 10, &mat0I),  WriteSampledImg(ds, 11, &mat1I),
            WriteSampledImg(ds, 12, &mat2I), WriteCombinedImg(ds, 13, &bg0I),  WriteCombinedImg(ds, 14, &bg1I),
            WriteCombinedImg(ds, 15, &bg2I), WriteSampledImg(ds, 16, &albI),   WriteSampledImg(ds, 17, &rghI),
            WriteSampledImg(ds, 18, &metI),  WriteSampledImg(ds, 19, &sssI),   WriteStorageBuf(ds, 20, &rslI),
            WriteStorageBuf(ds, 21, &whI),   WriteStorageImg(ds, 22, &outI)
        };
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &ds, 0, nullptr);
        vkCmdDispatch(InCmdList, (OutResourceInfo.Width + 15u) / 16u, (OutResourceInfo.Height + 15u) / 16u, 1);
    }

    return true;
}
