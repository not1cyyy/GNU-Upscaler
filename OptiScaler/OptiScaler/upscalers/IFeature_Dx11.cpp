#include <pch.h>
#include "IFeature_Dx11.h"
#include <State.h>
#include <misc/DeferredRelease.h>

using Microsoft::WRL::ComPtr;

IFeature_Dx11::ScopedComputeState::ScopedComputeState(ID3D11Device* device, ID3D11DeviceContext* context)
    : Context(context)
{
    if (Context == nullptr)
        return;

    UavCount = (device != nullptr && device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
                   ? D3D11_1_UAV_SLOT_COUNT
                   : D3D11_PS_CS_UAV_REGISTER_COUNT;

    Context->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, SRVs);
    Context->CSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, Samplers);
    Context->CSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, CBs);
    Context->CSGetUnorderedAccessViews(0, UavCount, UAVs);
    Context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, RTVs, &DSV);

    ID3D11RenderTargetView* nullRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    Context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nullRTVs, nullptr);
}

void IFeature_Dx11::ScopedComputeState::Restore()
{
    if (Restored)
        return;
    Restored = true;

    if (Context != nullptr)
    {
        Context->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, SRVs);
        Context->CSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, Samplers);
        Context->CSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, CBs);
        Context->CSSetUnorderedAccessViews(0, UavCount, UAVs, nullptr);
        Context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, RTVs, DSV);
    }

    for (auto*& srv : SRVs)
        SAFE_RELEASE(srv);
    for (auto*& sampler : Samplers)
        SAFE_RELEASE(sampler);
    for (auto*& cb : CBs)
        SAFE_RELEASE(cb);
    for (auto*& uav : UAVs)
        SAFE_RELEASE(uav);
    for (auto*& rtv : RTVs)
        SAFE_RELEASE(rtv);
    SAFE_RELEASE(DSV);
}

IFeature_Dx11::ScopedComputeState::~ScopedComputeState() { Restore(); }

bool IFeature_Dx11::Init(ID3D11Device* InDevice, ID3D11DeviceContext* InContext, NVSDK_NGX_Parameter* InParameters)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("ID3D11Device is null!");
        return false;
    }

    if (InContext == nullptr)
    {
        LOG_ERROR("ID3D11DeviceContext is null!");
        return false;
    }

    Device = InDevice;
    DeviceContext = InContext;

    auto result = InitInternal(InContext, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx11>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx11>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx11>("RCAS", InDevice);
        // DLSS/DLSSD take the bias mask as is
        if (GetUpscalerType() != Upscaler::DLSS && GetUpscalerType() != Upscaler::DLSSD)
            Bias = std::make_unique<Bias_Dx11>("Bias", InDevice);
        Magnifier = std::make_unique<Magnifier_Dx11>("Magnifier", InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx11>(InDevice);
    }

    return result;
}

