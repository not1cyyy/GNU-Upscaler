#pragma once

#include "NL_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#define NL_NUM_OF_HEAPS 3

class NL_Dx12 : public Shader_Dx12, public NL_Common
{
  private:
    FrameDescriptorHeap _frameHeapsComposite[NL_NUM_OF_HEAPS];
    FrameDescriptorHeap _frameHeapsGi[NL_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource* _giBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _giBufferStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    ID3D12Resource* _ssrBuffers[2] = { nullptr, nullptr };
    D3D12_RESOURCE_STATES _ssrBufferStates[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
    uint32_t _giWidth = 0;
    uint32_t _giHeight = 0;
    int _giWriteIdx = 0;
    bool _resetGiHistory = true;

    ID3D12RootSignature* _rootSignatureGi = nullptr;
    ID3D12PipelineState* _pipelineStateGi = nullptr;

    bool CreateGiBuffers(ID3D12Device* InDevice, uint32_t width, uint32_t height);
    void ReleaseGiBuffers();

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);
    void RestoreBufferState(D3D12_RESOURCE_STATES InState) { _bufferState = InState; }

    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* InMotionVectors,
                  ID3D12Resource* InDepth, NeuralLookConstants InConstants, ID3D12Resource* OutResource,
                  ID3D12Resource* InGBufferNormals = nullptr, ID3D12Resource* InGBufferAlbedo = nullptr,
                  ID3D12Resource* InGBufferRoughness = nullptr);

    ID3D12Resource* Buffer() const { return _buffer; }
    bool CanRender() const { return _init && _buffer != nullptr; }

    NL_Dx12(std::string InName, ID3D12Device* InDevice);
    ~NL_Dx12();
};
