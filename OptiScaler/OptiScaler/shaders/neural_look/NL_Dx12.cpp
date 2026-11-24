#include "pch.h"

#include "NL_Dx12.h"
#include "precompile/nl_composite_Shader.h"
#include "precompile/nl_gi_temporal_Shader.h"

#include <State.h>
#include <misc/DeferredRelease.h>
#include <wrl/client.h>

bool NL_Dx12::CreateGiBuffers(ID3D12Device* InDevice, uint32_t width, uint32_t height)
{
    const uint32_t targetW = std::max(1u, width / 2u);
    const uint32_t targetH = std::max(1u, height / 2u);

    if (_giBuffers[0] != nullptr && _giBuffers[1] != nullptr && _ssrBuffers[0] != nullptr &&
        _ssrBuffers[1] != nullptr && _giWidth == targetW && _giHeight == targetH)
    {
        return true;
    }

    ReleaseGiBuffers();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = targetW;
    desc.Height = targetH;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    for (int i = 0; i < 2; ++i)
    {
        auto hrGi = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                      IID_PPV_ARGS(&_giBuffers[i]));
        auto hrSsr = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                       IID_PPV_ARGS(&_ssrBuffers[i]));
        if (FAILED(hrGi) || _giBuffers[i] == nullptr || FAILED(hrSsr) || _ssrBuffers[i] == nullptr)
        {
            LOG_ERROR("[{0}] Failed to create GI/SSR ping-pong buffers {1}: {2:x} / {3:x}", _name, i, hrGi, hrSsr);
            ReleaseGiBuffers();
            return false;
        }

        _giBuffers[i]->SetName(i == 0 ? L"NL_GI_Buffer_0" : L"NL_GI_Buffer_1");
        _ssrBuffers[i]->SetName(i == 0 ? L"NL_SSR_Buffer_0" : L"NL_SSR_Buffer_1");
        _giBufferStates[i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        _ssrBufferStates[i] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    _giWidth = targetW;
    _giHeight = targetH;
    _resetGiHistory = true;
    return true;
}

void NL_Dx12::ReleaseGiBuffers()
{
    for (int i = 0; i < 2; ++i)
    {
        if (_giBuffers[i] != nullptr)
        {
            DeferredRelease::Release(_giBuffers[i]);
            _giBuffers[i] = nullptr;
            _giBufferStates[i] = D3D12_RESOURCE_STATE_COMMON;
        }

        if (_ssrBuffers[i] != nullptr)
        {
            DeferredRelease::Release(_ssrBuffers[i]);
            _ssrBuffers[i] = nullptr;
            _ssrBufferStates[i] = D3D12_RESOURCE_STATE_COMMON;
        }
    }

    _giWidth = 0;
    _giHeight = 0;
}

bool NL_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState)
{
    if (InDevice == nullptr || InSource == nullptr)
        return false;

    const auto inDesc = InSource->GetDesc();
    const auto packedFormat = Shader_Dx12::SelectPackedIntermediateFormat(InDevice, inDesc.Format);

    auto oldBuffer = _buffer;
    auto result = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 0, 0, packedFormat);

    if (result && _buffer != oldBuffer)
    {
        _buffer->SetName(L"NL_Input_Buffer");
        _bufferState = InState;
    }

    if (result)
        CreateGiBuffers(InDevice, static_cast<uint32_t>(inDesc.Width), inDesc.Height);

    return result;
}

