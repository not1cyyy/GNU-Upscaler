#include <pch.h>

#include <functional>
#include <vector>

#include "IFeature_Dx12.h"
#include "State.h"
#include <misc/DeferredRelease.h>

void IFeature_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                    D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState) const
{
    if (InBeforeState == InAfterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = InResource;
    barrier.Transition.StateBefore = InBeforeState;
    barrier.Transition.StateAfter = InAfterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);
}

void IFeature_Dx12::ForwardBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                                   D3D12_RESOURCE_STATES InBeforeState, D3D12_RESOURCE_STATES InAfterState)
{
    ResourceBarrier(InCommandList, InResource, InBeforeState, InAfterState);
    _barrierTransitions.emplace_back(InResource, InBeforeState, InAfterState);
}

void IFeature_Dx12::RestoreBarriers(ID3D12GraphicsCommandList* InCommandList)
{
    for (const auto& [resource, before, after] : _barrierTransitions | std::views::reverse)
        ResourceBarrier(InCommandList, resource, after, before);

    _barrierTransitions.clear();
}

bool IFeature_Dx12::Init(ID3D12Device* InDevice, ID3D12GraphicsCommandList* InCommandList,
                         NVSDK_NGX_Parameter* InParameters)
{
    Device = InDevice;

    auto result = InitInternal(InCommandList, InParameters);

    if (result)
    {
        if (!Config::Instance()->OverlayMenu.value_or_default() && (Imgui == nullptr || Imgui.get() == nullptr))
            Imgui = std::make_unique<Menu_Dx12>(Util::GetProcessWindow(), InDevice);

        OutputScaler = std::make_unique<OS_Dx12>("Output Scaling", InDevice, (TargetWidth() < DisplayWidth()));
        RCAS = std::make_unique<RCAS_Dx12>("RCAS", InDevice);
        // DLSS/DLSSD take the bias mask as is
        if (GetUpscalerType() != Upscaler::DLSS && GetUpscalerType() != Upscaler::DLSSD)
            Bias = std::make_unique<Bias_Dx12>("Bias", InDevice);
        Magnifier = std::make_unique<Magnifier_Dx12>("Magnifier", InDevice);
        AsyncCompute = std::make_unique<AsyncCompute_Dx12>(InDevice);

        UpscalerTime = std::make_unique<GpuTime_Dx12>(InDevice);
    }

    return result;
}

RcasConstants IFeature_Dx12::BuildRcasConstants(NVSDK_NGX_Parameter* InParameters)
{
    RcasConstants rcasConstants {};

    rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
    rcasConstants.DepthIsLinear = DepthLinear();
    rcasConstants.DepthIsReversed = DepthInverted();
    rcasConstants.IsHdr =
        IsHdr() || State::Instance().hdrOutputActive || State::Instance().outputColorSpace.transfer == ColorTransfer::PQ;

    _sharpness = _actualSharpness.value_or(_sharpness);
    _actualSharpness.reset();

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

    float nearPlane = 0.0f;
    float farPlane = 0.0f;

    // Passthrough camera near and far for DLSSD depth-adaptive sharpening
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

    return rcasConstants;
}

bool IFeature_Dx12::ExecutePostPassSet(ID3D12GraphicsCommandList* cmdList, const PendingAsyncPostPasses& passes)
{
    if (passes.runOutputScaler)
    {
        LOG_DEBUG("Scaling output...");
        OutputScaler->SetBufferState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!OutputScaler->Dispatch(cmdList, passes.osInput, passes.osOutput))
        {
            Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
            State::Instance().changeBackend[Handle()->Id] = true;
            return false;
        }
    }

    if (passes.runRcas)
    {
        RCAS->SetBufferState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!RCAS->Dispatch(cmdList, passes.rcasInput, passes.paramMotion, passes.rcasConstants, passes.rcasOutput,
                            passes.paramDepth))
        {
            Config::Instance()->RcasEnabled.set_volatile_value(false);
            return false;
        }
    }

    if (passes.runMagnifier)
    {
        Magnifier->SetBufferState(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (!Magnifier->Dispatch(cmdList, passes.magInput, passes.magOutput))
            return false;
    }

    return true;
}

