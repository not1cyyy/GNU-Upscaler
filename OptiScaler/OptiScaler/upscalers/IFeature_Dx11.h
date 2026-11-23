#pragma once
#include "IFeature.h"

#include <d3d11_1.h>
#include <menu/menu_dx11.h>
#include <shaders/rcas/RCAS_Dx11.h>
#include <shaders/output_scaling/OS_Dx11.h>
#include <shaders/bias/Bias_Dx11.h>
#include <shaders/magnifier/Magnifier_Dx11.h>
#include <gpu_time/GpuTime_Dx11.h>

class IFeature_Dx11 : public virtual IFeature
{
  protected:
    // Captures and restores D3D11 compute + OM state via bulk slot calls instead of per-slot COM loops.
    struct ScopedComputeState
    {
        ID3D11DeviceContext* Context = nullptr;
        UINT UavCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
        ID3D11ShaderResourceView* SRVs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
        ID3D11SamplerState* Samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        ID3D11Buffer* CBs[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
        ID3D11UnorderedAccessView* UAVs[D3D11_1_UAV_SLOT_COUNT] = {};
        ID3D11RenderTargetView* RTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* DSV = nullptr;
        bool Restored = false;

        ScopedComputeState(ID3D11Device* device, ID3D11DeviceContext* context);
        ~ScopedComputeState();

        ScopedComputeState(const ScopedComputeState&) = delete;
        ScopedComputeState& operator=(const ScopedComputeState&) = delete;

        void Restore();
    };

    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* DeviceContext = nullptr;
    inline static std::unique_ptr<Menu_Dx11> Imgui = nullptr;
    std::unique_ptr<OS_Dx11> OutputScaler = nullptr;
    std::unique_ptr<RCAS_Dx11> RCAS = nullptr;
    std::unique_ptr<Bias_Dx11> Bias = nullptr;
    std::unique_ptr<Magnifier_Dx11> Magnifier = nullptr;

    std::unique_ptr<GpuTime_Dx11> UpscalerTime = nullptr;

    virtual bool InitInternal(ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters) = 0;
    virtual bool EvaluateInternal(ID3D11DeviceContext* DeviceContext, NVSDK_NGX_Parameter* InParameters) = 0;

  public:
    // Only DX11 w/DX12 should be overriding those
    virtual bool Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters);
    virtual bool Evaluate(ID3D11DeviceContext* DeviceContext, NVSDK_NGX_Parameter* InParameters);

    API Api() const override { return API::DX11; }
    std::optional<double> ReadUpscalerTime(void* deviceContextVoid) override;
    void ReadDetailedGpuTimes(void* deviceContextVoid, std::vector<DetailedGpuTime>& detailedGpuTimes) override;

    IFeature_Dx11(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) : IFeature(InHandleId, InParameters) {}

    ~IFeature_Dx11();
};