void NL_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool NL_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource,
                       ID3D12Resource* InMotionVectors, ID3D12Resource* InDepth, NeuralLookConstants InConstants,
                       ID3D12Resource* OutResource, ID3D12Resource* InGBufferNormals,
                       ID3D12Resource* InGBufferAlbedo, ID3D12Resource* InGBufferRoughness)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr ||
        InMotionVectors == nullptr || InDepth == nullptr)
    {
        return false;
    }

    const auto outDesc = OutResource->GetDesc();
    const auto inDesc = InResource->GetDesc();
    const auto mvsDesc = InMotionVectors->GetDesc();
    const auto depthDesc = InDepth->GetDesc();

    if (!CreateGiBuffers(_device, static_cast<uint32_t>(outDesc.Width), outDesc.Height))
        return false;

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter = (_counter + 1) % NL_NUM_OF_HEAPS;
    _frameIndex++;

    const bool hdrPQ = Shader_Dx12::IsHdrPQActive();
    const auto srcSpace = Shader_Dx12::GetSrvColorSpace(inDesc.Format, hdrPQ);
    const auto dstSpace = Shader_Dx12::GetTargetStorageColorSpace(outDesc.Format, hdrPQ);
    const int32_t inEncodeMode = static_cast<int32_t>(GetColorEncodeMode(srcSpace, ShaderColorSpace::Linear));
    const int32_t outEncodeMode = static_cast<int32_t>(GetColorEncodeMode(ShaderColorSpace::Linear, dstSpace));

    const bool useGBuffers = Config::Instance()->NeuralLookUseGBuffers.value_or_default();
    const bool hasNormals = useGBuffers && InGBufferNormals != nullptr;
    const bool hasAlbedo = useGBuffers && InGBufferAlbedo != nullptr;
    const bool hasRoughness = useGBuffers && InGBufferRoughness != nullptr;

    const auto* cfg = Config::Instance();
    const float giIntensity = cfg->NeuralLookGiIntensity.value_or_default();
    const float aoIntensity = cfg->NeuralLookAoIntensity.value_or_default();
    const float ssrIntensity = cfg->NeuralLookSsrIntensity.value_or_default();
    const float volGlow = cfg->NeuralLookVolumetricGlow.value_or_default();
    const int debugView = cfg->NeuralLookDebugView.value_or_default();

    const bool ssrEnabled = (_activeTier > 0) && (ssrIntensity > 1e-4f || volGlow > 1e-4f || debugView == 5);
    const bool runGiPass = (_activeTier > 0) &&
                           (giIntensity > 1e-4f || aoIntensity > 1e-4f || ssrEnabled || debugView == 1 || debugView == 2);

    _giWriteIdx = (_giWriteIdx + 1) & 1;
    const int prevIdx = _giWriteIdx ^ 1;

    if (runGiPass)
    {
        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, _giBuffers[_giWriteIdx],
                                    &_giBufferStates[_giWriteIdx]);
        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, _giBuffers[prevIdx],
                                    &_giBufferStates[prevIdx]);
        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, _ssrBuffers[_giWriteIdx],
                                    &_ssrBufferStates[_giWriteIdx]);
        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, _ssrBuffers[prevIdx],
                                    &_ssrBufferStates[prevIdx]);

        FrameDescriptorHeap& giHeap = _frameHeapsGi[_counter];
        CreateShaderResourceView(_device, InResource, giHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, InDepth, giHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, InMotionVectors, giHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _giBuffers[prevIdx], giHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, hasNormals ? InGBufferNormals : InResource, giHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _ssrBuffers[prevIdx], giHeap.GetSrvCPU(5));
        CreateUnorderedAccessView(_device, _giBuffers[_giWriteIdx], giHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _ssrBuffers[_giWriteIdx], giHeap.GetUavCPU(1), 0);

        NLGiInternalConstants giConstants {};
        giConstants.OutputWidth = static_cast<int32_t>(outDesc.Width);
        giConstants.OutputHeight = static_cast<int32_t>(outDesc.Height);
        giConstants.GiWidth = static_cast<int32_t>(_giWidth);
        giConstants.GiHeight = static_cast<int32_t>(_giHeight);
        giConstants.DepthWidth = static_cast<int32_t>(depthDesc.Width);
        giConstants.DepthHeight = static_cast<int32_t>(depthDesc.Height);
        giConstants.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        giConstants.MotionHeight = static_cast<int32_t>(mvsDesc.Height);

        NeuralLookConstants giConstIn = InConstants;
        if (_resetGiHistory)
        {
            giConstIn.ResetHistory = true;
            _resetGiHistory = false;
        }
        FillGiConstants(giConstants, giConstIn, hasNormals, inEncodeMode, ssrEnabled);

        if (!CreateConstantsBuffer(_device, giHeap, giConstants, giHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create GI constants buffer", _name);
            return false;
        }

        ID3D12DescriptorHeap* heaps[] = { giHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        InCmdList->SetComputeRootSignature(_rootSignatureGi);
        InCmdList->SetPipelineState(_pipelineStateGi);
        InCmdList->SetComputeRootDescriptorTable(0, giHeap.GetTableGPUStart());

        const UINT gx = (_giWidth + 7u) / 8u;
        const UINT gy = (_giHeight + 7u) / 8u;
        InCmdList->Dispatch(gx, gy, 1);

        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, _giBuffers[_giWriteIdx],
                                    &_giBufferStates[_giWriteIdx]);
        Shader_Dx12::SetBufferState(InCmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, _ssrBuffers[_giWriteIdx],
                                    &_ssrBufferStates[_giWriteIdx]);
    }

    // Composite pass
    {
        FrameDescriptorHeap& compHeap = _frameHeapsComposite[_counter];
        CreateShaderResourceView(_device, InResource, compHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, InDepth, compHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, InMotionVectors, compHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _giBuffers[_giWriteIdx], compHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, hasNormals ? InGBufferNormals : InResource, compHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, hasAlbedo ? InGBufferAlbedo : InResource, compHeap.GetSrvCPU(5));
        CreateShaderResourceView(_device, hasRoughness ? InGBufferRoughness : InResource, compHeap.GetSrvCPU(6));
        CreateShaderResourceView(_device, _ssrBuffers[_giWriteIdx], compHeap.GetSrvCPU(7));
        CreateUnorderedAccessView(_device, OutResource, compHeap.GetUavCPU(0), 0);

        NLCompositeInternalConstants compConstants {};
        compConstants.OutputWidth = static_cast<int32_t>(outDesc.Width);
        compConstants.OutputHeight = static_cast<int32_t>(outDesc.Height);
        compConstants.GiWidth = static_cast<int32_t>(_giWidth);
        compConstants.GiHeight = static_cast<int32_t>(_giHeight);
        compConstants.DepthWidth = static_cast<int32_t>(depthDesc.Width);
        compConstants.DepthHeight = static_cast<int32_t>(depthDesc.Height);
        compConstants.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        compConstants.MotionHeight = static_cast<int32_t>(mvsDesc.Height);

        FillCompositeConstants(compConstants, InConstants, runGiPass, ssrEnabled, hasNormals, hasAlbedo, hasRoughness,
                               inEncodeMode, outEncodeMode);

        if (!CreateConstantsBuffer(_device, compHeap, compConstants, compHeap.GetCbvCPU(0)))
        {
            LOG_ERROR("[{0}] Failed to create Composite constants buffer", _name);
            return false;
        }

        ID3D12DescriptorHeap* heaps[] = { compHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
        InCmdList->SetComputeRootSignature(_rootSignature);
        InCmdList->SetPipelineState(_pipelineState);
        InCmdList->SetComputeRootDescriptorTable(0, compHeap.GetTableGPUStart());

        const UINT gx = (static_cast<UINT>(outDesc.Width) + 15u) / 16u;
        const UINT gy = (outDesc.Height + 15u) / 16u;
        InCmdList->Dispatch(gx, gy, 1);
    }

    return true;
}