bool IFeature_Dx12::DispatchPendingPostPassesAsync(ID3D12CommandQueue* directQueue)
{
    const PendingAsyncPostPasses passes = _pendingAsyncPasses;
    _pendingAsyncPasses = {};

    if (!passes.active)
        return true;

    if (AsyncCompute != nullptr && AsyncCompute->IsInit() && directQueue != nullptr &&
        Config::Instance()->Dx12AsyncCompute.value_or_default())
    {
        return AsyncCompute->Dispatch(directQueue,
                                      [&](ID3D12GraphicsCommandList* computeCmdList) -> bool
                                      { return ExecutePostPassSet(computeCmdList, passes); });
    }

    return false;
}

bool IFeature_Dx12::Evaluate(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    // Frees deferred objects even when presents aren't hooked
    DeferredRelease::Collect();

    if (!IsInited())
    {
        LOG_ERROR("Not inited!");
        return false;
    }

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

    ID3D12Resource* paramOutput = nullptr;
    ID3D12Resource* paramMotion = nullptr;
    ID3D12Resource* paramDepth = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    // Order is important as that's the order of shader dispatch
    struct PostPass
    {
        PostPassStage stage;
        ID3D12Resource* inputBuffer = nullptr;
        ID3D12Resource* outputBuffer = nullptr;
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
    ID3D12Resource* currentTarget = paramOutput;
    for (size_t i = passCount; i-- > 0;)
    {
        auto& pass = pipeline[i];
        ID3D12Resource* requiredInput = nullptr;

        switch (pass.stage)
        {
        case PostPassStage::OutputScaling:
            if (OutputScaler->CreateBufferResource(Device, currentTarget, TargetWidth(), TargetHeight(),
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
            {
                OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                requiredInput = OutputScaler->Buffer();
            }
            break;

        case PostPassStage::Rcas:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput &&
                RCAS->CreateBufferResource(Device, currentTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                RCAS->CanRender())
            {
                // Disable any built-in sharpness shaders
                InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                _actualSharpness = _sharpness;
                _sharpness = 0.0f;

                RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                requiredInput = RCAS->Buffer();
            }
            break;

        case PostPassStage::Magnifier:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput &&
                Magnifier->CreateBufferResource(Device, currentTarget, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                Magnifier->CanRender())
            {
                Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                requiredInput = Magnifier->Buffer();
            }
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

    UpscalerTime->Start(InCommandList);

    _barrierTransitions.clear();

    // Game's real output, our pass buffers track their own state
    std::optional<int32_t> outputBarrier = Config::Instance()->OutputResourceBarrier;
    if (paramOutput && outputBarrier.has_value() && upscaler != Upscaler::DLSS && upscaler != Upscaler::DLSSD)
    {
        ForwardBarrier(InCommandList, paramOutput, (D3D12_RESOURCE_STATES) outputBarrier.value(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    auto evalResult = EvaluateInternal(InCommandList, InParameters);

    UpscalerTime->End(InCommandList);

    InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput);

    if (hasSharpness)
        InParameters->Set(NVSDK_NGX_Parameter_Sharpness, originalSharpness);

    _pendingAsyncPasses = {};

    if (!evalResult)
    {
        RestoreBarriers(InCommandList);
        return false;
    }

    const bool deferToAsyncQueue = _deferPostPasses && passCount > 0 && !outputBarrier.has_value() &&
                                   (Config::Instance()->OverlayMenu.value_or_default() || _frameCount <= 30) &&
                                   Config::Instance()->Dx12AsyncCompute.value_or_default() &&
                                   AsyncCompute != nullptr && AsyncCompute->IsInit();

    if (deferToAsyncQueue)
    {
        PendingAsyncPostPasses pending {};
        pending.paramMotion = paramMotion;
        pending.paramDepth = paramDepth;

        for (size_t i = 0; i < passCount; ++i)
        {
            const auto& pass = pipeline[i];
            if (!pass.inputBuffer || !pass.outputBuffer)
                continue;

            switch (pass.stage)
            {
            case PostPassStage::OutputScaling:
                if (OutputScaler)
                {
                    pending.runOutputScaler = true;
                    pending.osInput = pass.inputBuffer;
                    pending.osOutput = pass.outputBuffer;
                    pending.active = true;
                }
                break;

            case PostPassStage::Rcas:
                if (RCAS && RCAS->CanRender() && paramMotion && paramOutput)
                {
                    pending.runRcas = true;
                    pending.rcasInput = pass.inputBuffer;
                    pending.rcasOutput = pass.outputBuffer;
                    pending.rcasConstants = BuildRcasConstants(InParameters);
                    pending.active = true;
                }
                break;

            case PostPassStage::Magnifier:
                if (Magnifier && Magnifier->CanRender() && paramMotion && paramOutput)
                {
                    pending.runMagnifier = true;
                    pending.magInput = pass.inputBuffer;
                    pending.magOutput = pass.outputBuffer;
                    pending.active = true;
                }
                break;
            }
        }

        if (pending.active)
        {
            _pendingAsyncPasses = pending;
            RestoreBarriers(InCommandList);
            return evalResult;
        }
    }

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
            OutputScaler->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            if (!OutputScaler->Dispatch(InCommandList, pass.inputBuffer, pass.outputBuffer))
            {
                Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                State::Instance().changeBackend[Handle()->Id] = true;
                ok = false;
            }
            break;

        case PostPassStage::Rcas:
            if (RCAS->CanRender() && paramMotion && paramOutput)
            {
                RCAS->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                const RcasConstants rcasConstants = BuildRcasConstants(InParameters);

                if (!RCAS->Dispatch(InCommandList, pass.inputBuffer, paramMotion, rcasConstants, pass.outputBuffer,
                                    paramDepth))
                {
                    Config::Instance()->RcasEnabled.set_volatile_value(false);
                    ok = false;
                }
            }
            break;

        case PostPassStage::Magnifier:
            if (Magnifier->CanRender() && paramMotion && paramOutput)
            {
                Magnifier->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ok = Magnifier->Dispatch(InCommandList, pass.inputBuffer, pass.outputBuffer);
            }
            break;
        }

        if (!ok)
        {
            RestoreBarriers(InCommandList);
            return true;
        }
    }

    // imgui
    if (!Config::Instance()->OverlayMenu.value_or_default() && _frameCount > 30)
    {
        if (Imgui != nullptr && Imgui.get() != nullptr)
        {
            if (Imgui->IsHandleDifferent())
            {
                Imgui.reset();
            }
            else
                Imgui->Render(InCommandList, paramOutput);
        }
        else
        {
            if (Imgui == nullptr || Imgui.get() == nullptr)
                Imgui = std::make_unique<Menu_Dx12>(GetForegroundWindow(), Device);
        }
    }

    // After Imgui, which expects the output in UAV
    RestoreBarriers(InCommandList);

    return evalResult;
}

std::optional<double> IFeature_Dx12::ReadUpscalerTime(void* commandQueueVoid)
{
    if (!UpscalerTime || !RCAS || !OutputScaler)
        return std::nullopt;

    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    lastUpscalerTime = UpscalerTime->ReadGpuTime(commandQueue);
    lastRcasTime = RCAS->ReadGpuTime(commandQueue);
    lastOutputScalingTime = OutputScaler->ReadGpuTime(commandQueue);

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime);
}

void IFeature_Dx12::ReadDetailedGpuTimes(void* commandQueueVoid, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    ID3D12CommandQueue* commandQueue = (ID3D12CommandQueue*) commandQueueVoid;

    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    auto magnifierTime = Magnifier->ReadGpuTime(commandQueue);

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}

IFeature_Dx12::IFeature_Dx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters) {}

IFeature_Dx12::~IFeature_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    Imgui.reset();
    OutputScaler.reset();
    RCAS.reset();
    Bias.reset();
    AsyncCompute.reset();
}
