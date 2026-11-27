#include "pch.h"

#include "NL_Dx12.h"
#include "precompile/nl_composite_Shader.h"
#include "precompile/nl_denoise_Shader.h"
#include "precompile/nl_gi_temporal_Shader.h"
#include "precompile/nl_neural_prior_Shader.h"
#include "precompile/nl_prep_Shader.h"
#include "precompile/nl_pyramid_Shader.h"

#include <State.h>
#include <Util.h>
#include <misc/DeferredRelease.h>
#include <wrl/client.h>

namespace
{
inline bool IsValidSrvTex(ID3D12Resource* res)
{
    return res != nullptr && (res->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
}

inline void CreateBufferSRV(ID3D12Device* device, ID3D12Resource* buf, UINT numElements, UINT stride,
                            D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = numElements;
    srvDesc.Buffer.StructureByteStride = stride;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(buf, &srvDesc, dest);
}

inline void CreateBufferUAV(ID3D12Device* device, ID3D12Resource* buf, UINT numElements, UINT stride,
                            D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = numElements;
    uavDesc.Buffer.StructureByteStride = stride;
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(buf, nullptr, &uavDesc, dest);
}

inline void CreateTexture2DMipSRV(ID3D12Device* device, ID3D12Resource* tex, DXGI_FORMAT fmt, UINT mip,
                                  D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = fmt;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = mip;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex, &srvDesc, dest);
}

inline void CreateTexture3DUAV(ID3D12Device* device, ID3D12Resource* tex, DXGI_FORMAT fmt, UINT depth,
                               D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.Format = fmt;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.MipSlice = 0;
    uavDesc.Texture3D.FirstWSlice = 0;
    uavDesc.Texture3D.WSize = depth;
    device->CreateUnorderedAccessView(tex, nullptr, &uavDesc, dest);
}

struct BarrierBatch
{
    std::vector<D3D12_RESOURCE_BARRIER> list;

    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES& curState, D3D12_RESOURCE_STATES targetState)
    {
        if (res == nullptr || curState == targetState)
            return;

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = curState;
        b.Transition.StateAfter = targetState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list.push_back(b);
        curState = targetState;
    }

    void Flush(ID3D12GraphicsCommandList* cmdList)
    {
        if (!list.empty())
        {
            cmdList->ResourceBarrier(static_cast<UINT>(list.size()), list.data());
            list.clear();
        }
    }
};
} // namespace

bool NL_Dx12::CreateStaticBuffers(ID3D12Device* InDevice)
{
    if (_sceneLightBuffer != nullptr && _resolvedSceneLightBuffer != nullptr && _worldHashBuffer != nullptr)
        return true;

    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    if (_sceneLightBuffer == nullptr)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kSceneLightUints * sizeof(uint32_t),
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(InDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&_sceneLightBuffer))))
            return false;
        _sceneLightBuffer->SetName(L"NL_SceneLightBuf");
        _sceneLightState = D3D12_RESOURCE_STATE_COMMON;
    }

    if (_resolvedSceneLightBuffer == nullptr)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kResolvedSceneLightFloat4s * 16u,
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(InDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&_resolvedSceneLightBuffer))))
            return false;
        _resolvedSceneLightBuffer->SetName(L"NL_ResolvedSceneLightBuf");
        _resolvedSceneLightState = D3D12_RESOURCE_STATE_COMMON;
    }

    if (_worldHashBuffer == nullptr)
    {
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(kWorldHashCapacity * 2u * sizeof(uint32_t),
                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(InDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(&_worldHashBuffer))))
            return false;
        _worldHashBuffer->SetName(L"NL_WorldHashBuf");
        _worldHashState = D3D12_RESOURCE_STATE_COMMON;
    }

    return true;
}

void NL_Dx12::UpdateModelWeightsIfNeeded(ID3D12Device* InDevice)
{
    const std::string desired = Config::Instance()->NeuralLookModelPack.value_or_default();
    if (_modelWeightsBuffer != nullptr && _loadedModelPack == desired)
        return;

    if (_modelWeightsBuffer == nullptr)
    {
        const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(NLModel::kFloatCount * sizeof(float));
        if (FAILED(InDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&_modelWeightsBuffer))) ||
            _modelWeightsBuffer == nullptr)
        {
            LOG_ERROR("[{0}] Failed to create ModelWeightsBuf", _name);
            return;
        }
        _modelWeightsBuffer->SetName(L"NL_ModelWeightsBuf");
    }

    std::vector<float> weights;
    NLModel::ResolveModelWeights(desired, Util::DllPath().parent_path(), weights);

    void* mapped = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    if (SUCCEEDED(_modelWeightsBuffer->Map(0, &readRange, &mapped)) && mapped != nullptr)
    {
        std::memcpy(mapped, weights.data(), NLModel::kFloatCount * sizeof(float));
        _modelWeightsBuffer->Unmap(0, nullptr);
        _loadedModelPack = desired;
        LOG_INFO("[{0}] Loaded Neural Look model pack '{1}'", _name, desired);
    }
}

