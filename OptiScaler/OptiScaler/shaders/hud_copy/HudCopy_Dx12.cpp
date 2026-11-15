#include "pch.h"
#include "HudCopy_Dx12.h"
#include "HudCopy_Common.h"

#include <Config.h>
#include <State.h>
#include "precompile/HudCopy_Shader.h"

void HudCopy_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                   D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

bool HudCopy_Dx12::DispatchKernel(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless,
                                  ID3D12Resource* presentSrc, float hudDetectionThreshold, UINT64 width, UINT height)
{
    _counter++;
    _counter = _counter % HudCopy_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, hudless, currentHeap.GetSrvCPU(0));
    CreateShaderResourceView(_device, presentSrc, currentHeap.GetSrvCPU(1));
    CreateUnorderedAccessView(_device, _buffer, currentHeap.GetUavCPU(0), 0);

    InternalHudCopyParams constants {};
    constants.DiffThreshold = hudDetectionThreshold;

    if (!CreateConstantsBuffer(_device, currentHeap, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);

    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = static_cast<UINT>((width + InNumThreadsX - 1) / InNumThreadsX);
    UINT dispatchHeight = (height + InNumThreadsY - 1) / InNumThreadsY;

    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);
    return true;
}

bool HudCopy_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless, ID3D12Resource* present,
                            D3D12_RESOURCE_STATES hudlessState, D3D12_RESOURCE_STATES presentState,
                            float hudDetectionThreshold)
{
    if (!_init || _device == nullptr || hudless == nullptr || present == nullptr || cmdList == nullptr)
        return false;

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), cmdList);

    // Follows the present size and format, kept in COPY_DEST between dispatches
    auto previousBuffer = _buffer;
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (!Shader_Dx12::CreateBufferResource(_device, present, D3D12_RESOURCE_STATE_COPY_DEST, &_buffer, resourceFlags))
        return false;

    if (_buffer != previousBuffer)
        _buffer->SetName(L"HudCopy_Buffer");

    ResourceBarrier(cmdList, present, presentState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    cmdList->CopyResource(_buffer, present);

    // Make sure present is in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ResourceBarrier(cmdList, hudless, hudlessState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    auto presentDesc = present->GetDesc();
    if (!DispatchKernel(cmdList, hudless, present, hudDetectionThreshold, presentDesc.Width, presentDesc.Height))
    {
        ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, presentState);
        ResourceBarrier(cmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState);
        return false;
    }

    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(present, _buffer);

    // Restore resource states
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_COPY_DEST, presentState);
    ResourceBarrier(cmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState);
    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    return true;
}

bool HudCopy_Dx12::PrepareAsyncDispatch(ID3D12GraphicsCommandList* directCmdList, ID3D12Resource* hudless,
                                        ID3D12Resource* present, D3D12_RESOURCE_STATES hudlessState,
                                        D3D12_RESOURCE_STATES presentState)
{
    if (!_init || _device == nullptr || hudless == nullptr || present == nullptr || directCmdList == nullptr)
        return false;

    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    auto prevBuffer = _buffer;
    if (!Shader_Dx12::CreateBufferResource(_device, present, D3D12_RESOURCE_STATE_COPY_DEST, &_buffer, resourceFlags))
        return false;
    if (_buffer != prevBuffer)
        _buffer->SetName(L"HudCopy_Buffer");

    auto prevSrcBuffer = _srcBuffer;
    if (!Shader_Dx12::CreateBufferResource(_device, present, D3D12_RESOURCE_STATE_COPY_DEST, &_srcBuffer,
                                           resourceFlags))
        return false;
    if (_srcBuffer != prevSrcBuffer)
        _srcBuffer->SetName(L"HudCopy_SrcBuffer");

    auto presentDesc = present->GetDesc();
    _asyncWidth = presentDesc.Width;
    _asyncHeight = presentDesc.Height;

    // Copy swapchain backbuffer on Direct queue so Compute queue never transitions PRESENT/graphics-only states
    ResourceBarrier(directCmdList, present, presentState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    directCmdList->CopyResource(_buffer, present);
    directCmdList->CopyResource(_srcBuffer, present);
    ResourceBarrier(directCmdList, present, D3D12_RESOURCE_STATE_COPY_SOURCE, presentState);

    ResourceBarrier(directCmdList, _srcBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResourceBarrier(directCmdList, _buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ResourceBarrier(directCmdList, hudless, hudlessState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    return true;
}

bool HudCopy_Dx12::DispatchAsyncCompute(ID3D12GraphicsCommandList* computeCmdList, ID3D12Resource* hudless,
                                        float hudDetectionThreshold)
{
    if (!_init || _device == nullptr || hudless == nullptr || _buffer == nullptr || _srcBuffer == nullptr ||
        computeCmdList == nullptr)
        return false;

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), computeCmdList);

    const bool ok = DispatchKernel(computeCmdList, hudless, _srcBuffer, hudDetectionThreshold, _asyncWidth, _asyncHeight);

    ResourceBarrier(computeCmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(computeCmdList, _srcBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

    return ok;
}

void HudCopy_Dx12::CompleteAsyncDispatch(ID3D12GraphicsCommandList* directCmdList, ID3D12Resource* hudless,
                                         ID3D12Resource* present, D3D12_RESOURCE_STATES hudlessState,
                                         D3D12_RESOURCE_STATES presentState)
{
    ResourceBarrier(directCmdList, present, presentState, D3D12_RESOURCE_STATE_COPY_DEST);
    directCmdList->CopyResource(present, _buffer);
    ResourceBarrier(directCmdList, present, D3D12_RESOURCE_STATE_COPY_DEST, presentState);
    ResourceBarrier(directCmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState);
    ResourceBarrier(directCmdList, _buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
}

HudCopy_Dx12::HudCopy_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 2, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, HudCopy_cso, sizeof(HudCopy_cso), shaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, HudCopy_NUM_OF_HEAPS);
}

HudCopy_Dx12::~HudCopy_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < HudCopy_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

    DeferredRelease::Release(_buffer);
    DeferredRelease::Release(_srcBuffer);
}
