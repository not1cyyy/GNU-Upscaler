#include "pch.h"

#include "NL_Vk.h"
#include "precompile/nl_composite_Shader_Vk.h"
#include "precompile/nl_gi_temporal_Shader_Vk.h"

#include <State.h>
#include <misc/DeferredRelease.h>

bool NL_Vk::CreateGiImages(uint32_t width, uint32_t height)
{
    const uint32_t targetW = std::max(1u, width / 2u);
    const uint32_t targetH = std::max(1u, height / 2u);

    if (_giImages[0] != VK_NULL_HANDLE && _giImages[1] != VK_NULL_HANDLE && _giWidth == targetW && _giHeight == targetH)
        return true;

    ReleaseGiImages();

    for (int i = 0; i < 2; ++i)
    {
        VkImageCreateInfo imageInfo {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { targetW, targetH, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

        if (vkCreateImage(_device, &imageInfo, nullptr, &_giImages[i]) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create GI VkImage {1}", _name, i);
            ReleaseGiImages();
            return false;
        }

        VkMemoryRequirements memReq {};
        vkGetImageMemoryRequirements(_device, _giImages[i], &memReq);

        VkMemoryAllocateInfo allocInfo {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReq.size;
        allocInfo.memoryTypeIndex =
            FindMemoryType(_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(_device, &allocInfo, nullptr, &_giMemories[i]) != VK_SUCCESS ||
            vkBindImageMemory(_device, _giImages[i], _giMemories[i], 0) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to allocate/bind GI VkImage memory {1}", _name, i);
            ReleaseGiImages();
            return false;
        }

        VkImageViewCreateInfo viewInfo {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = _giImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        if (vkCreateImageView(_device, &viewInfo, nullptr, &_giImageViews[i]) != VK_SUCCESS)
        {
            LOG_ERROR("[{0}] Failed to create GI VkImageView {1}", _name, i);
            ReleaseGiImages();
            return false;
        }

        _giImageInitialized[i] = false;
    }

    _giWidth = targetW;
    _giHeight = targetH;
    _resetGiHistory = true;
    return true;
}

void NL_Vk::ReleaseGiImages()
{
    for (int i = 0; i < 2; ++i)
    {
        if (_giImageViews[i] != VK_NULL_HANDLE || _giImages[i] != VK_NULL_HANDLE || _giMemories[i] != VK_NULL_HANDLE)
        {
            DeferredRelease::Enqueue(
                [device = _device, view = _giImageViews[i], img = _giImages[i], mem = _giMemories[i]]()
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

        _giImageViews[i] = VK_NULL_HANDLE;
        _giImages[i] = VK_NULL_HANDLE;
        _giMemories[i] = VK_NULL_HANDLE;
        _giImageInitialized[i] = false;
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

    CreateSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    CreateConstantBuffer(sizeof(NLCompositeInternalConstants));

    // 1. Composite pipeline (UBO 0, 7x combined image samplers 1..7, storage image 8)
    std::vector<VkDescriptorSetLayoutBinding> compBindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
    };
    CreateLayouts(compBindings);

    std::vector<VkDescriptorPoolSize> compPoolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 * _maxFramesInFlight },
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

    // 2. GI temporal pipeline (UBO 0, 5x combined image samplers 1..5, storage image 6)
    std::vector<VkDescriptorSetLayoutBinding> giBindings = {
        CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        CreateBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
        CreateBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
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
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight }
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

    _init = true;
}

NL_Vk::~NL_Vk()
{
    ReleaseGiImages();

    DeferredRelease::Enqueue(
        [device = _device, pipelineGi = _pipelineGi, descriptorPoolGi = _descriptorPoolGi,
         descriptorSetLayoutGi = _descriptorSetLayoutGi, pipelineLayoutGi = _pipelineLayoutGi,
         constantBufferGi = _constantBufferGi, constantBufferMemoryGi = _constantBufferMemoryGi]()
        {
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

void NL_Vk::UpdateGiDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                  VkImageView colorView, VkImageView depthView, VkImageView motionView,
                                  VkImageView prevGiView, VkImageView normalsView, VkImageView outGiView)
{
    VkDescriptorImageInfo colorInfo { _textureSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo depthInfo { _textureSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo motionInfo { _textureSampler, motionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo prevInfo { _textureSampler, prevGiView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo normInfo { _textureSampler, normalsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo { VK_NULL_HANDLE, outGiView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkWriteDescriptorSet> writes = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 3, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &motionInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 4, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &prevInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 5, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          &outInfo, nullptr, nullptr }
    };

    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void NL_Vk::UpdateCompositeDescriptorSet(VkDescriptorSet descriptorSet, VkDescriptorBufferInfo bufferInfo,
                                         VkImageView colorView, VkImageView depthView, VkImageView motionView,
                                         VkImageView giView, VkImageView normalsView, VkImageView albedoView,
                                         VkImageView roughnessView, VkImageView outColorView)
{
    VkDescriptorImageInfo colorInfo { _textureSampler, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo depthInfo { _textureSampler, depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo motionInfo { _textureSampler, motionView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo giInfo { _textureSampler, giView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo normInfo { _textureSampler, normalsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo albedoInfo { _textureSampler, albedoView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo roughInfo { _textureSampler, roughnessView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo outInfo { VK_NULL_HANDLE, outColorView, VK_IMAGE_LAYOUT_GENERAL };

    std::vector<VkWriteDescriptorSet> writes = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          nullptr, &bufferInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr },
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
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 8, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
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

    const float giIntensity = Config::Instance()->NeuralLookGiIntensity.value_or_default();
    const float aoIntensity = Config::Instance()->NeuralLookAoIntensity.value_or_default();
    const int debugView = Config::Instance()->NeuralLookDebugView.value_or_default();
    const bool runGiPass = (_activeTier > 0) && (giIntensity > 1e-4f || aoIntensity > 1e-4f || debugView == 1 || debugView == 2);

    _giWriteIdx = (_giWriteIdx + 1) & 1;
    const int prevIdx = _giWriteIdx ^ 1;
    const VkImageSubresourceRange giRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Ensure prev image is in SHADER_READ_ONLY_OPTIMAL
    if (!_giImageInitialized[prevIdx])
    {
        SetImageLayout(InCmdList, _giImages[prevIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        _giImageInitialized[prevIdx] = true;
    }

    if (runGiPass)
    {
        const VkImageLayout prevWriteLayout =
            _giImageInitialized[_giWriteIdx] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        SetImageLayout(InCmdList, _giImages[_giWriteIdx], prevWriteLayout, VK_IMAGE_LAYOUT_GENERAL, giRange);
        _giImageInitialized[_giWriteIdx] = true;

        NLGiInternalConstants giConst {};
        giConst.OutputWidth = static_cast<int32_t>(OutResourceInfo.Width);
        giConst.OutputHeight = static_cast<int32_t>(OutResourceInfo.Height);
        giConst.GiWidth = static_cast<int32_t>(_giWidth);
        giConst.GiHeight = static_cast<int32_t>(_giHeight);
        giConst.DepthWidth = static_cast<int32_t>(InDepthInfo->Width);
        giConst.DepthHeight = static_cast<int32_t>(InDepthInfo->Height);
        giConst.MotionWidth = static_cast<int32_t>(InMotionVectorsInfo.Width);
        giConst.MotionHeight = static_cast<int32_t>(InMotionVectorsInfo.Height);

        NeuralLookConstants giConstIn = InConstants;
        if (_resetGiHistory)
        {
            giConstIn.ResetHistory = true;
            _resetGiHistory = false;
        }
        FillGiConstants(giConst, giConstIn, hasNormals, inEncodeMode);

        const VkDescriptorBufferInfo giBufInfo { _constantBufferGi, _constantStrideGi * _currentSetIndex,
                                                 sizeof(NLGiInternalConstants) };
        if (_mappedConstantBufferGi != nullptr)
            memcpy((uint8_t*) _mappedConstantBufferGi + giBufInfo.offset, &giConst, sizeof(NLGiInternalConstants));

        const VkImageView mvView =
            (InMotionVectorsInfo.ImageView != VK_NULL_HANDLE) ? InMotionVectorsInfo.ImageView : InResourceInfo.ImageView;

        UpdateGiDescriptorSet(_descriptorSetsGi[_currentSetIndex], giBufInfo, InResourceInfo.ImageView,
                              InDepthInfo->ImageView, mvView, _giImageViews[prevIdx],
                              hasNormals ? InGBufferNormals->ImageView : InResourceInfo.ImageView,
                              _giImageViews[_giWriteIdx]);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineGi);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayoutGi, 0, 1,
                                &_descriptorSetsGi[_currentSetIndex], 0, nullptr);

        vkCmdDispatch(InCmdList, (_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);

        SetImageLayout(InCmdList, _giImages[_giWriteIdx], VK_IMAGE_LAYOUT_GENERAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
    }
    else if (!_giImageInitialized[_giWriteIdx])
    {
        SetImageLayout(InCmdList, _giImages[_giWriteIdx], VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, giRange);
        _giImageInitialized[_giWriteIdx] = true;
    }

    // Composite pass
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

        FillCompositeConstants(compConst, InConstants, runGiPass, hasNormals, hasAlbedo, hasRoughness, inEncodeMode,
                               outEncodeMode);

        const VkDescriptorBufferInfo compBufInfo = WriteConstants(&compConst, sizeof(NLCompositeInternalConstants));
        const VkImageView mvView =
            (InMotionVectorsInfo.ImageView != VK_NULL_HANDLE) ? InMotionVectorsInfo.ImageView : InResourceInfo.ImageView;

        UpdateCompositeDescriptorSet(_descriptorSets[_currentSetIndex], compBufInfo, InResourceInfo.ImageView,
                                     InDepthInfo->ImageView, mvView, _giImageViews[_giWriteIdx],
                                     hasNormals ? InGBufferNormals->ImageView : InResourceInfo.ImageView,
                                     hasAlbedo ? InGBufferAlbedo->ImageView : InResourceInfo.ImageView,
                                     hasRoughness ? InGBufferRoughness->ImageView : InResourceInfo.ImageView,
                                     OutResourceInfo.ImageView);

        vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1,
                                &_descriptorSets[_currentSetIndex], 0, nullptr);

        vkCmdDispatch(InCmdList, (OutResourceInfo.Width + 15u) / 16u, (OutResourceInfo.Height + 15u) / 16u, 1);
    }

    return true;
}
