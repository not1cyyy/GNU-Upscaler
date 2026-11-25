#include <pch.h>

#include "IFeature_Vk.h"
#include "State.h"
#include "nvsdk_ngx_vk.h"
#include <misc/DeferredRelease.h>

bool IFeature_Vk::Init(VkInstance InInstance, VkPhysicalDevice InPD, VkDevice InDevice, VkCommandBuffer InCmdBuffer,
                       PFN_vkGetInstanceProcAddr InGIPA, PFN_vkGetDeviceProcAddr InGDPA,
                       NVSDK_NGX_Parameter* InParameters)
{
    Instance = InInstance;
    PhysicalDevice = InPD;
    Device = InDevice;
    GIPA = InGIPA;
    GDPA = InGDPA;

    auto result = InitInternal(InCmdBuffer, InParameters);

    if (result)
    {
        OutputScaler = std::make_unique<OS_Vk>("Output Scaling", InDevice, InPD, (TargetWidth() < DisplayWidth()));
        NeuralLook = std::make_unique<NL_Vk>("Neural Look", InDevice, InPD);
        _neuralLookActiveTier = (NeuralLook && NeuralLook->IsInit()) ? NeuralLook->ActiveTier() : -1;
        RCAS = std::make_unique<RCAS_Vk>("RCAS", InDevice, InPD);
        // DLSS/DLSSD take the bias mask as is
        if (GetUpscalerType() != Upscaler::DLSS && GetUpscalerType() != Upscaler::DLSSD)
            Bias = std::make_unique<Bias_Vk>("Bias", InDevice, InPD);
        Magnifier = std::make_unique<Magnifier_Vk>("Magnifier", InDevice, InPD);
        UpscalerTime = std::make_unique<GpuTime_Vk>(InDevice, InPD);
    }

    return result;
}

