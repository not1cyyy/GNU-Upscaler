#include "pch.h"

#include "NL_Vk.h"
#include "precompile/nl_composite_Shader_Vk.h"
#include "precompile/nl_gi_temporal_Shader_Vk.h"
#include "precompile/nl_prep_Shader_Vk.h"

#include <State.h>
#include <misc/DeferredRelease.h>

bool NL_Vk::CreateHalfResImage(uint32_t width, uint32_t height, VkFormat format, VkImage& outImg, VkImageView& outView,
                               VkDeviceMemory& outMem)
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

    if (vkCreateImage(_device, &imageInfo, nullptr, &outImg) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReq {};
    vkGetImageMemoryRequirements(_device, outImg, &memReq);

    VkMemoryAllocateInfo allocInfo {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(_device, &allocInfo, nullptr, &outMem) != VK_SUCCESS ||
        vkBindImageMemory(_device, outImg, outMem, 0) != VK_SUCCESS)
    {
        return false;
    }

    VkImageViewCreateInfo viewInfo {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImg;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    return vkCreateImageView(_device, &viewInfo, nullptr, &outView) == VK_SUCCESS;
}

bool NL_Vk::CreateGiImages(uint32_t width, uint32_t height)
{
    const uint32_t targetW = std::max(1u, width / 2u);
    const uint32_t targetH = std::max(1u, height / 2u);

    if (_halfColorDepthImage != VK_NULL_HANDLE && _halfNormalImage != VK_NULL_HANDLE &&
        _giImages[0] != VK_NULL_HANDLE && _giImages[1] != VK_NULL_HANDLE && _ssrImages[0] != VK_NULL_HANDLE &&
        _ssrImages[1] != VK_NULL_HANDLE && _giWidth == targetW && _giHeight == targetH)
    {
        return true;
    }

    ReleaseGiImages();

    if (!CreateHalfResImage(targetW, targetH, VK_FORMAT_R16G16B16A16_SFLOAT, _halfColorDepthImage,
                            _halfColorDepthView, _halfColorDepthMemory) ||
        !CreateHalfResImage(targetW, targetH, VK_FORMAT_R16G16_SFLOAT, _halfNormalImage, _halfNormalView,
                            _halfNormalMemory))
    {
        LOG_ERROR("[{0}] Failed to create half-res G-proxy VkImages", _name);
        ReleaseGiImages();
        return false;
    }
    _halfColorDepthInit = false;
    _halfNormalInit = false;

    for (int i = 0; i < 2; ++i)
    {
        if (!CreateHalfResImage(targetW, targetH, VK_FORMAT_R16G16B16A16_SFLOAT, _giImages[i], _giImageViews[i],
                                _giMemories[i]) ||
            !CreateHalfResImage(targetW, targetH, VK_FORMAT_R16G16B16A16_SFLOAT, _ssrImages[i], _ssrImageViews[i],
                                _ssrMemories[i]))
        {
            LOG_ERROR("[{0}] Failed to create GI/SSR VkImages {1}", _name, i);
            ReleaseGiImages();
            return false;
        }

        _giImageInitialized[i] = false;
        _ssrImageInitialized[i] = false;
    }

    _giWidth = targetW;
    _giHeight = targetH;
    _resetGiHistory = true;
    return true;
}

void NL_Vk::ReleaseGiImages()
{
    if (_halfColorDepthView != VK_NULL_HANDLE || _halfColorDepthImage != VK_NULL_HANDLE ||
        _halfColorDepthMemory != VK_NULL_HANDLE || _halfNormalView != VK_NULL_HANDLE ||
        _halfNormalImage != VK_NULL_HANDLE || _halfNormalMemory != VK_NULL_HANDLE)
    {
        DeferredRelease::Enqueue(
            [device = _device, cdView = _halfColorDepthView, cdImg = _halfColorDepthImage,
             cdMem = _halfColorDepthMemory, nView = _halfNormalView, nImg = _halfNormalImage, nMem = _halfNormalMemory]()
            {
                if (cdView != VK_NULL_HANDLE)
                    vkDestroyImageView(device, cdView, nullptr);
                if (cdImg != VK_NULL_HANDLE)
                    vkDestroyImage(device, cdImg, nullptr);
                if (cdMem != VK_NULL_HANDLE)
                    vkFreeMemory(device, cdMem, nullptr);
                if (nView != VK_NULL_HANDLE)
                    vkDestroyImageView(device, nView, nullptr);
                if (nImg != VK_NULL_HANDLE)
                    vkDestroyImage(device, nImg, nullptr);
                if (nMem != VK_NULL_HANDLE)
                    vkFreeMemory(device, nMem, nullptr);
            },
            (uint64_t) _device);
    }

    _halfColorDepthView = VK_NULL_HANDLE;
    _halfColorDepthImage = VK_NULL_HANDLE;
    _halfColorDepthMemory = VK_NULL_HANDLE;
    _halfColorDepthInit = false;

    _halfNormalView = VK_NULL_HANDLE;
    _halfNormalImage = VK_NULL_HANDLE;
    _halfNormalMemory = VK_NULL_HANDLE;
    _halfNormalInit = false;

    for (int i = 0; i < 2; ++i)
    {
        if (_giImageViews[i] != VK_NULL_HANDLE || _giImages[i] != VK_NULL_HANDLE || _giMemories[i] != VK_NULL_HANDLE ||
            _ssrImageViews[i] != VK_NULL_HANDLE || _ssrImages[i] != VK_NULL_HANDLE || _ssrMemories[i] != VK_NULL_HANDLE)
        {
            DeferredRelease::Enqueue(
                [device = _device, giView = _giImageViews[i], giImg = _giImages[i], giMem = _giMemories[i],
                 ssrView = _ssrImageViews[i], ssrImg = _ssrImages[i], ssrMem = _ssrMemories[i]]()
                {
                    if (giView != VK_NULL_HANDLE)
                        vkDestroyImageView(device, giView, nullptr);
                    if (giImg != VK_NULL_HANDLE)
                        vkDestroyImage(device, giImg, nullptr);
                    if (giMem != VK_NULL_HANDLE)
                        vkFreeMemory(device, giMem, nullptr);
                    if (ssrView != VK_NULL_HANDLE)
                        vkDestroyImageView(device, ssrView, nullptr);
                    if (ssrImg != VK_NULL_HANDLE)
                        vkDestroyImage(device, ssrImg, nullptr);
                    if (ssrMem != VK_NULL_HANDLE)
                        vkFreeMemory(device, ssrMem, nullptr);
                },
                (uint64_t) _device);
        }

        _giImageViews[i] = VK_NULL_HANDLE;
        _giImages[i] = VK_NULL_HANDLE;
        _giMemories[i] = VK_NULL_HANDLE;
        _giImageInitialized[i] = false;

        _ssrImageViews[i] = VK_NULL_HANDLE;
        _ssrImages[i] = VK_NULL_HANDLE;
        _ssrMemories[i] = VK_NULL_HANDLE;
        _ssrImageInitialized[i] = false;
    }

    _giWidth = 0;
    _giHeight = 0;
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

    // 1. Composite pipeline (UBO 0, 8x combined image samplers 1..8, storage image 9)
    std::vector<VkDescriptorSetLayoutBinding> compBindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
    };
    CreateLayouts(compBindings);

    std::vector<VkDescriptorPoolSize> compPoolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight }
    };
    CreateDescriptorPool(compPoolSizes, _maxFramesInFlight);
    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);

    std::vector<char> compCode(nl_composite_spv, nl_composite_spv + sizeof(nl_composite_spv));
    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, compCode))
    {
        LOG_ERROR("[{0}] Failed to create Composite pipeline", _name);
        return;
    }

    // 2. Prep pipeline (UBO 0, 3x combined image samplers 1..3, 2x storage images 4..5)
    {
        std::vector<VkDescriptorSetLayoutBinding> prepBindings = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        };

        VkDescriptorSetLayoutCreateInfo prepLayoutInfo {};
        prepLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        prepLayoutInfo.bindingCount = static_cast<uint32_t>(prepBindings.size());
        prepLayoutInfo.pBindings = prepBindings.data();
        if (vkCreateDescriptorSetLayout(_device, &prepLayoutInfo, nullptr, &_descriptorSetLayoutPrep) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create Prep descriptor set layout", _name);
            return;
        }

        VkPipelineLayoutCreateInfo prepPipeLayoutInfo {};
        prepPipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        prepPipeLayoutInfo.setLayoutCount = 1;
        prepPipeLayoutInfo.pSetLayouts = &_descriptorSetLayoutPrep;
        if (vkCreatePipelineLayout(_device, &prepPipeLayoutInfo, nullptr, &_pipelineLayoutPrep) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create Prep pipeline layout", _name);
            return;
        }

        std::vector<VkDescriptorPoolSize> prepPoolSizes = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * _maxFramesInFlight }
        };
        VkDescriptorPoolCreateInfo prepPoolInfo {};
        prepPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        prepPoolInfo.poolSizeCount = static_cast<uint32_t>(prepPoolSizes.size());
        prepPoolInfo.pPoolSizes = prepPoolSizes.data();
        prepPoolInfo.maxSets = _maxFramesInFlight;
        if (vkCreateDescriptorPool(_device, &prepPoolInfo, nullptr, &_descriptorPoolPrep) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create Prep descriptor pool", _name);
            return;
        }

        Shader_Vk::CreateDescriptorSets(_descriptorSetLayoutPrep, _descriptorPoolPrep, _descriptorSetsPrep);

        _constantStridePrep = UniformStride(_physicalDevice, sizeof(NLPrepInternalConstants));
        const VkDeviceSize prepBufSize = _constantStridePrep * _maxFramesInFlight;
        if (Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_constantBufferPrep, &_constantBufferMemoryPrep,
                                            prepBufSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            vkMapMemory(_device, _constantBufferMemoryPrep, 0, prepBufSize, 0, &_mappedConstantBufferPrep);
        }

        std::vector<char> prepCode(nl_prep_spv, nl_prep_spv + sizeof(nl_prep_spv));
        if (!CreateComputePipeline(_device, _pipelineLayoutPrep, &_pipelinePrep, prepCode))
        {
            LOG_ERROR("[{0}] Failed to create Prep pipeline", _name);
            return;
        }
    }

    // 3. GI+SSR temporal pipeline (UBO 0, 5x combined image samplers 1..5, 2x storage images 6..7)
    {
        std::vector<VkDescriptorSetLayoutBinding> giBindings = {
            CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            CreateBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            CreateBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        };

        VkDescriptorSetLayoutCreateInfo giLayoutInfo {};
        giLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        giLayoutInfo.bindingCount = static_cast<uint32_t>(giBindings.size());
        giLayoutInfo.pBindings = giBindings.data();
        if (vkCreateDescriptorSetLayout(_device, &giLayoutInfo, nullptr, &_descriptorSetLayoutGi) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create GI descriptor set layout", _name);
            return;
        }

        VkPipelineLayoutCreateInfo giPipeLayoutInfo {};
        giPipeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        giPipeLayoutInfo.setLayoutCount = 1;
        giPipeLayoutInfo.pSetLayouts = &_descriptorSetLayoutGi;
        if (vkCreatePipelineLayout(_device, &giPipeLayoutInfo, nullptr, &_pipelineLayoutGi) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create GI pipeline layout", _name);
            return;
        }

        std::vector<VkDescriptorPoolSize> giPoolSizes = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * _maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * _maxFramesInFlight }
        };
        VkDescriptorPoolCreateInfo giPoolInfo {};
        giPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        giPoolInfo.poolSizeCount = static_cast<uint32_t>(giPoolSizes.size());
        giPoolInfo.pPoolSizes = giPoolSizes.data();
        giPoolInfo.maxSets = _maxFramesInFlight;
        if (vkCreateDescriptorPool(_device, &giPoolInfo, nullptr, &_descriptorPoolGi) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create GI descriptor pool", _name);
            return;
        }

        Shader_Vk::CreateDescriptorSets(_descriptorSetLayoutGi, _descriptorPoolGi, _descriptorSetsGi);

        _constantStrideGi = UniformStride(_physicalDevice, sizeof(NLGiInternalConstants));
        const VkDeviceSize giBufSize = _constantStrideGi * _maxFramesInFlight;
        if (Shader_Vk::CreateBufferResource(_device, _physicalDevice, &_constantBufferGi, &_constantBufferMemoryGi,
                                            giBufSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            vkMapMemory(_device, _constantBufferMemoryGi, 0, giBufSize, 0, &_mappedConstantBufferGi);
        }

        std::vector<char> giCode(nl_gi_temporal_spv, nl_gi_temporal_spv + sizeof(nl_gi_temporal_spv));
        if (!CreateComputePipeline(_device, _pipelineLayoutGi, &_pipelineGi, giCode))
        {
            LOG_ERROR("[{0}] Failed to create GI pipeline", _name);
            return;
        }
    }

    _init = true;
}

