#pragma once

#include "NL_Common.h"
#include "NL_Model.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <string>
#include <vector>

#define NL_NUM_OF_HEAPS 3

class NL_Dx12 : public Shader_Dx12, public NL_Common
{
  private:
    FrameDescriptorHeap _frameHeapsPrep[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsPyramid[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsPrior[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsGi[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsDenoise[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsComposite[NL_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    // Half-res Temporal G-Proxy & GI/SSR/Meta/Dir + 1-Pass Denoised targets
    ID3D12Resource* _halfColorDepthBuffer = nullptr;
    D3D12_RESOURCE_STATES _halfColorDepthState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _halfNormalBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _halfNormalStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

    ID3D12Resource* _giBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _giBufferStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _ssrBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _ssrBufferStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _giMetaBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _giMetaStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _giDirBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _giDirStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };

    ID3D12Resource* _denoisedGiBuffer = nullptr;
    D3D12_RESOURCE_STATES _denoisedGiState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _denoisedSsrBuffer = nullptr;
    D3D12_RESOURCE_STATES _denoisedSsrState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _denoisedMetaBuffer = nullptr;
    D3D12_RESOURCE_STATES _denoisedMetaState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _denoisedGiDirBuffer = nullptr;
    D3D12_RESOURCE_STATES _denoisedGiDirState = D3D12_RESOURCE_STATE_COMMON;

    // 5-mip Color + Hi-Z MinDepth pyramid (1/4 through 1/64 res) & 1/8-res Material/Features/Bilateral Grid
    ID3D12Resource* _colorPyramidBuffer = nullptr;
    D3D12_RESOURCE_STATES _colorPyramidState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _eighthNormalBuffer = nullptr;
    D3D12_RESOURCE_STATES _eighthNormalState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _eighthFeaturesBuffer = nullptr;
    D3D12_RESOURCE_STATES _eighthFeaturesState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _material0Buffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _material0States[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _material1Buffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _material1States[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _material2Buffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _material2States[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _bilateralGridBuffers[2][3] = {};
    D3D12_RESOURCE_STATES _bilateralGridStates[2][3] = {
        { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON },
        { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON }
    };

    // Scene light field, World-Space Spatial Hash Radiance Cache, and VRAM Neural Look model weights
    ID3D12Resource* _sceneLightBuffer = nullptr;
    D3D12_RESOURCE_STATES _sceneLightState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _resolvedSceneLightBuffer = nullptr;
    D3D12_RESOURCE_STATES _resolvedSceneLightState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _worldHashBuffer = nullptr;
    D3D12_RESOURCE_STATES _worldHashState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* _modelWeightsUploadBuffer = nullptr;
    ID3D12Resource* _modelWeightsBuffer = nullptr;
    D3D12_RESOURCE_STATES _modelWeightsState = D3D12_RESOURCE_STATE_COMMON;
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

    ID3D12RootSignature* _rootSignaturePrep = nullptr;
    ID3D12PipelineState* _pipelineStatePrep = nullptr;
    ID3D12RootSignature* _rootSignaturePyramid = nullptr;
    ID3D12PipelineState* _pipelineStatePyramid = nullptr;
    ID3D12RootSignature* _rootSignaturePrior = nullptr;
    ID3D12PipelineState* _pipelineStatePrior = nullptr;
    ID3D12RootSignature* _rootSignatureGi = nullptr;
    ID3D12PipelineState* _pipelineStateGi = nullptr;
    ID3D12RootSignature* _rootSignatureDenoise = nullptr;
    ID3D12PipelineState* _pipelineStateDenoise = nullptr;

    bool CreateGiBuffers(ID3D12Device* InDevice, uint32_t width, uint32_t height);
    bool CreateStaticBuffers(ID3D12Device* InDevice);
    void UpdateModelWeightsIfNeeded(ID3D12Device* InDevice);
    void ReleaseGiBuffers();

    bool SetupPassRootSigAndPso(ID3D12Device* device, uint32_t numSrv, uint32_t numUav, uint32_t numCbv,
                                uint32_t numStaticSamplers, const void* bytecode, size_t bytecodeSize,
                                ID3D12RootSignature** outRootSig, ID3D12PipelineState** outPso,
                                FrameDescriptorHeap* heaps, FrameDescriptorHeap* extraHeaps = nullptr);

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);
    void RestoreBufferState(D3D12_RESOURCE_STATES InState) { _bufferState = InState; }

    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* InMotionVectors,
                  ID3D12Resource* InDepth, NeuralLookConstants InConstants, ID3D12Resource* OutResource,
                  ID3D12Resource* InGBufferNormals = nullptr, ID3D12Resource* InGBufferAlbedo = nullptr,
                  ID3D12Resource* InGBufferRoughness = nullptr, ID3D12Resource* InGBufferMetallic = nullptr,
                  ID3D12Resource* InGBufferSubsurface = nullptr, ID3D12Resource* InReactiveMask = nullptr,
                  ID3D12Resource* InTransparencyMask = nullptr, ID3D12Resource* InExposure = nullptr);

    ID3D12Resource* Buffer() const { return _buffer; }
    bool CanRender() const { return _init && _buffer != nullptr; }

    NL_Dx12(std::string InName, ID3D12Device* InDevice);
    ~NL_Dx12();
};