bool IFeature_Vk::Evaluate(VkCommandBuffer InCmdBuffer, NVSDK_NGX_Parameter* InParameters)
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

    auto upscaler = GetUpscalerType();
    bool useRcas = upscaler == Upscaler::XeSS ||
                   (upscaler == Upscaler::DLSS && Version() >= feature_version(2, 5, 1)) || upscaler == Upscaler::DLSSD;

    if (!useRcas)
        useRcas = Config::Instance()->RcasEnabled.value_or_default();

    if (_sharpness == 0.0f)
        useRcas = false;

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

    NVSDK_NGX_Resource_VK* paramOutput = nullptr;
    NVSDK_NGX_Resource_VK* paramMotion = nullptr;
    NVSDK_NGX_Resource_VK* paramDepth = nullptr;
    NVSDK_NGX_Resource_VK* paramGBufferNormals = nullptr;
    NVSDK_NGX_Resource_VK* paramGBufferAlbedo = nullptr;
    NVSDK_NGX_Resource_VK* paramGBufferRoughness = nullptr;

    InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &paramOutput);
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramMotion);
    InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

    if (Config::Instance()->NeuralLookUseGBuffers.value_or_default())
    {
        InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Normals, (void**) &paramGBufferNormals);
        if (InParameters->Get(NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo, (void**) &paramGBufferAlbedo) !=
            NVSDK_NGX_Result_Success)
        {
            InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Albedo, (void**) &paramGBufferAlbedo);
        }
        InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Roughness, (void**) &paramGBufferRoughness);
    }

    const bool useNeuralLook = Config::Instance()->NeuralLookEnabled.value_or_default() && NeuralLook != nullptr &&
                               NeuralLook->IsInit() && paramDepth != nullptr && paramMotion != nullptr;

    const bool fuseRcasIntoNeuralLook =
        useNeuralLook && useRcas && Config::Instance()->SharpnessShader.value_or_default() == SharpenShader::RCAS &&
        !Config::Instance()->MotionSharpnessEnabled.value_or_default() &&
        !Config::Instance()->MotionSharpnessDebug.value_or_default();

    // Save the original output so we can restore it later
    VkImageInfo originalOutput {};
    if (paramOutput)
    {
        originalOutput.Image = paramOutput->Resource.ImageViewInfo.Image;
        originalOutput.ImageView = paramOutput->Resource.ImageViewInfo.ImageView;
        originalOutput.SubresourceRange = paramOutput->Resource.ImageViewInfo.SubresourceRange;
        originalOutput.Format = paramOutput->Resource.ImageViewInfo.Format;
        originalOutput.Width = paramOutput->Resource.ImageViewInfo.Width;
        originalOutput.Height = paramOutput->Resource.ImageViewInfo.Height;
    }

    const VkImageUsageFlags intermediateUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Order is important as that's the order of shader dispatch
    struct PostPass
    {
        PostPassStage stage;
        VkImageInfo inputBuffer {};
        VkImageInfo outputBuffer {};
    };

    std::array<PostPass, 4> pipeline {};
    size_t passCount = 0;

    if (useOutputScaling)
        pipeline[passCount++].stage = PostPassStage::OutputScaling;

    if (useNeuralLook)
        pipeline[passCount++].stage = PostPassStage::NeuralLook;

    _actualSharpness.reset();
    if (useRcas && !fuseRcasIntoNeuralLook)
        pipeline[passCount++].stage = PostPassStage::Rcas;

    if (Magnifier->ShouldRun())
        pipeline[passCount++].stage = PostPassStage::Magnifier;

    // RCAS setup and DLSS zero the game's sharpness, restored after EvaluateInternal
    float originalSharpness = 0.0f;
    bool hasSharpness =
        InParameters->Get(NVSDK_NGX_Parameter_Sharpness, &originalSharpness) == NVSDK_NGX_Result_Success;

    // Iterate BACKWARDS to establish where each shader needs to pull its input from
    VkImageInfo currentTarget = originalOutput;
    for (size_t i = passCount; i-- > 0;)
    {
        auto& pass = pipeline[i];
        VkImageInfo requiredInput {};

        switch (pass.stage)
        {
        case PostPassStage::OutputScaling:
            if (OutputScaler->CreateImageResource(Device, PhysicalDevice, TargetWidth(), TargetHeight(),
                                                  currentTarget.Format, intermediateUsage))
            {
                OutputScaler->SetImageLayout(InCmdBuffer, OutputScaler->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                             VK_IMAGE_LAYOUT_GENERAL, currentTarget.SubresourceRange);

                // TODO: improve query of the output info
                VkImageInfo info = currentTarget;
                info.Image = OutputScaler->GetImage();
                info.ImageView = OutputScaler->GetImageView();
                info.Width = TargetWidth();
                info.Height = TargetHeight();
                requiredInput = info;
            }
            break;

        case PostPassStage::NeuralLook:
            if (paramMotion && paramDepth && paramOutput && NeuralLook->IsInit() &&
                NeuralLook->CreateImageResource(Device, PhysicalDevice, currentTarget.Width, currentTarget.Height,
                                                currentTarget.Format, intermediateUsage))
            {
                if (fuseRcasIntoNeuralLook)
                {
                    InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                    _actualSharpness = _sharpness;
                    _sharpness = 0.0f;
                }

                NeuralLook->SetImageLayout(InCmdBuffer, NeuralLook->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_GENERAL, currentTarget.SubresourceRange);

                VkImageInfo info = currentTarget;
                info.Image = NeuralLook->GetImage();
                info.ImageView = NeuralLook->GetImageView();
                requiredInput = info;
            }
            break;

        case PostPassStage::Rcas:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput && RCAS->CanRender() &&
                RCAS->CreateImageResource(Device, PhysicalDevice, currentTarget.Width, currentTarget.Height,
                                          currentTarget.Format, intermediateUsage))
            {
                // Disable any built-in sharpness shaders
                InParameters->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
                _actualSharpness = _sharpness;
                _sharpness = 0.0f;

                RCAS->SetImageLayout(InCmdBuffer, RCAS->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_GENERAL, currentTarget.SubresourceRange);

                VkImageInfo info = currentTarget;
                info.Image = RCAS->GetImage();
                info.ImageView = RCAS->GetImageView();
                requiredInput = info;
            }
            break;

        case PostPassStage::Magnifier:
            // Same checks as Dispatch, a skipped pass must not take over the output
            if (paramMotion && paramOutput && Magnifier->CanRender() &&
                Magnifier->CreateImageResource(Device, PhysicalDevice, currentTarget.Width, currentTarget.Height,
                                               currentTarget.Format, intermediateUsage))
            {
                Magnifier->SetImageLayout(InCmdBuffer, Magnifier->GetImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                                          VK_IMAGE_LAYOUT_GENERAL, currentTarget.SubresourceRange);

                VkImageInfo info = currentTarget;
                info.Image = Magnifier->GetImage();
                info.ImageView = Magnifier->GetImageView();
                requiredInput = info;
            }
            break;
        }

        if (requiredInput.Image != VK_NULL_HANDLE)
        {
            pass.outputBuffer = currentTarget;
            pass.inputBuffer = requiredInput;
            currentTarget = requiredInput; // Shift the target back for the next previous stage
        }
    }

    // Write target back into the params
    // In DX11/DX12 we set ngx param but in Vulkan we can set just the resource info
    if (paramOutput)
    {
        paramOutput->Resource.ImageViewInfo.Image = currentTarget.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = currentTarget.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = currentTarget.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = currentTarget.Format;
        paramOutput->Resource.ImageViewInfo.Width = currentTarget.Width;
        paramOutput->Resource.ImageViewInfo.Height = currentTarget.Height;
    }

    UpscalerTime->Start(InCmdBuffer);

    auto evalResult = EvaluateInternal(InCmdBuffer, InParameters);

    UpscalerTime->End(InCmdBuffer);

    // Restore original output pointer
    if (paramOutput)
    {
        paramOutput->Resource.ImageViewInfo.Image = originalOutput.Image;
        paramOutput->Resource.ImageViewInfo.ImageView = originalOutput.ImageView;
        paramOutput->Resource.ImageViewInfo.SubresourceRange = originalOutput.SubresourceRange;
        paramOutput->Resource.ImageViewInfo.Format = originalOutput.Format;
        paramOutput->Resource.ImageViewInfo.Width = originalOutput.Width;
        paramOutput->Resource.ImageViewInfo.Height = originalOutput.Height;
    }

    if (hasSharpness)
        InParameters->Set(NVSDK_NGX_Parameter_Sharpness, originalSharpness);

    if (!evalResult)
        return false;

    // Iterate FORWARDS to execute the shaders in the defined order
    for (size_t i = 0; i < passCount; ++i)
    {
        const auto& pass = pipeline[i];
        if (pass.inputBuffer.Image == VK_NULL_HANDLE || pass.outputBuffer.Image == VK_NULL_HANDLE)
            continue;

        bool ok = true;
        switch (pass.stage)
        {
        case PostPassStage::OutputScaling:
            LOG_DEBUG("Scaling output...");
            OutputScaler->SetImageLayout(InCmdBuffer, pass.inputBuffer.Image, VK_IMAGE_LAYOUT_GENERAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pass.inputBuffer.SubresourceRange);

            if (!OutputScaler->Dispatch(InCmdBuffer, pass.inputBuffer, pass.outputBuffer))
            {
                Config::Instance()->OutputScalingEnabled.set_volatile_value(false);
                State::Instance().changeBackend[Handle()->Id] = true;
                ok = false;
            }
            break;

        case PostPassStage::NeuralLook:
            if (NeuralLook && NeuralLook->IsInit() && paramMotion && paramDepth && paramOutput)
            {
                NeuralLook->SetImageLayout(InCmdBuffer, pass.inputBuffer.Image, VK_IMAGE_LAYOUT_GENERAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pass.inputBuffer.SubresourceRange);

                NeuralLookConstants nlConstants {};
                nlConstants.DepthIsLinear = DepthLinear();
                nlConstants.DepthIsReversed = DepthInverted();
                nlConstants.IsHdr = IsHdr() || State::Instance().hdrOutputActive ||
                                    State::Instance().outputColorSpace.transfer == ColorTransfer::PQ;
                if (fuseRcasIntoNeuralLook)
                {
                    nlConstants.FusedRcasSharpness = _actualSharpness.value_or(_sharpness);
                    _sharpness = _actualSharpness.value_or(_sharpness);
                    _actualSharpness.reset();
                }

                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &nlConstants.MvScaleX);
                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &nlConstants.MvScaleY);

                float nearPlane = 0.0f;
                float farPlane = 0.0f;
                if (InParameters->Get("DLSSG.CameraNear", &nearPlane) == NVSDK_NGX_Result_Success &&
                    InParameters->Get("DLSSG.CameraFar", &farPlane) == NVSDK_NGX_Result_Success)
                {
                    nlConstants.CameraNear = nearPlane;
                    nlConstants.CameraFar = farPlane;
                }
                else
                {
                    nlConstants.CameraNear = Config::Instance()->FsrCameraNear.value_or_default();
                    nlConstants.CameraFar = Config::Instance()->FsrCameraFar.value_or_default();
                }

                float vFov = 0.0f;
                if (InParameters->Get("DLSSG.CameraVFov", &vFov) == NVSDK_NGX_Result_Success && vFov > 0.05f)
                    nlConstants.CameraVFov = vFov;

                float aspectRatio = 0.0f;
                if (InParameters->Get("DLSSG.CameraAspectRatio", &aspectRatio) == NVSDK_NGX_Result_Success &&
                    aspectRatio > 0.1f)
                {
                    nlConstants.AspectRatio = aspectRatio;
                }
                else if (TargetHeight() > 0)
                {
                    nlConstants.AspectRatio = static_cast<float>(TargetWidth()) / static_cast<float>(TargetHeight());
                }

                int resetFlag = 0;
                if (InParameters->Get(NVSDK_NGX_Parameter_Reset, &resetFlag) == NVSDK_NGX_Result_Success &&
                    resetFlag != 0)
                    nlConstants.ResetHistory = true;

                VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                VkImageInfo* depthInfo = paramDepth ? (VkImageInfo*) &paramDepth->Resource.ImageViewInfo : nullptr;
                VkImageInfo* normInfo =
                    paramGBufferNormals ? (VkImageInfo*) &paramGBufferNormals->Resource.ImageViewInfo : nullptr;
                VkImageInfo* albedoInfo =
                    paramGBufferAlbedo ? (VkImageInfo*) &paramGBufferAlbedo->Resource.ImageViewInfo : nullptr;
                VkImageInfo* roughInfo =
                    paramGBufferRoughness ? (VkImageInfo*) &paramGBufferRoughness->Resource.ImageViewInfo : nullptr;

                if (!NeuralLook->Dispatch(Device, InCmdBuffer, nlConstants, pass.inputBuffer, mvInfo, pass.outputBuffer,
                                          depthInfo, normInfo, albedoInfo, roughInfo))
                {
                    Config::Instance()->NeuralLookEnabled.set_volatile_value(false);
                    ok = false;
                }
            }
            break;

        case PostPassStage::Rcas:
            if (RCAS->CanRender() && paramMotion && paramOutput)
            {
                RCAS->SetImageLayout(InCmdBuffer, pass.inputBuffer.Image, VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pass.inputBuffer.SubresourceRange);

                RcasConstants rcasConstants {};
                rcasConstants.Sharpness = _actualSharpness.value_or(_sharpness);
                rcasConstants.DepthIsLinear = DepthLinear();
                rcasConstants.DepthIsReversed = DepthInverted();
                rcasConstants.IsHdr = IsHdr() || State::Instance().hdrOutputActive ||
                                      State::Instance().outputColorSpace.transfer == ColorTransfer::PQ;

                _sharpness = _actualSharpness.value_or(_sharpness);
                _actualSharpness.reset();

                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &rcasConstants.MvScaleX);
                InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &rcasConstants.MvScaleY);

                float nearPlane = 0.0f;
                float farPlane = 0.0f;

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

                // In Vulkan we pass the Info structs instead of raw resources
                VkImageInfo mvInfo = *(VkImageInfo*) &paramMotion->Resource.ImageViewInfo;
                VkImageInfo* depthInfo = paramDepth ? (VkImageInfo*) &paramDepth->Resource.ImageViewInfo : nullptr;

                if (!RCAS->Dispatch(Device, InCmdBuffer, rcasConstants, pass.inputBuffer, mvInfo, pass.outputBuffer,
                                    depthInfo))
                {
                    Config::Instance()->RcasEnabled.set_volatile_value(false);
                    ok = false;
                }
            }
            break;

        case PostPassStage::Magnifier:
            if (Magnifier->CanRender() && paramMotion && paramOutput)
            {
                Magnifier->SetImageLayout(InCmdBuffer, pass.inputBuffer.Image, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, pass.inputBuffer.SubresourceRange);

                ok = Magnifier->Dispatch(InCmdBuffer, pass.inputBuffer, pass.outputBuffer);
            }
            break;
        }

        // Upscale succeeded, a failed pass only drops the remaining passes
        if (!ok)
            break;
    }

    _frameCount++;

    return evalResult;
}

