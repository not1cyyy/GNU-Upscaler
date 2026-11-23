#include "pch.h"
#include "HCC_Dx12.h"
#include "HCC_Common.h"

#include <Config.h>
#include <State.h>
#include "precompile/HCC_Shader.h"

void HCC_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
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

bool HCC_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless, ID3D12Resource* present,
                        D3D12_RESOURCE_STATES hudlessState, D3D12_RESOURCE_STATES presentState)
{
    if (!_init || _device == nullptr || hudless == nullptr || present == nullptr || cmdList == nullptr)
        return false;

    const auto pDesc = present->GetDesc();
    const auto hDesc = hudless->GetDesc();
    if (hDesc.Width != pDesc.Width || hDesc.Height != pDesc.Height)
        return false;

    const DXGI_FORMAT bufferFormat = StripSrgbFormat(TranslateTypelessFormats(pDesc.Format));

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), cmdList);

    _counter++;
    _counter = _counter % HCC_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    // Follows the present size and format, kept in COPY_DEST between dispatches
    auto previousBuffer = _buffer;
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (!Shader_Dx12::CreateBufferResource(_device, present, D3D12_RESOURCE_STATE_COPY_DEST, &_buffer, resourceFlags, 0,
                                           0, bufferFormat))
        return false;

    if (_buffer != previousBuffer)
        _buffer->SetName(L"HCC_Buffer");

    ResourceBarrier(cmdList, present, presentState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    cmdList->CopyResource(_buffer, present);

    // Make sure present is in D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ResourceBarrier(cmdList, hudless, hudlessState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Create views; only cast typeless resources to avoid D3D12 concrete view-cast failures
    const bool castHudless =
        IsTypelessFormat(hDesc.Format) && AreFormatsCompatible(bufferFormat, hDesc.Format);
    const DXGI_FORMAT hSrvFormat = castHudless ? bufferFormat : TranslateTypelessFormats(hDesc.Format);
    const DXGI_FORMAT pSrvFormat = TranslateTypelessFormats(pDesc.Format);

    CreateShaderResourceView(_device, hudless, currentHeap.GetSrvCPU(0),
                             castHudless ? bufferFormat : DXGI_FORMAT_UNKNOWN);
    CreateShaderResourceView(_device, present, currentHeap.GetSrvCPU(1));
    CreateUnorderedAccessView(_device, _buffer, currentHeap.GetUavCPU(0), 0);

    const bool hdrPQ = IsHdrPQActive();
    const auto targetSpace = GetTargetStorageColorSpace(bufferFormat, hdrPQ);

    InternalCompareParams constants {};
    constants.DiffThreshold = GetUiCompareDiffThreshold();
    constants.PinkAmount = 0.6f;
    constants.HudlessEncodeMode = GetColorEncodeMode(GetSrvColorSpace(hSrvFormat, hdrPQ), targetSpace);
    constants.PresentEncodeMode = GetColorEncodeMode(GetSrvColorSpace(pSrvFormat, hdrPQ), targetSpace);

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

    auto presentDesc = present->GetDesc();
    UINT dispatchWidth = static_cast<UINT>((presentDesc.Width + InNumThreadsX - 1) / InNumThreadsX);
    UINT dispatchHeight = (presentDesc.Height + InNumThreadsY - 1) / InNumThreadsY;

    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(present, _buffer);

    // Restore resource states
    ResourceBarrier(cmdList, present, D3D12_RESOURCE_STATE_COPY_DEST, presentState);
    ResourceBarrier(cmdList, hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, hudlessState);
    ResourceBarrier(cmdList, _buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    return true;
}

HCC_Dx12::HCC_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
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

    if (!CreateComputePipeline(InDevice, &_pipelineState, HCC_cso, sizeof(HCC_cso), shaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, HCC_NUM_OF_HEAPS);
}

HCC_Dx12::~HCC_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < HCC_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

    DeferredRelease::Release(_buffer);
}