bool NL_Dx12::CreateGiBuffers(ID3D12Device* InDevice, uint32_t width, uint32_t height)
{
    const uint32_t targetW = std::max(1u, width / 2u);
    const uint32_t targetH = std::max(1u, height / 2u);

    if (_halfColorDepthBuffer != nullptr && _colorPyramidBuffer != nullptr && _bilateralGridBuffers[0] != nullptr &&
        _denoisedGiBuffer != nullptr && _giWidth == targetW && _giHeight == targetH)
    {
        return CreateStaticBuffers(InDevice);
    }

    ReleaseGiBuffers();
    if (!CreateStaticBuffers(InDevice))
        return false;

    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    auto createTex2D = [&](uint32_t w, uint32_t h, uint16_t mips, ID3D12Resource*& outRes,
                           D3D12_RESOURCE_STATES& outState, const wchar_t* dbgName) -> bool
    {
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = mips;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&outRes))) ||
            outRes == nullptr)
        {
            return false;
        }
        outRes->SetName(dbgName);
        outState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        return true;
    };

    if (!createTex2D(targetW, targetH, 1, _halfColorDepthBuffer, _halfColorDepthState, L"NL_HalfColorDepth") ||
        !createTex2D(targetW, targetH, 1, _denoisedGiBuffer, _denoisedGiState, L"NL_DenoisedGI") ||
        !createTex2D(targetW, targetH, 1, _denoisedSsrBuffer, _denoisedSsrState, L"NL_DenoisedSSR") ||
        !createTex2D(targetW, targetH, 1, _denoisedMetaBuffer, _denoisedMetaState, L"NL_DenoisedMeta"))
    {
        ReleaseGiBuffers();
        return false;
    }

    for (int i = 0; i < 2; ++i)
    {
        if (!createTex2D(targetW, targetH, 1, _halfNormalBuffers[i], _halfNormalStates[i],
                         i == 0 ? L"NL_HalfNormal_0" : L"NL_HalfNormal_1") ||
            !createTex2D(targetW, targetH, 1, _giBuffers[i], _giBufferStates[i],
                         i == 0 ? L"NL_GI_0" : L"NL_GI_1") ||
            !createTex2D(targetW, targetH, 1, _ssrBuffers[i], _ssrBufferStates[i],
                         i == 0 ? L"NL_SSR_0" : L"NL_SSR_1") ||
            !createTex2D(targetW, targetH, 1, _giMetaBuffers[i], _giMetaStates[i],
                         i == 0 ? L"NL_GIMeta_0" : L"NL_GIMeta_1"))
        {
            ReleaseGiBuffers();
            return false;
        }
    }

    // 5-mip Color + Hi-Z MinDepth pyramid (mip 0 = 1/4 res)
    uint32_t mw = std::max(1u, (targetW + 1u) / 2u);
    uint32_t mh = std::max(1u, (targetH + 1u) / 2u);
    for (int m = 0; m < 5; ++m)
    {
        _mipDims[m][0] = std::max(1u, mw >> m);
        _mipDims[m][1] = std::max(1u, mh >> m);
    }

    if (!createTex2D(_mipDims[0][0], _mipDims[0][1], 5, _colorPyramidBuffer, _colorPyramidState, L"NL_ColorPyramid"))
    {
        ReleaseGiBuffers();
        return false;
    }

    _eighthWidth = _mipDims[1][0];
    _eighthHeight = _mipDims[1][1];
    _gridWidth = std::max(1u, (_eighthWidth + 7u) / 8u);
    _gridHeight = std::max(1u, (_eighthHeight + 7u) / 8u);

    if (!createTex2D(_eighthWidth, _eighthHeight, 1, _eighthNormalBuffer, _eighthNormalState, L"NL_EighthNormal") ||
        !createTex2D(_eighthWidth, _eighthHeight, 1, _material0Buffers[0], _material0States[0], L"NL_Mat0_0") ||
        !createTex2D(_eighthWidth, _eighthHeight, 1, _material0Buffers[1], _material0States[1], L"NL_Mat0_1") ||
        !createTex2D(_eighthWidth, _eighthHeight, 1, _material1Buffers[0], _material1States[0], L"NL_Mat1_0") ||
        !createTex2D(_eighthWidth, _eighthHeight, 1, _material1Buffers[1], _material1States[1], L"NL_Mat1_1"))
    {
        ReleaseGiBuffers();
        return false;
    }

    // 3 x 3D Bilateral Grid textures (GridW x GridH x 8)
    for (int g = 0; g < 3; ++g)
    {
        D3D12_RESOURCE_DESC d3 {};
        d3.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        d3.Width = _gridWidth;
        d3.Height = _gridHeight;
        d3.DepthOrArraySize = static_cast<UINT16>(kBilateralLumaBins);
        d3.MipLevels = 1;
        d3.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d3.SampleDesc.Count = 1;
        d3.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (FAILED(InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &d3,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&_bilateralGridBuffers[g]))) ||
            _bilateralGridBuffers[g] == nullptr)
        {
            ReleaseGiBuffers();
            return false;
        }
        _bilateralGridBuffers[g]->SetName(g == 0 ? L"NL_BilateralGrid0"
                                                 : (g == 1 ? L"NL_BilateralGrid1" : L"NL_BilateralGrid2"));
        _bilateralGridStates[g] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    _giWidth = targetW;
    _giHeight = targetH;
    _resetGiHistory = true;
    return true;
}