std::optional<double> IFeature_Vk::ReadUpscalerTime(void* unused)
{
    if (!UpscalerTime || !RCAS || !OutputScaler)
        return std::nullopt;

    lastUpscalerTime = UpscalerTime->ReadGpuTime();
    lastRcasTime = RCAS->ReadGpuTime();
    lastOutputScalingTime = OutputScaler->ReadGpuTime();
    lastNeuralLookTime = NeuralLook ? NeuralLook->ReadGpuTime() : std::nullopt;
    if (lastNeuralLookTime.has_value() && NeuralLook)
    {
        NeuralLook->UpdateBudget(lastNeuralLookTime.value());
        _neuralLookActiveTier = NeuralLook->ActiveTier();
    }

    return sumOpts(lastUpscalerTime, lastRcasTime, lastOutputScalingTime, lastNeuralLookTime);
}

void IFeature_Vk::ReadDetailedGpuTimes(void* unused, std::vector<DetailedGpuTime>& detailedGpuTimes)
{
    detailedGpuTimes.clear();

    // Do not call ReadGpuTime twice for shaders
    if (lastUpscalerTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { ShortName(), lastUpscalerTime.value(), true });

    if (lastNeuralLookTime && NeuralLook)
        detailedGpuTimes.emplace_back(DetailedGpuTime { NeuralLook->Name(), lastNeuralLookTime.value(), true });

    if (lastRcasTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { RCAS->Name(), lastRcasTime.value(), true });

    if (lastOutputScalingTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { OutputScaler->Name(), lastOutputScalingTime.value(), true });

    if (!Magnifier)
        return;

    auto magnifierTime = Magnifier->ReadGpuTime();

    if (magnifierTime)
        detailedGpuTimes.emplace_back(DetailedGpuTime { Magnifier->Name(), magnifierTime.value(), false });
}
