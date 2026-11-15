#pragma once
#include "HudCopy_Common.h"

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <dxgi1_6.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define HudCopy_NUM_OF_HEAPS 3

class HudCopy_Dx12 : public Shader_Dx12, public HudCopy_Common
{
  private:
    struct alignas(256) TileHudCopyParams
    {
        float DiffThreshold = 0.02f;
        uint32_t ScreenWidth = 0;
        uint32_t ScreenHeight = 0;
        uint32_t FrameIndex = 100;
    };

    FrameDescriptorHeap _frameHeaps[HudCopy_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    ID3D12Resource* _srcBuffer = nullptr;
    ID3D12Resource* _tileMaskBuffer = nullptr;
    ID3D12PipelineState* _tilePipelineState = nullptr;
    UINT64 _asyncWidth = 0;
    UINT _asyncHeight = 0;
    UINT64 _lastTileWidth = 0;
    UINT _lastTileHeight = 0;
    uint32_t _tileFrameIndex = 100;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

    static void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState);
    bool DispatchKernel(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless, ID3D12Resource* presentSrc,
                        float hudDetectionThreshold, UINT64 width, UINT height);

  public:
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* hudless, ID3D12Resource* present,
                  D3D12_RESOURCE_STATES hudlessState, D3D12_RESOURCE_STATES presentState, float hudDetectionThreshold);

    // Split async-compute path: Direct queue prepares copies/states -> Compute queue runs shader -> Direct queue copies back
    bool PrepareAsyncDispatch(ID3D12GraphicsCommandList* directCmdList, ID3D12Resource* hudless, ID3D12Resource* present,
                              D3D12_RESOURCE_STATES hudlessState, D3D12_RESOURCE_STATES presentState);
    bool DispatchAsyncCompute(ID3D12GraphicsCommandList* computeCmdList, ID3D12Resource* hudless,
                              float hudDetectionThreshold);
    void CompleteAsyncDispatch(ID3D12GraphicsCommandList* directCmdList, ID3D12Resource* hudless,
                               ID3D12Resource* present, D3D12_RESOURCE_STATES hudlessState,
                               D3D12_RESOURCE_STATES presentState);

    HudCopy_Dx12(std::string InName, ID3D12Device* InDevice);

    ~HudCopy_Dx12();
};