void NL_Dx12::ReleaseGiBuffers()
{
    auto rel = [](ID3D12Resource*& r, D3D12_RESOURCE_STATES& s)
    {
        if (r != nullptr)
        {
            DeferredRelease::Release(r);
            r = nullptr;
            s = D3D12_RESOURCE_STATE_COMMON;
        }
    };

    rel(_halfColorDepthBuffer, _halfColorDepthState);
    rel(_denoisedGiBuffer, _denoisedGiState);
    rel(_denoisedSsrBuffer, _denoisedSsrState);
    rel(_denoisedMetaBuffer, _denoisedMetaState);
    rel(_colorPyramidBuffer, _colorPyramidState);
    rel(_eighthNormalBuffer, _eighthNormalState);

    for (int i = 0; i < 2; ++i)
    {
        rel(_halfNormalBuffers[i], _halfNormalStates[i]);
        rel(_giBuffers[i], _giBufferStates[i]);
        rel(_ssrBuffers[i], _ssrBufferStates[i]);
        rel(_giMetaBuffers[i], _giMetaStates[i]);
        rel(_material0Buffers[i], _material0States[i]);
        rel(_material1Buffers[i], _material1States[i]);
    }

    for (int g = 0; g < 3; ++g)
        rel(_bilateralGridBuffers[g], _bilateralGridStates[g]);

    _giWidth = 0;
    _giHeight = 0;
    _eighthWidth = 0;
    _eighthHeight = 0;
    _gridWidth = 0;
    _gridHeight = 0;
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

bool NL_Dx12::SetupPassRootSigAndPso(ID3D12Device* device, uint32_t numSrv, uint32_t numUav, uint32_t numCbv,
                                      uint32_t numStaticSamplers, const void* bytecode, size_t bytecodeSize,
                                      ID3D12RootSignature** outRootSig, ID3D12PipelineState** outPso,
                                      FrameDescriptorHeap* heaps)
{
    CD3DX12_DESCRIPTOR_RANGE1 ranges[3] {};
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numSrv, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, numUav, 0);
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numCbv, 0);

    CD3DX12_ROOT_PARAMETER1 rootParam {};
    rootParam.InitAsDescriptorTable(3, ranges);

    CD3DX12_STATIC_SAMPLER_DESC linearClamp(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc {};
    rootSigDesc.Init_1_1(1, &rootParam, numStaticSamplers, numStaticSamplers > 0 ? &linearClamp : nullptr,
                         D3D12_ROOT_SIGNATURE_FLAG_NONE);

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rootSigDesc, &sigBlob, &errBlob)) ||
        FAILED(device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                                             IID_PPV_ARGS(outRootSig))))
    {
        return false;
    }

    if (!CreateComputeShader(device, *outRootSig, outPso, nullptr, CD3DX12_SHADER_BYTECODE(bytecode, bytecodeSize)))
        return false;

    if (heaps != nullptr)
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        for (int i = 0; i < NL_NUM_OF_HEAPS; ++i)
        {
            if (!heaps[i].Initialize(device, numSrv, numUav, numCbv, 0))
                return false;
        }
    }

    return true;
}

