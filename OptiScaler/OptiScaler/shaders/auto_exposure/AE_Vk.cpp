#include "pch.h"
#include "AE_Vk.h"

#include "precompile/AE_Shader_Vk.h"

#include <State.h>
#include <misc/DeferredRelease.h>
#include <algorithm>

bool AE_Vk::EnsureDataBuffer(uint32_t InElements)
{
    if (_dataBuffer != VK_NULL_HANDLE && _dataElements >= InElements)
        return true;

    if (_dataBuffer != VK_NULL_HANDLE || _dataMemory != VK_NULL_HANDLE)
    {
        // Previous frames in flight can still reference the storage buffer
        DeferredRelease::Enqueue(
            [device = _device, buffer = _dataBuffer, memory = _dataMemory]()
            {
                if (buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(device, buffer, nullptr);

                if (memory != VK_NULL_HANDLE)
                    vkFreeMemory(device, memory, nullptr);
            },
            (uint64_t) _device);

        _dataBuffer = VK_NULL_HANDLE;
        _dataMemory = VK_NULL_HANDLE;
        _dataElements = 0;
        _historyValid = false;
    }

    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    if (!CreateBufferResource(_device, _physicalDevice, &newBuffer, &newMemory,
                              static_cast<VkDeviceSize>(InElements) * sizeof(float),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
        LOG_ERROR("[{0}] Failed to create auto exposure storage buffer", _name);
        return false;
    }

    _dataBuffer = newBuffer;
    _dataMemory = newMemory;
    _dataElements = InElements;

    return true;
}

AE_Vk::AE_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice)
    : Shader_Vk(InName, InDevice, InPhysicalDevice)
{
    if (InDevice == VK_NULL_HANDLE || InPhysicalDevice == VK_NULL_HANDLE)
    {
        LOG_ERROR("InDevice or InPhysicalDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // 2 dispatches (Pass 0 + Pass 1) per frame need distinct UBO slots and descriptor sets
    _maxFramesInFlight = 16;

    CreateConstantBuffer(sizeof(InternalConstants));
    if (_constantBuffer == VK_NULL_HANDLE)
    {
        LOG_ERROR("[{0}] Failed to create constant buffer!", _name);
        return;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings = { CreateBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                                                           CreateBinding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
                                                           CreateBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                                           CreateBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) };
    CreateLayouts(bindings);
    if (_descriptorSetLayout == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE)
        return;

    std::vector<VkDescriptorPoolSize> poolSizes = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, _maxFramesInFlight },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, _maxFramesInFlight } };
    CreateDescriptorPool(poolSizes, _maxFramesInFlight);
    if (_descriptorPool == VK_NULL_HANDLE)
        return;

    CreateDescriptorSets(_descriptorSetLayout, _descriptorPool, _descriptorSets);
    if (_descriptorSets.empty())
        return;

    std::vector<char> shaderCode(AE_spv, AE_spv + sizeof(AE_spv));
    if (!CreateComputePipeline(_device, _pipelineLayout, &_pipeline, shaderCode))
    {
        LOG_ERROR("[{0}] Failed to create pipeline!", _name);
        return;
    }

    if (!CreateImageResource(1, 1, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
    {
        LOG_ERROR("[{0}] Failed to create exposure image!", _name);
        return;
    }

    _exposureLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _init = true;
}

AE_Vk::~AE_Vk()
{
    if (_dataBuffer != VK_NULL_HANDLE || _dataMemory != VK_NULL_HANDLE)
    {
        DeferredRelease::Enqueue(
            [device = _device, buffer = _dataBuffer, memory = _dataMemory]()
            {
                if (buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(device, buffer, nullptr);

                if (memory != VK_NULL_HANDLE)
                    vkFreeMemory(device, memory, nullptr);
            },
            (uint64_t) _device);

        _dataBuffer = VK_NULL_HANDLE;
        _dataMemory = VK_NULL_HANDLE;
    }
}

bool AE_Vk::Dispatch(VkCommandBuffer InCmdList, VkImageView InColorView, uint32_t InRenderWidth,
                     uint32_t InRenderHeight, float InPreExposure, float InDeltaTimeSeconds, bool InReset)
{
    if (!CanRender() || InCmdList == VK_NULL_HANDLE || InColorView == VK_NULL_HANDLE || InRenderWidth == 0 ||
        InRenderHeight == 0)
        return false;

    const uint32_t groupCountX = (InRenderWidth + AE_TILE - 1) / AE_TILE;
    const uint32_t groupCountY = (InRenderHeight + AE_TILE - 1) / AE_TILE;
    const uint32_t groupCount = groupCountX * groupCountY;

    if (!EnsureDataBuffer(groupCount + 1))
        return false;

    VkImageMemoryBarrier imageBarrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = _intermediateImage;
    imageBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (_exposureLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        imageBarrier.oldLayout = _exposureLayout;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.srcAccessMask = (_exposureLayout == VK_IMAGE_LAYOUT_UNDEFINED) ? 0 : VK_ACCESS_SHADER_READ_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &imageBarrier);
        _exposureLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Previous pass 1 may still be reading/writing Data[0] and tile sums
    SetBufferState(InCmdList, _dataBuffer, VK_WHOLE_SIZE,
                   _historyValid ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT) : 0,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    InternalConstants constants {};
    constants.RenderWidth = InRenderWidth;
    constants.RenderHeight = InRenderHeight;
    constants.GroupCountX = groupCountX;
    constants.GroupCount = groupCount;
    constants.DeltaTime = std::clamp(InDeltaTimeSeconds, 0.0f, 1.0f);
    constants.PreExposure = InPreExposure > 0.0f ? InPreExposure : 1.0f;
    constants.Reset = (InReset || !_historyValid) ? 1u : 0u;

    vkCmdBindPipeline(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);

    const VkDescriptorImageInfo colorInfo { VK_NULL_HANDLE, InColorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    const VkDescriptorBufferInfo dataInfo { _dataBuffer, 0, VK_WHOLE_SIZE };
    const VkDescriptorImageInfo exposureInfo { VK_NULL_HANDLE, _intermediateImageView, VK_IMAGE_LAYOUT_GENERAL };

    auto dispatchPass = [&](uint32_t passIndex, uint32_t gx, uint32_t gy)
    {
        _currentSetIndex = (_currentSetIndex + 1) % _maxFramesInFlight;
        constants.PassIndex = passIndex;

        const VkDescriptorSet descriptorSet = _descriptorSets[_currentSetIndex];
        const auto bufferInfo = WriteConstants(&constants, sizeof(constants));

        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 0, 0, 1,
              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 1, 0, 1,
              VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &colorInfo, nullptr, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 2, 0, 1,
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dataInfo, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet, 3, 0, 1,
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &exposureInfo, nullptr, nullptr }
        };

        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
        vkCmdBindDescriptorSets(InCmdList, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &descriptorSet, 0,
                                nullptr);
        vkCmdDispatch(InCmdList, gx, gy, 1);
    };

    // Per-tile log luma sums
    dispatchPass(0, groupCountX, groupCountY);

    SetBufferState(InCmdList, _dataBuffer, VK_WHOLE_SIZE, VK_ACCESS_SHADER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    // Average, temporal adaptation and exposure
    dispatchPass(1, 1, 1);

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(InCmdList, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &imageBarrier);
    _exposureLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    _historyValid = true;

    return true;
}