NL_Dx12::NL_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    // 1. Setup Composite root signature (8 SRVs, 1 UAV, 1 CBV) and PSO
    if (!SetupRootSignature(InDevice, 8, 1, 1))
    {
        LOG_ERROR("[{0}] Failed to setup Composite root signature", _name);
        return;
    }

    if (!CreateComputeShader(InDevice, _rootSignature, &_pipelineState, nullptr,
                             CD3DX12_SHADER_BYTECODE(nl_composite_cso, sizeof(nl_composite_cso))))
    {
        LOG_ERROR("[{0}] Failed to create Composite compute PSO", _name);
        return;
    }

    if (!InitHeaps(InDevice, _frameHeapsComposite, NL_NUM_OF_HEAPS))
        return;

    // 2. Setup GI+SSR root signature (6 SRVs, 2 UAVs, 1 CBV) and PSO
    {
        CD3DX12_DESCRIPTOR_RANGE1 ranges[3] = {};
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);
        ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

        CD3DX12_ROOT_PARAMETER1 rootParam {};
        rootParam.InitAsDescriptorTable(3, ranges);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc {};
        rootSigDesc.Init_1_1(1, &rootParam, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
        if (FAILED(D3D12SerializeVersionedRootSignature(&rootSigDesc, &sigBlob, &errBlob)) ||
            FAILED(InDevice->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                                 IID_PPV_ARGS(&_rootSignatureGi))))
        {
            LOG_ERROR("[{0}] Failed to create GI root signature", _name);
            return;
        }

        if (!CreateComputeShader(InDevice, _rootSignatureGi, &_pipelineStateGi, nullptr,
                                 CD3DX12_SHADER_BYTECODE(nl_gi_temporal_cso, sizeof(nl_gi_temporal_cso))))
        {
            LOG_ERROR("[{0}] Failed to create GI compute PSO", _name);
            return;
        }

        ScopedSkipHeapCapture skipHeapCapture {};
        for (int i = 0; i < NL_NUM_OF_HEAPS; ++i)
        {
            if (!_frameHeapsGi[i].Initialize(InDevice, 6, 2, 1, 0))
            {
                LOG_ERROR("[{0}] Failed to init GI descriptor heap", _name);
                return;
            }
        }
    }

    _init = true;
}

NL_Dx12::~NL_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    DeferredRelease::Release(_pipelineStateGi);
    DeferredRelease::Release(_rootSignatureGi);

    for (int i = 0; i < NL_NUM_OF_HEAPS; ++i)
    {
        _frameHeapsComposite[i].ReleaseHeaps();
        _frameHeapsGi[i].ReleaseHeaps();
    }

    ReleaseGiBuffers();
    DeferredRelease::Release(_buffer);
}