bool NL_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource,
                       ID3D12Resource* InMotionVectors, ID3D12Resource* InDepth, NeuralLookConstants InConstants,
                       ID3D12Resource* OutResource, ID3D12Resource* InGBufferNormals,
                       ID3D12Resource* InGBufferAlbedo, ID3D12Resource* InGBufferRoughness,
                       ID3D12Resource* InGBufferMetallic, ID3D12Resource* InGBufferSubsurface,
                       ID3D12Resource* InReactiveMask, ID3D12Resource* InTransparencyMask,
                       ID3D12Resource* InExposure)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr ||
        InMotionVectors == nullptr || InDepth == nullptr)
    {
        return false;
    }

    const auto outDesc = OutResource->GetDesc();
    const auto mvsDesc = InMotionVectors->GetDesc();
    const auto depthDesc = InDepth->GetDesc();

    if (!CreateGiBuffers(_device, static_cast<uint32_t>(outDesc.Width), outDesc.Height))
        return false;

    UpdateModelWeightsIfNeeded(_device);
    if (_modelWeightsBuffer == nullptr)
        return false;

    ScopedGpuTime_Dx12 scopedGpuTime(GpuTime.get(), InCmdList);

    _counter = (_counter + 1) % NL_NUM_OF_HEAPS;
    _frameIndex++;

    const bool hdrPQ = Shader_Dx12::IsHdrPQActive();
    const auto srcSpace = Shader_Dx12::GetSrvColorSpace(InResource->GetDesc().Format, hdrPQ);
    const auto dstSpace = Shader_Dx12::GetTargetStorageColorSpace(outDesc.Format, hdrPQ);
    const int32_t inEncodeMode = static_cast<int32_t>(GetColorEncodeMode(srcSpace, ShaderColorSpace::Linear));
    const int32_t outEncodeMode = static_cast<int32_t>(GetColorEncodeMode(ShaderColorSpace::Linear, dstSpace));

    const bool useGBuffers = Config::Instance()->NeuralLookUseGBuffers.value_or_default();
    const bool hasNormals = useGBuffers && IsValidSrvTex(InGBufferNormals);
    const bool hasAlbedo = useGBuffers && IsValidSrvTex(InGBufferAlbedo);
    const bool hasRoughness = useGBuffers && IsValidSrvTex(InGBufferRoughness);
    const bool hasMetallic = useGBuffers && IsValidSrvTex(InGBufferMetallic);
    const bool hasSubsurface = useGBuffers && IsValidSrvTex(InGBufferSubsurface);
    const bool hasReactive = IsValidSrvTex(InReactiveMask);
    const bool hasTransparency = IsValidSrvTex(InTransparencyMask);
    const bool hasExposure = IsValidSrvTex(InExposure);

    const auto* cfg = Config::Instance();
    const float giIntensity = cfg->NeuralLookGiIntensity.value_or_default();
    const float aoIntensity = cfg->NeuralLookAoIntensity.value_or_default();
    const float ssrIntensity = cfg->NeuralLookSsrIntensity.value_or_default();
    const float contactIntensity = cfg->NeuralLookContactShadowIntensity.value_or_default();
    const bool worldHashEnabled = cfg->NeuralLookWorldHashEnabled.value_or_default();
    const int debugView = cfg->NeuralLookDebugView.value_or_default();

    const bool ssrEnabled = (_activeTier > 0) && (ssrIntensity > 1e-4f || debugView == 5);
    const bool runGiPass = (giIntensity > 1e-4f || aoIntensity > 1e-4f || contactIntensity > 1e-4f || ssrEnabled ||
                            debugView == 1 || debugView == 2 || debugView == 7);

    _giWriteIdx = (_giWriteIdx + 1) & 1;
    const int prevIdx = _giWriteIdx ^ 1;

    NeuralLookConstants temporalConstIn = InConstants;
    if (_resetGiHistory)
    {
        temporalConstIn.ResetHistory = true;
        _resetGiHistory = false;
    }

    BarrierBatch barriers;

    // --- Pass 0: Prep (Temporal G-Proxy + World Hash Radiance Injection + SceneLightBuf Clear) ---
    {
        barriers.Transition(_halfColorDepthBuffer, _halfColorDepthState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_halfNormalBuffers[_giWriteIdx], _halfNormalStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_halfNormalBuffers[prevIdx], _halfNormalStates[prevIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_giBuffers[prevIdx], _giBufferStates[prevIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_sceneLightBuffer, _sceneLightState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_worldHashBuffer, _worldHashState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& prepHeap = _frameHeapsPrep[_counter];
        CreateShaderResourceView(_device, InResource, prepHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, InDepth, prepHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, hasNormals ? InGBufferNormals : InResource, prepHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, InMotionVectors, prepHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _halfNormalBuffers[prevIdx], prepHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, hasReactive ? InReactiveMask : InResource, prepHeap.GetSrvCPU(5));
        CreateShaderResourceView(_device, hasTransparency ? InTransparencyMask : InResource, prepHeap.GetSrvCPU(6));
        CreateShaderResourceView(_device, hasExposure ? InExposure : InResource, prepHeap.GetSrvCPU(7));
        CreateShaderResourceView(_device, _giBuffers[prevIdx], prepHeap.GetSrvCPU(8));
        CreateUnorderedAccessView(_device, _halfColorDepthBuffer, prepHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _halfNormalBuffers[_giWriteIdx], prepHeap.GetUavCPU(1), 0);
        CreateBufferUAV(_device, _sceneLightBuffer, kSceneLightUints, sizeof(uint32_t), prepHeap.GetUavCPU(2));
        CreateBufferUAV(_device, _worldHashBuffer, kWorldHashCapacity * 2u, sizeof(uint32_t), prepHeap.GetUavCPU(3));

        NLPrepInternalConstants prepConst {};
        prepConst.OutputWidth = static_cast<int32_t>(outDesc.Width);
        prepConst.OutputHeight = static_cast<int32_t>(outDesc.Height);
        prepConst.HalfWidth = static_cast<int32_t>(_giWidth);
        prepConst.HalfHeight = static_cast<int32_t>(_giHeight);
        prepConst.DepthWidth = static_cast<int32_t>(depthDesc.Width);
        prepConst.DepthHeight = static_cast<int32_t>(depthDesc.Height);
        prepConst.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        prepConst.MotionHeight = static_cast<int32_t>(mvsDesc.Height);
        FillPrepConstants(prepConst, temporalConstIn, hasNormals, hasReactive, hasTransparency, hasExposure,
                          worldHashEnabled, inEncodeMode);

        if (!CreateConstantsBuffer(_device, prepHeap, prepConst, prepHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* heaps[] = { prepHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, heaps);
        InCmdList->SetComputeRootSignature(_rootSignaturePrep);
        InCmdList->SetPipelineState(_pipelineStatePrep);
        InCmdList->SetComputeRootDescriptorTable(0, prepHeap.GetTableGPUStart());
        InCmdList->Dispatch((_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);
    }

    // --- Pass 1: SPD 5-Mip Hi-Z Depth + Color Pyramid + Scene L1 SH Accumulation ---
    {
        barriers.Transition(_halfColorDepthBuffer, _halfColorDepthState,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_halfNormalBuffers[_giWriteIdx], _halfNormalStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_colorPyramidBuffer, _colorPyramidState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_eighthNormalBuffer, _eighthNormalState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& pyrHeap = _frameHeapsPyramid[_counter];
        CreateShaderResourceView(_device, _halfColorDepthBuffer, pyrHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _halfNormalBuffers[_giWriteIdx], pyrHeap.GetSrvCPU(1));
        for (uint32_t m = 0; m < 5; ++m)
            CreateUnorderedAccessView(_device, _colorPyramidBuffer, pyrHeap.GetUavCPU(m), m);
        CreateUnorderedAccessView(_device, _eighthNormalBuffer, pyrHeap.GetUavCPU(5), 0);
        CreateBufferUAV(_device, _sceneLightBuffer, kSceneLightUints, sizeof(uint32_t), pyrHeap.GetUavCPU(6));

        NLPyramidInternalConstants pyrConst {};
        pyrConst.HalfWidth = static_cast<int32_t>(_giWidth);
        pyrConst.HalfHeight = static_cast<int32_t>(_giHeight);
        pyrConst.Mip0Width = static_cast<int32_t>(_mipDims[0][0]);
        pyrConst.Mip0Height = static_cast<int32_t>(_mipDims[0][1]);
        pyrConst.Mip1Width = static_cast<int32_t>(_mipDims[1][0]);
        pyrConst.Mip1Height = static_cast<int32_t>(_mipDims[1][1]);
        pyrConst.Mip2Width = static_cast<int32_t>(_mipDims[2][0]);
        pyrConst.Mip2Height = static_cast<int32_t>(_mipDims[2][1]);
        pyrConst.Mip3Width = static_cast<int32_t>(_mipDims[3][0]);
        pyrConst.Mip3Height = static_cast<int32_t>(_mipDims[3][1]);
        pyrConst.Mip4Width = static_cast<int32_t>(_mipDims[4][0]);
        pyrConst.Mip4Height = static_cast<int32_t>(_mipDims[4][1]);
        FillPyramidConstants(pyrConst, temporalConstIn, static_cast<int32_t>(outDesc.Width),
                             static_cast<int32_t>(outDesc.Height));

        if (!CreateConstantsBuffer(_device, pyrHeap, pyrConst, pyrHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* heaps[] = { pyrHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, heaps);
        InCmdList->SetComputeRootSignature(_rootSignaturePyramid);
        InCmdList->SetPipelineState(_pipelineStatePyramid);
        InCmdList->SetComputeRootDescriptorTable(0, pyrHeap.GetTableGPUStart());
        InCmdList->Dispatch((_mipDims[0][0] + 15u) / 16u, (_mipDims[0][1] + 15u) / 16u, 1);
    }

    // --- Pass 2: Real Trained Neural Material/Intrinsic Prior + 3D Bilateral Grid ---
    {
        barriers.Transition(_colorPyramidBuffer, _colorPyramidState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_eighthNormalBuffer, _eighthNormalState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_sceneLightBuffer, _sceneLightState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_material0Buffers[prevIdx], _material0States[prevIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_material1Buffers[prevIdx], _material1States[prevIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_material0Buffers[_giWriteIdx], _material0States[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_material1Buffers[_giWriteIdx], _material1States[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (int g = 0; g < 3; ++g)
            barriers.Transition(_bilateralGridBuffers[g], _bilateralGridStates[g], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_resolvedSceneLightBuffer, _resolvedSceneLightState,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& priorHeap = _frameHeapsPrior[_counter];
        CreateTexture2DMipSRV(_device, _colorPyramidBuffer, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, priorHeap.GetSrvCPU(0));
        CreateTexture2DMipSRV(_device, _colorPyramidBuffer, DXGI_FORMAT_R16G16B16A16_FLOAT, 3, priorHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, _eighthNormalBuffer, priorHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, InMotionVectors, priorHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _material0Buffers[prevIdx], priorHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _material1Buffers[prevIdx], priorHeap.GetSrvCPU(5));
        CreateBufferSRV(_device, _sceneLightBuffer, kSceneLightUints, sizeof(uint32_t), priorHeap.GetSrvCPU(6));
        CreateBufferSRV(_device, _modelWeightsBuffer, NLModel::kFloat4Count, 16u, priorHeap.GetSrvCPU(7));

        CreateUnorderedAccessView(_device, _material0Buffers[_giWriteIdx], priorHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _material1Buffers[_giWriteIdx], priorHeap.GetUavCPU(1), 0);
        for (int g = 0; g < 3; ++g)
            CreateTexture3DUAV(_device, _bilateralGridBuffers[g], DXGI_FORMAT_R16G16B16A16_FLOAT, kBilateralLumaBins,
                               priorHeap.GetUavCPU(2 + g));
        CreateBufferUAV(_device, _resolvedSceneLightBuffer, kResolvedSceneLightFloat4s, 16u, priorHeap.GetUavCPU(5));

        NLNeuralPriorInternalConstants priorConst {};
        priorConst.EighthWidth = static_cast<int32_t>(_eighthWidth);
        priorConst.EighthHeight = static_cast<int32_t>(_eighthHeight);
        priorConst.Mip3Width = static_cast<int32_t>(_mipDims[3][0]);
        priorConst.Mip3Height = static_cast<int32_t>(_mipDims[3][1]);
        priorConst.GridWidth = static_cast<int32_t>(_gridWidth);
        priorConst.GridHeight = static_cast<int32_t>(_gridHeight);
        priorConst.OutputWidth = static_cast<int32_t>(outDesc.Width);
        priorConst.OutputHeight = static_cast<int32_t>(outDesc.Height);
        priorConst.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        priorConst.MotionHeight = static_cast<int32_t>(mvsDesc.Height);
        FillNeuralPriorConstants(priorConst, temporalConstIn);

        if (!CreateConstantsBuffer(_device, priorHeap, priorConst, priorHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* heaps[] = { priorHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, heaps);
        InCmdList->SetComputeRootSignature(_rootSignaturePrior);
        InCmdList->SetPipelineState(_pipelineStatePrior);
        InCmdList->SetComputeRootDescriptorTable(0, priorHeap.GetTableGPUStart());
        InCmdList->Dispatch(_gridWidth, _gridHeight, 1);
    }

    // --- Pass 3 & Pass 4: Visibility-Bitmask SSGI + Contact Shadows + Hi-Z SSR + À-Trous Denoise ---
    if (runGiPass)
    {
        barriers.Transition(_material1Buffers[_giWriteIdx], _material1States[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_resolvedSceneLightBuffer, _resolvedSceneLightState,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_worldHashBuffer, _worldHashState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_ssrBuffers[prevIdx], _ssrBufferStates[prevIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_giBuffers[_giWriteIdx], _giBufferStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_ssrBuffers[_giWriteIdx], _ssrBufferStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_giMetaBuffers[_giWriteIdx], _giMetaStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& giHeap = _frameHeapsGi[_counter];
        CreateShaderResourceView(_device, _colorPyramidBuffer, giHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _halfColorDepthBuffer, giHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, _halfNormalBuffers[_giWriteIdx], giHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _halfNormalBuffers[prevIdx], giHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, InMotionVectors, giHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _giBuffers[prevIdx], giHeap.GetSrvCPU(5));
        CreateShaderResourceView(_device, _ssrBuffers[prevIdx], giHeap.GetSrvCPU(6));
        CreateShaderResourceView(_device, _material1Buffers[_giWriteIdx], giHeap.GetSrvCPU(7));
        CreateBufferSRV(_device, _resolvedSceneLightBuffer, kResolvedSceneLightFloat4s, 16u, giHeap.GetSrvCPU(8));
        CreateBufferSRV(_device, _worldHashBuffer, kWorldHashCapacity * 2u, sizeof(uint32_t), giHeap.GetSrvCPU(9));

        CreateUnorderedAccessView(_device, _giBuffers[_giWriteIdx], giHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _ssrBuffers[_giWriteIdx], giHeap.GetUavCPU(1), 0);
        CreateUnorderedAccessView(_device, _giMetaBuffers[_giWriteIdx], giHeap.GetUavCPU(2), 0);

        NLGiInternalConstants giConst {};
        giConst.OutputWidth = static_cast<int32_t>(outDesc.Width);
        giConst.OutputHeight = static_cast<int32_t>(outDesc.Height);
        giConst.GiWidth = static_cast<int32_t>(_giWidth);
        giConst.GiHeight = static_cast<int32_t>(_giHeight);
        giConst.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        giConst.MotionHeight = static_cast<int32_t>(mvsDesc.Height);
        FillGiConstants(giConst, temporalConstIn, ssrEnabled, worldHashEnabled);

        if (!CreateConstantsBuffer(_device, giHeap, giConst, giHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* heaps[] = { giHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, heaps);
        InCmdList->SetComputeRootSignature(_rootSignatureGi);
        InCmdList->SetPipelineState(_pipelineStateGi);
        InCmdList->SetComputeRootDescriptorTable(0, giHeap.GetTableGPUStart());
        InCmdList->Dispatch((_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);

        // Pass 4: À-Trous Variance-Guided Denoiser
        barriers.Transition(_giBuffers[_giWriteIdx], _giBufferStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_ssrBuffers[_giWriteIdx], _ssrBufferStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_giMetaBuffers[_giWriteIdx], _giMetaStates[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_denoisedGiBuffer, _denoisedGiState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_denoisedSsrBuffer, _denoisedSsrState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Transition(_denoisedMetaBuffer, _denoisedMetaState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& dnHeap = _frameHeapsDenoise[_counter];
        CreateShaderResourceView(_device, _halfColorDepthBuffer, dnHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, _halfNormalBuffers[_giWriteIdx], dnHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, _giBuffers[_giWriteIdx], dnHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _ssrBuffers[_giWriteIdx], dnHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _giMetaBuffers[_giWriteIdx], dnHeap.GetSrvCPU(4));
        CreateUnorderedAccessView(_device, _denoisedGiBuffer, dnHeap.GetUavCPU(0), 0);
        CreateUnorderedAccessView(_device, _denoisedSsrBuffer, dnHeap.GetUavCPU(1), 0);
        CreateUnorderedAccessView(_device, _denoisedMetaBuffer, dnHeap.GetUavCPU(2), 0);

        NLDenoiseInternalConstants dnConst {};
        FillDenoiseConstants(dnConst, temporalConstIn, static_cast<int32_t>(_giWidth),
                             static_cast<int32_t>(_giHeight), ssrEnabled);
        if (!CreateConstantsBuffer(_device, dnHeap, dnConst, dnHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* dnHeaps[] = { dnHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, dnHeaps);
        InCmdList->SetComputeRootSignature(_rootSignatureDenoise);
        InCmdList->SetPipelineState(_pipelineStateDenoise);
        InCmdList->SetComputeRootDescriptorTable(0, dnHeap.GetTableGPUStart());
        InCmdList->Dispatch((_giWidth + 7u) / 8u, (_giHeight + 7u) / 8u, 1);
    }

    // --- Pass 5: Full-Res PBR Composite + Skin SSS + Bilateral Grid + AgX + Fused RCAS ---
    {
        barriers.Transition(_material0Buffers[_giWriteIdx], _material0States[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_material1Buffers[_giWriteIdx], _material1States[_giWriteIdx],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (int g = 0; g < 3; ++g)
            barriers.Transition(_bilateralGridBuffers[g], _bilateralGridStates[g],
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_resolvedSceneLightBuffer, _resolvedSceneLightState,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_worldHashBuffer, _worldHashState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_denoisedGiBuffer, _denoisedGiState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_denoisedSsrBuffer, _denoisedSsrState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Transition(_denoisedMetaBuffer, _denoisedMetaState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.Flush(InCmdList);

        FrameDescriptorHeap& compHeap = _frameHeapsComposite[_counter];
        CreateShaderResourceView(_device, InResource, compHeap.GetSrvCPU(0));
        CreateShaderResourceView(_device, InDepth, compHeap.GetSrvCPU(1));
        CreateShaderResourceView(_device, InMotionVectors, compHeap.GetSrvCPU(2));
        CreateShaderResourceView(_device, _denoisedGiBuffer, compHeap.GetSrvCPU(3));
        CreateShaderResourceView(_device, _halfNormalBuffers[_giWriteIdx], compHeap.GetSrvCPU(4));
        CreateShaderResourceView(_device, _denoisedSsrBuffer, compHeap.GetSrvCPU(5));
        CreateShaderResourceView(_device, _denoisedMetaBuffer, compHeap.GetSrvCPU(6));
        CreateShaderResourceView(_device, _colorPyramidBuffer, compHeap.GetSrvCPU(7));
        CreateShaderResourceView(_device, _material0Buffers[_giWriteIdx], compHeap.GetSrvCPU(8));
        CreateShaderResourceView(_device, _material1Buffers[_giWriteIdx], compHeap.GetSrvCPU(9));
        for (int g = 0; g < 3; ++g)
            CreateShaderResourceView(_device, _bilateralGridBuffers[g], compHeap.GetSrvCPU(10 + g));
        CreateShaderResourceView(_device, hasAlbedo ? InGBufferAlbedo : InResource, compHeap.GetSrvCPU(13));
        CreateShaderResourceView(_device, hasRoughness ? InGBufferRoughness : InResource, compHeap.GetSrvCPU(14));
        CreateShaderResourceView(_device, hasMetallic ? InGBufferMetallic : InResource, compHeap.GetSrvCPU(15));
        CreateShaderResourceView(_device, hasSubsurface ? InGBufferSubsurface : InResource, compHeap.GetSrvCPU(16));
        CreateBufferSRV(_device, _resolvedSceneLightBuffer, kResolvedSceneLightFloat4s, 16u, compHeap.GetSrvCPU(17));
        CreateBufferSRV(_device, _worldHashBuffer, kWorldHashCapacity * 2u, sizeof(uint32_t), compHeap.GetSrvCPU(18));
        CreateUnorderedAccessView(_device, OutResource, compHeap.GetUavCPU(0), 0);

        NLCompositeInternalConstants compConst {};
        compConst.OutputWidth = static_cast<int32_t>(outDesc.Width);
        compConst.OutputHeight = static_cast<int32_t>(outDesc.Height);
        compConst.GiWidth = static_cast<int32_t>(_giWidth);
        compConst.GiHeight = static_cast<int32_t>(_giHeight);
        compConst.DepthWidth = static_cast<int32_t>(depthDesc.Width);
        compConst.DepthHeight = static_cast<int32_t>(depthDesc.Height);
        compConst.MotionWidth = static_cast<int32_t>(mvsDesc.Width);
        compConst.MotionHeight = static_cast<int32_t>(mvsDesc.Height);

        FillCompositeConstants(compConst, temporalConstIn, runGiPass, ssrEnabled, hasNormals, hasAlbedo, hasRoughness,
                               hasMetallic, hasSubsurface, worldHashEnabled, inEncodeMode, outEncodeMode);

        if (!CreateConstantsBuffer(_device, compHeap, compConst, compHeap.GetCbvCPU(0)))
            return false;

        ID3D12DescriptorHeap* heaps[] = { compHeap.GetHeapCSU() };
        InCmdList->SetDescriptorHeaps(1, heaps);
        InCmdList->SetComputeRootSignature(_rootSignature);
        InCmdList->SetPipelineState(_pipelineState);
        InCmdList->SetComputeRootDescriptorTable(0, compHeap.GetTableGPUStart());
        InCmdList->Dispatch((static_cast<UINT>(outDesc.Width) + 15u) / 16u, (outDesc.Height + 15u) / 16u, 1);
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

    if (!SetupPassRootSigAndPso(InDevice, 9, 4, 1, 1, nl_prep_cso, sizeof(nl_prep_cso), &_rootSignaturePrep,
                                &_pipelineStatePrep, _frameHeapsPrep) ||
        !SetupPassRootSigAndPso(InDevice, 2, 7, 1, 0, nl_pyramid_cso, sizeof(nl_pyramid_cso), &_rootSignaturePyramid,
                                &_pipelineStatePyramid, _frameHeapsPyramid) ||
        !SetupPassRootSigAndPso(InDevice, 8, 6, 1, 1, nl_neural_prior_cso, sizeof(nl_neural_prior_cso),
                                &_rootSignaturePrior, &_pipelineStatePrior, _frameHeapsPrior) ||
        !SetupPassRootSigAndPso(InDevice, 10, 3, 1, 1, nl_gi_temporal_cso, sizeof(nl_gi_temporal_cso), &_rootSignatureGi,
                                &_pipelineStateGi, _frameHeapsGi) ||
        !SetupPassRootSigAndPso(InDevice, 5, 3, 1, 0, nl_denoise_cso, sizeof(nl_denoise_cso), &_rootSignatureDenoise,
                                &_pipelineStateDenoise, _frameHeapsDenoise) ||
        !SetupPassRootSigAndPso(InDevice, 19, 1, 1, 1, nl_composite_cso, sizeof(nl_composite_cso), &_rootSignature,
                                &_pipelineState, _frameHeapsComposite))
    {
        LOG_ERROR("[{0}] Failed to setup Neural Look DX12 pipelines", _name);
        return;
    }

    _init = true;
}

NL_Dx12::~NL_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    DeferredRelease::Release(_pipelineStatePrep);
    DeferredRelease::Release(_rootSignaturePrep);
    DeferredRelease::Release(_pipelineStatePyramid);
    DeferredRelease::Release(_rootSignaturePyramid);
    DeferredRelease::Release(_pipelineStatePrior);
    DeferredRelease::Release(_rootSignaturePrior);
    DeferredRelease::Release(_pipelineStateGi);
    DeferredRelease::Release(_rootSignatureGi);
    DeferredRelease::Release(_pipelineStateDenoise);
    DeferredRelease::Release(_rootSignatureDenoise);

    for (int i = 0; i < NL_NUM_OF_HEAPS; ++i)
    {
        _frameHeapsPrep[i].ReleaseHeaps();
        _frameHeapsPyramid[i].ReleaseHeaps();
        _frameHeapsPrior[i].ReleaseHeaps();
        _frameHeapsGi[i].ReleaseHeaps();
        _frameHeapsDenoise[i].ReleaseHeaps();
        _frameHeapsComposite[i].ReleaseHeaps();
    }

    ReleaseGiBuffers();
    DeferredRelease::Release(_sceneLightBuffer);
    DeferredRelease::Release(_resolvedSceneLightBuffer);
    DeferredRelease::Release(_worldHashBuffer);
    DeferredRelease::Release(_modelWeightsBuffer);
    DeferredRelease::Release(_buffer);
}
