#include "pch.h"
#include "HudCopy_Dx12.h"
#include "HudCopy_Common.h"

#include <Config.h>
#include <State.h>
#include <shaders/Shader_Common.h>
#include "precompile/HudCopy_Shader.h"

namespace
{
constexpr uint32_t kTileMaskElements = 1024; // 32x32 grid of 128x128px super-tiles (covers up to 4096x4096 natively)

constexpr const char* kTileSkipHudCopyShader = R"(
cbuffer Params : register(b0)
{
    float DiffThreshold;
    uint ScreenWidth;
    uint ScreenHeight;
    uint FrameIndex;
};

Texture2D<float3> Hudless : register(t0);
Texture2D<float3> PresentCopy : register(t1);
RWTexture2D<float3> Present : register(u0);
RWBuffer<uint> TileActive : register(u1);

groupshared uint g_tileHasHud;

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    uint2 tileCoord = uint2(min(groupID.x >> 3, 31u), min(groupID.y >> 3, 31u));
    uint tileIdx = (tileCoord.y << 5) | tileCoord.x;

    uint scanStripe = FrameIndex & 15u;
    bool forceScan = (FrameIndex < 116u) || ((tileCoord.y & 15u) == scanStripe);
    uint lastActive = TileActive[tileIdx];

    if (!forceScan && (lastActive == 0u || (FrameIndex - lastActive) > 16u))
        return;

    if (groupIndex == 0)
        g_tileHasHud = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint2 pixelCoord = dispatchThreadID.xy;
    if (pixelCoord.x < ScreenWidth && pixelCoord.y < ScreenHeight)
    {
        float3 hudless = Hudless.Load(int3(pixelCoord, 0));
        float3 present = PresentCopy.Load(int3(pixelCoord, 0));

        float3 diff = abs(hudless - present);
        float delta = max(max(diff.r, diff.g), diff.b);

        if (delta > 0.0005f)
        {
            float uiMask = smoothstep(DiffThreshold, DiffThreshold * 2.0f, delta);
            float3 ui = (present - hudless) * uiMask;
            Present[pixelCoord] = hudless + ui;
        }

        if (delta > max(DiffThreshold * 0.5f, 0.0005f))
            InterlockedOr(g_tileHasHud, 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    if (groupIndex == 0 && g_tileHasHud != 0u)
        TileActive[tileIdx] = FrameIndex;
}
)";
} // namespace

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

    const bool useTileSkip = Config::Instance()->FGHudCopyTileSkip.value_or_default() &&
                             _tilePipelineState != nullptr && _tileMaskBuffer != nullptr;

    if (useTileSkip)
    {
        if (width != _lastTileWidth || height != _lastTileHeight)
        {
            _lastTileWidth = width;
            _lastTileHeight = height;
            _tileFrameIndex = 100u;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC tileUavDesc {};
        tileUavDesc.Format = DXGI_FORMAT_R32_UINT;
        tileUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        tileUavDesc.Buffer.FirstElement = 0;
        tileUavDesc.Buffer.NumElements = kTileMaskElements;
        _device->CreateUnorderedAccessView(_tileMaskBuffer, nullptr, &tileUavDesc, currentHeap.GetUavCPU(1));

        TileHudCopyParams tileConstants {};
        tileConstants.DiffThreshold = hudDetectionThreshold;
        tileConstants.ScreenWidth = static_cast<uint32_t>(width);
        tileConstants.ScreenHeight = height;
        tileConstants.FrameIndex = _tileFrameIndex++;

        if (!CreateConstantsBuffer(_device, currentHeap, tileConstants, currentHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create tile constants buffer", _name);
            return false;
        }
    }
    else
    {
        InternalHudCopyParams constants {};
        constants.DiffThreshold = hudDetectionThreshold;

        if (!CreateConstantsBuffer(_device, currentHeap, constants, currentHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
            return false;
        }
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(useTileSkip ? _tilePipelineState : _pipelineState);

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

    const bool ok = DispatchKernel(computeCmdList, hudless, _srcBuffer, hudDetectionThreshold, _asyncWidth, _asyncHeight);

    if (ok)
    {
        ResourceBarrier(computeCmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        ResourceBarrier(computeCmdList, _srcBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
    }

    return ok;
}

void HudCopy_Dx12::AbortAsyncPrepare(ID3D12GraphicsCommandList* directCmdList)
{
    if (directCmdList == nullptr)
        return;

    if (_buffer != nullptr)
        ResourceBarrier(directCmdList, _buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    if (_srcBuffer != nullptr)
        ResourceBarrier(directCmdList, _srcBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
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

    if (!SetupRootSignature(InDevice, 2, 2, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, HudCopy_cso, sizeof(HudCopy_cso), shaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    // Compile coarse-tile early-out pipeline using runtime D3DCompile (non-fatal fallback to HudCopy_cso)
    if (auto tileBlob = CompileShader(kTileSkipHudCopyShader, "CSMain", "cs_5_0"))
    {
        if (CreateComputeShader(InDevice, _rootSignature, &_tilePipelineState, tileBlob, {}))
        {
            const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            const auto maskDesc = CD3DX12_RESOURCE_DESC::Buffer(kTileMaskElements * sizeof(uint32_t),
                                                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (SUCCEEDED(InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &maskDesc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                            IID_PPV_ARGS(&_tileMaskBuffer))) &&
                _tileMaskBuffer != nullptr)
            {
                _tileMaskBuffer->SetName(L"HudCopy_TileMask");
            }
            else
            {
                DeferredRelease::Release(_tilePipelineState);
                _tilePipelineState = nullptr;
            }
        }
        tileBlob->Release();
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
    DeferredRelease::Release(_tileMaskBuffer);
    DeferredRelease::Release(_tilePipelineState);
}