NL_Vk::~NL_Vk()
{
    ReleaseGiImages();

    DeferredRelease::Enqueue(
        [device = _device, pipelinePrep = _pipelinePrep, descriptorPoolPrep = _descriptorPoolPrep,
         descriptorSetLayoutPrep = _descriptorSetLayoutPrep, pipelineLayoutPrep = _pipelineLayoutPrep,
         constantBufferPrep = _constantBufferPrep, constantBufferMemoryPrep = _constantBufferMemoryPrep,
         pipelineGi = _pipelineGi, descriptorPoolGi = _descriptorPoolGi, descriptorSetLayoutGi = _descriptorSetLayoutGi,
         pipelineLayoutGi = _pipelineLayoutGi, constantBufferGi = _constantBufferGi,
         constantBufferMemoryGi = _constantBufferMemoryGi]()
        {
            if (pipelinePrep != VK_NULL_HANDLE)
                vkDestroyPipeline(device, pipelinePrep, nullptr);
            if (descriptorPoolPrep != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptorPoolPrep, nullptr);
            if (descriptorSetLayoutPrep != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptorSetLayoutPrep, nullptr);
            if (pipelineLayoutPrep != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, pipelineLayoutPrep, nullptr);
            if (constantBufferPrep != VK_NULL_HANDLE)
                vkDestroyBuffer(device, constantBufferPrep, nullptr);
            if (constantBufferMemoryPrep != VK_NULL_HANDLE)
                vkFreeMemory(device, constantBufferMemoryPrep, nullptr);
            if (pipelineGi != VK_NULL_HANDLE)
                vkDestroyPipeline(device, pipelineGi, nullptr);
            if (descriptorPoolGi != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptorPoolGi, nullptr);
            if (descriptorSetLayoutGi != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptorSetLayoutGi, nullptr);
            if (pipelineLayoutGi != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, pipelineLayoutGi, nullptr);
            if (constantBufferGi != VK_NULL_HANDLE)
                vkDestroyBuffer(device, constantBufferGi, nullptr);
            if (constantBufferMemoryGi != VK_NULL_HANDLE)
                vkFreeMemory(device, constantBufferMemoryGi, nullptr);
        },
        (uint64_t) _device);
}

