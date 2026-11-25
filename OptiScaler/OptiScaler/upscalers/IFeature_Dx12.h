#pragma once
#include <d3d12.h>
#include "IFeature.h"

#include "SysUtils.h"
#include <Util.h>
#include <menu/menu_dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include <shaders/neural_look/NL_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <shaders/bias/Bias_Dx12.h>
#include <shaders/magnifier/Magnifier_Dx12.h>
#include <shaders/AsyncCompute_Dx12.h>
#include <gpu_time/GpuTime_Dx12.h>

class IFeature_Dx12 : public virtual IFeature
{
  private:
    struct PendingAsyncPostPasses
    {
        bool active = false;
        ID3D12Resource* paramMotion = nullptr;
        ID3D12Resource* paramDepth = nullptr;
        bool runOutputScaler = false;
        ID3D12Resource* osInput = nullptr;
        ID3D12Resource* osOutput = nullptr;
        bool runNeuralLook = false;
        ID3D12Resource* nlInput = nullptr;
        ID3D12Resource* nlOutput = nullptr;
        ID3D12Resource* paramGBufferNormals = nullptr;
        ID3D12Resource* paramGBufferAlbedo = nullptr;
        ID3D12Resource* paramGBufferRoughness = nullptr;
        NeuralLookConstants nlConstants {};
        bool runRcas = false;
        ID3D12Resource* rcasInput = nullptr;
        ID3D12Resource* rcasOutput = nullptr;
        RcasConstants rcasConstants {};
        bool runMagnifier = false;
        ID3D12Resource* magInput = nullptr;
        ID3D12Resource* magOutput = nullptr;
    };

    std::vector<std::tuple<ID3D12Resource*, D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>> _barrierTransitions;
    bool _deferPostPasses = false;
    PendingAsyncPostPasses _pendingAsyncPasses {};
    UINT64 _pendingAsyncDoneFence = 0;

    void RestoreBarriers(ID3D12GraphicsCommandList* InCommandList);
    RcasConstants BuildRcasConstants(NVSDK_NGX_Parameter* InParameters);
    NeuralLookConstants BuildNeuralLookConstants(NVSDK_NGX_Parameter* InParameters, bool fuseRcas);
    bool ExecutePostPassSet(ID3D12GraphicsCommandList* cmdList, const PendingAsyncPostPasses& passes);

  protected:
    ID3D12Device* Device = nullptr;
    static inline std::unique_ptr<Menu_Dx12> Imgui = nullptr;
    std::unique_ptr<OS_Dx12> OutputScaler = nullptr;
    std::unique_ptr<NL_Dx12> NeuralLook = nullptr;
    std::unique_ptr<RCAS_Dx12> RCAS = nullptr;
    std::unique_ptr<Bias_Dx12> Bias = nullptr;
    std::unique_ptr<Magnifier_Dx12> Magnifier = nullptr;
    std::unique_ptr<AsyncCompute_Dx12> AsyncCompute = nullptr;

    std::unique_ptr<GpuTime_Dx12> UpscalerTime = nullptr;

    void ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                         D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const;

    // Recorded transitions are undone by Evaluate after the shader passes
    void ForwardBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                        D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState);

    virtual bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) = 0;

  public:
    bool Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    bool Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    void SetDeferPostPasses(bool defer) { _deferPostPasses = defer; }
    bool BeginPendingPostPassesAsync(ID3D12CommandQueue* directQueue);
    bool WaitPendingPostPassesAsync(ID3D12CommandQueue* directQueue);
    bool DispatchPendingPostPassesAsync(ID3D12CommandQueue* directQueue);

    API Api() const override { return API::DX12; }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override;
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

    IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~IFeature_Dx12();
};