bool IFeature_Dx11::Evaluate(ID3D11DeviceContext* InDeviceContext, NVSDK_NGX_Parameter* InParameters)
{
    // Frees deferred objects even when presents aren't hooked
    DeferredRelease::Collect();

    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

    auto result = true;
    ScopedComputeState computeStateGuard(Device, InDeviceContext);

    if (Config::Instance()->OverrideSharpness.value_or_default())
        _sharpness = Config::Instance()->Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(InParameters);

    if (_sharpness > 1.0f)
        _sharpness = 1.0f;

    // Those upcalers don't have their own sharpness so always need to use RCAS when sharpness is set
    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

    // Need RCAS for MAS
    if (!useRcas && (Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
                     Config::Instance()->MotionSharpness.value_or_default() > 0.0f))
    {
        useRcas = true;
    }

    if (!RCAS->IsInit())
        useRcas = false;

    bool useOutputScaling =
        Config::Instance()->OutputScalingEnabled.value_or_default() && (LowResMV() || RenderWidth() == DisplayWidth());

    if (!OutputScaler->IsInit())
        useOutputScaling = false;

    ID3D11Resource* paramOutput = nullptr;
    ID3D11Resource* paramMotion = nullptr;
    ID3D11Resource* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    // Order is important as that's the order of shader dispatch
    struct PostPass
    {
        PostPassStage stage;
        ID3D11Resource* inputBuffer = nullptr;
        ID3D11Resource* outputBuffer = nullptr;
    };

    std::array<PostPass, 3> pipeline {};
    size_t passCount = 0;

    if (useOutputScaling)
        pipeline[passCount++].stage = PostPassStage::OutputScaling;

    _actualSharpness.reset();
    if (useRcas)
        pipeline[passCount++].stage = PostPassStage::Rcas;

    if (Magnifier->ShouldRun())
        pipeline[passCount++].stage = PostPassStage::Magnifier;

    // RCAS setup and DLSS zero the game's sharpness, restored after EvaluateInternal
    float originalSharpness = 0.0f;
    bool hasSharpness =
        InParameters->Get(NVSDK_NGX_Parameter_Sharpness, &originalSharpness) == NVSDK_NGX_Result_Success;

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    ID3D11Resource* currentTarget = paramOutput;
    for (size_t i = passCount; i-- > 0;)
    {
        auto& pass = pipeline[i];
        ID3D11Resource* requiredInput = nullptr;

        switch (pass.stage)
        {
        case PostPassStage::OutputScaling:
            if (OutputScaler->CreateBufferResource(Device, currentTarget, TargetWidth(), TargetHeight()))
                requiredInput = OutputScaler->Buffer();
            break;

        case PostPassStage::Rcas:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput && RCAS->CreateBufferResource(Device, currentTarget) && RCAS->CanRender())
            {
                // Disable any built-in sharpness shaders
                InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                _actualSharpness = _sharpness;
                _sharpness = 0.0f;

                requiredInput = RCAS->Buffer();
            }
            break;

        case PostPassStage::Magnifier:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput && Magnifier->CreateBufferResource(Device, currentTarget) &&
                Magnifier->CanRender())
                requiredInput = Magnifier->Buffer();
            break;
        }

        if (requiredInput)
        {
            pass.outputBuffer = currentTarget;
            pass.inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Upscaler will write to the first active shader, or just output
    InParameters->Set(NVSDK_NGX_Parameter_Output, currentTarget);

    UpscalerTime->Start(InDeviceContext);

    auto evalResult = EvaluateInternal(InDeviceContext, InParameters);

    UpscalerTime->End(InDeviceContext);

    InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

    if (hasSharpness)
        InParameters->Set(NVSDK_NGX_Parameter_Sharpness, originalSharpness);

    if (!evalResult)
        result = false;

    bool pipelineFailed = false;

    // Iterate FORWARDS to execute the shaders in the defined order
    for (size_t i = 0; i < passCount; ++i)
    {
        const auto& pass = pipeline[i];
        if (!pass.inputBuffer || !pass.outputBuffer)
            continue;

        bool ok = true;
        switch (pass.stage)
        {
        case PostPassStage::OutputScaling:
            LOG_DEBUG("Scaling output...");

            if (!OutputScaler->Dispatch(Device, InDeviceContext, (ID3D11Texture2D*) pass.inputBuffer,
                                        (ID3D11Texture2D*) pass.outputBuffer))
            {
                Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                State::Instance().changeBackend[Handle()->Id] = true;
                ok = false;
            }
            break;

        case PostPassStage::Rcas:
            if (RCAS->CanRender() && paramMotion && paramOutput)
            {
                RcasConstants rcasConstants {};

                rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                rcasConstants.DepthIsLinear = DepthLinear();
                rcasConstants.DepthIsReversed = DepthInverted();
                rcasConstants.IsHdr = IsHdr() || State::Instance().hdrOutputActive ||
                                      State::Instance().outputColorSpace.transfer == ColorTransfer::PQ;

                // Restore value
                _sharpness = _actualSharpness.value_or(_sharpness);
                _actualSharpness.reset();

                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                float nearPlane = 0.0f;
                float farPlane = 0.0f;

                // We need camera near and far for DLSSD
                // We passthrough those values from the DLSSG params onto the upscaler's params
                if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                    InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                {
                    rcasConstants.CameraNear = nearPlane;
                    rcasConstants.CameraFar = farPlane;
                }
                else
                {
                    rcasConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                    rcasConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                }

                if (!RCAS->Dispatch(Device, InDeviceContext, (ID3D11Texture2D*) pass.inputBuffer,
                                    (ID3D11Texture2D*) paramMotion, rcasConstants, (ID3D11Texture2D*) pass.outputBuffer,
                                    (ID3D11Texture2D*) paramDepth))
                {
                    Config::Instance()->RcasEnabled.set_volatile_value(false);
                    ok = false;
                }
            }
            break;

        case PostPassStage::Magnifier:
            if (Magnifier->CanRender() && paramMotion && paramOutput)
            {
                ok = Magnifier->Dispatch(Device, InDeviceContext, (ID3D11Texture2D*) pass.inputBuffer,
                                         (ID3D11Texture2D*) pass.outputBuffer);
            }
            break;
        }

        if (!ok)
        {
            pipelineFailed = true;
            break;
        }
    }

    // imgui
    if (!pipelineFailed && !Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InDeviceContext, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx11>(GetForegroundWindow(), Device);
        }
    }

    computeStateGuard.Restore();

    _frameCount++;

    return result;
}

std::optional<double> IFeature_Dx11::ReadUpscalerTime(void* deviceContextVoid)
{
    if (!UpscalerTime || !RCAS || !OutputScaler)
        return std::nullopt;

    auto* deviceContext = (ID3D11DeviceContext*) deviceContextVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(deviceContext);
    lastRcasTime = RCAS->ReadGpuTime(deviceContext);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(deviceContext);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx11::ReadDetailedGpuTimes(void* deviceContextVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    auto* deviceContext = (ID3D11DeviceContext*) deviceContextVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(deviceContext);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx11::~IFeature_Dx11()
{
    if (State::Instance().isShuttingDown)
        return;

    Imgui.reset();
    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
}