void NL_Vk::UpdatePrepDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                    VkImageView colorView, VkImageView depthView, VkImageView normalsView,
                                    VkImageView outColorDepthView, VkImageView outNormalView)
{
    VkDescriptorImageInfo colorInfo { _textureSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo depthInfo { _textureSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo normInfo { _textureSampler, normalsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outCdInfo { VK_NULL_HANDLE, outColorDepthView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo outNInfo { VK_NULL_HANDLE, outNormalView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkWriteDescriptorSet> writes = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 3, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outCdInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outNInfo, nullptr, nullptr }
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void NL_Vk::UpdateGiDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                  VkImageView halfColorDepthView, VkImageView halfNormalView, VkImageView motionView,
                                  VkImageView prevGiView, VkImageView prevSsrView, VkImageView outGiView,
                                  VkImageView outSsrView)
{
    VkDescriptorImageInfo cdInfo { _textureSampler, halfColorDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo normInfo { _textureSampler, halfNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo motionInfo { _textureSampler, motionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo prevGiInfo { _textureSampler, prevGiView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo prevSsrInfo { _textureSampler, prevSsrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outGiInfo { VK_NULL_HANDLE, outGiView, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo outSsrInfo { VK_NULL_HANDLE, outSsrView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkWriteDescriptorSet> writes = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cdInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 3, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &motionInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 4, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &prevGiInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 5, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &prevSsrInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outGiInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outSsrInfo, nullptr, nullptr }
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void NL_Vk::UpdateCompositeDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                         VkImageView colorView, VkImageView halfColorDepthView, VkImageView motionView,
                                         VkImageView giView, VkImageView halfNormalView, VkImageView albedoView,
                                         VkImageView roughnessView, VkImageView ssrView, VkImageView outColorView)
{
    VkDescriptorImageInfo colorInfo { _textureSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo cdInfo { _textureSampler, halfColorDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo motionInfo { _textureSampler, motionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo giInfo { _textureSampler, giView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo normInfo { _textureSampler, halfNormalView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo albedoInfo { _textureSampler, albedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo roughInfo { _textureSampler, roughnessView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo ssrInfo { _textureSampler, ssrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo { VK_NULL_HANDLE, outColorView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkWriteDescriptorSet> writes = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cdInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 3, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &motionInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 4, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &giInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 5, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 6, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &albedoInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 7, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &roughInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 8, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ssrInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outInfo, nullptr, nullptr }
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool NL_Vk::Dispatch(VkDevice InDevice, VkCommandBuffer InCmdList, NeuralLookConstants InConstants,
                     const VkImageInfo& InResourceInfo, const VkImageInfo& InMotionVectorsInfo,
                     const VkImageInfo& OutResourceInfo, VkImageInfo* InDepthInfo, VkImageInfo* InGBufferNormals,
                     VkImageInfo* InGBufferAlbedo, VkImageInfo* InGBufferRoughness)
{
    if (!_init || InDevice == VK_NULL_HANDLE || InCmdList == VK_NULL_HANDLE || InDepthInfo == nullptr ||
        InDepthInfo->ImageView == VK_NULL_HANDLE || InResourceInfo.ImageView == VK_NULL_HANDLE ||
        OutResourceInfo.ImageView == VK_NULL_HANDLE)
    {
        return false;
    }

    if (!CreateGiImages(OutResourceInfo.Width, OutResourceInfo.Height))
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

    const auto* cfg = Config::Instance();
    const float giIntensity = cfg->NeuralLookGiIntensity.value_or_default();
    const float aoIntensity = cfg->NeuralLookAoIntensity.value_or_default();
    const float ssrIntensity = cfg->NeuralLookSsrIntensity.value_or_default();
    const float volGlow = cfg->NeuralLookVolumetricGlow.value_or_default();
    const int debugView = cfg->NeuralLookDebugView.value_or_default();

    const bool ssrEnabled = (_activeTier > 0) && (ssrIntensity > 1e-4f || volGlow > 1e-4f || debugView == 5);
    const bool runGiPass = (_activeTier > 0) &&
                           (giIntensity > 1e-4f || aoIntensity > 1e-4f || ssrEnabled || debugView == 1 || debugView == 2);

    const VkImageSubresourceRange giRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Pass 0: Half-res G-Proxy Prep
    {
        SetImageLayout(InCmdList, _halfColorDepthImage,
                       _halfColorDepthInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_GENERAL, giRange);
        _halfColorDepthInit = true;

        SetImageLayout(InCmdList, _halfNormalImage,
                       _halfNormalInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_GENERAL, giRange);
        _halfNormalInit = true;

        NLPrepInternalConstants prepConst {};
        prepConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        prepConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        prepConst.HalfWidth = static_cast<int32_t>(_giWidth);
        prepConst.HalfHeight = static_cast<int32_t>(_giHeight);
        prepConst.DepthWidth = static_cast<int32_t>(InDepthInfo->Width);
        prepConst.DepthHeight = static_cast<int32_t>(InDepthInfo->Height);
        FillPrepConstants(prepConst, InConstants, hasNormals, inEncodeMode);

        const VkDescriptorBufferInfo prepBufInfo { _constantBufferPrep, _constantStridePrep * _currentSetIndex,
                                                   sizeof(NLPrepInternalConstants) };
        if (_mappedConstantBufferPrep != nullptr)
            memcpy((uint8_t*) _mappedConstantBufferPrep + prepBufInfo.offset, &prepConst, sizeof(NLPrepInternalConstants));

        UpdatePrepDescriptorSet(_descriptorSetsPrep[_currentSetIndex], prepBufInfo, InResourceInfo.ImageView,
                                InDepthInfo->ImageView,
                                hasNormals ? InGBufferNormals->ImageView : InResourceInfo.ImageView,
                                _halfColorDepthView, _halfNormalView);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelinePrep);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayoutPrep, 0, 1,
                                &_descriptorSetsPrep[_currentSetIndex], 0, nullptr);
        vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);

        SetImageLayout(InCmdList, _halfColorDepthImage, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        SetImageLayout(InCmdList, _halfNormalImage, VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
    }

    _giWriteIdx = (_giWriteIdx + 1) & 1;
    const int prevIdx = _giWriteIdx ^ 1;

    if (!_giImageInitialized[prevIdx])
    {
        SetImageLayout(InCmdList, _giImages[prevIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        _giImageInitialized[prevIdx] = true;
    }

    if (!_ssrImageInitialized[prevIdx])
    {
        SetImageLayout(InCmdList, _ssrImages[prevIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        _ssrImageInitialized[prevIdx] = true;
    }

    const VkImageView mvView =
        (InMotionVectorsInfo.ImageView != VK_NULL_HANDLE) ? InMotionVectorsInfo.ImageView : InResourceInfo.ImageView;

    // Pass 1: Checkerboard Temporal SSGI + SSR
    if (runGiPass)
    {
        const VkImageLayout prevGiLayout =
            _giImageInitialized[_giWriteIdx] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        SetImageLayout(InCmdList, _giImages[_giWriteIdx], prevGiLayout, VK_IMAGE_LAYOUT_GENERAL, giRange);
        _giImageInitialized[_giWriteIdx] = true;

        const VkImageLayout prevSsrLayout =
            _ssrImageInitialized[_giWriteIdx] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        SetImageLayout(InCmdList, _ssrImages[_giWriteIdx], prevSsrLayout, VK_IMAGE_LAYOUT_GENERAL, giRange);
        _ssrImageInitialized[_giWriteIdx] = true;

        NLGiInternalConstants giConst {};
        giConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        giConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        giConst.GiWidth = static_cast<int32_t>(_giWidth);
        giConst.GiHeight = static_cast<int32_t>(_giHeight);
        giConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        giConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);

        NeuralLookConstants giConstIn = InConstants;
        if (_resetGiHistory)
        {
            giConstIn.ResetHistory = true;
            _resetGiHistory = false;
        }
        FillGiConstants(giConst, giConstIn, ssrEnabled);

        const VkDescriptorBufferInfo giBufInfo { _constantBufferGi, _constantStrideGi * _currentSetIndex,
                                                 sizeof(NLGiInternalConstants) };
        if (_mappedConstantBufferGi != nullptr)
            memcpy((uint8_t*) _mappedConstantBufferGi + giBufInfo.offset, &giConst, sizeof(NLGiInternalConstants));

        UpdateGiDescriptorSet(_descriptorSetsGi[_currentSetIndex], giBufInfo, _halfColorDepthView, _halfNormalView,
                              mvView, _giImageViews[prevIdx], _ssrImageViews[prevIdx], _giImageViews[_giWriteIdx],
                              _ssrImageViews[_giWriteIdx]);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineGi);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayoutGi, 0, 1,
                                &_descriptorSetsGi[_currentSetIndex], 0, nullptr);

        vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);

        SetImageLayout(InCmdList, _giImages[_giWriteIdx], VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        SetImageLayout(InCmdList, _ssrImages[_giWriteIdx], VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
    }
    else
    {
        if (!_giImageInitialized[_giWriteIdx])
        {
            SetImageLayout(InCmdList, _giImages[_giWriteIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
            _giImageInitialized[_giWriteIdx] = true;
        }
        if (!_ssrImageInitialized[_giWriteIdx])
        {
            SetImageLayout(InCmdList, _ssrImages[_giWriteIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
            _ssrImageInitialized[_giWriteIdx] = true;
        }
    }

    // Pass 2: Full-res Composite
    {
        NLCompositeInternalConstants compConst {};
        compConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        compConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        compConst.GiWidth = static_cast<int32_t>(_giWidth);
        compConst.GiHeight = static_cast<int32_t>(_giHeight);
        compConst.DepthWidth = static_cast<int32_t>(InDepthInfo->Width);
        compConst.DepthHeight = static_cast<int32_t>(InDepthInfo->Height);
        compConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        compConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);

        FillCompositeConstants(compConst, InConstants, runGiPass, ssrEnabled, hasNormals, hasAlbedo, hasRoughness,
                               inEncodeMode, outEncodeMode);

        const VkDescriptorBufferInfo compBufInfo = WriteConstants(&compConst, sizeof(NLCompositeInternalConstants));

        UpdateCompositeDescriptorSet(_descriptorSets[_currentSetIndex], compBufInfo, InResourceInfo.ImageView,
                                     _halfColorDepthView, mvView, _giImageViews[_giWriteIdx], _halfNormalView,
                                     hasAlbedo ? InGBufferAlbedo->ImageView : InResourceInfo.ImageView,
                                     hasRoughness ? InGBufferRoughness->ImageView : InResourceInfo.ImageView,
                                     _ssrImageViews[_giWriteIdx], OutResourceInfo.ImageView);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1,
                                &_descriptorSets[_currentSetIndex], 0, nullptr);

        vkCmdDispatch(InCmdList, (OutResourceInfo.Width + 15u) / 16u, (OutResourceInfo.Height + 15u) / 16u, 1);
    }

    return true;
}
