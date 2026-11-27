#pragma once

#include "SysUtils.h"
#include <Config.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

struct NeuralLookConstants
{
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    int32_t DepthSubrectBaseX = 0;
    int32_t DepthSubrectBaseY = 0;
    int32_t MvSubrectBaseX = 0;
    int32_t MvSubrectBaseY = 0;

    float MvScaleX = 1.0f;
    float MvScaleY = 1.0f;
    float JitterOffsetX = 0.0f;
    float JitterOffsetY = 0.0f;
    float PreExposure = 1.0f;

    float CameraNear = 0.1f;
    float CameraFar = 1000.0f;
    float CameraVFov = 1.04719755f; // ~60 deg in radians
    float AspectRatio = 16.0f / 9.0f;
    float FusedRcasSharpness = 0.0f;

    bool DepthIsLinear = false;
    bool DepthIsReversed = false;
    bool IsHdr = false;
    bool ResetHistory = false;
    bool LowResMV = true;

    bool HasCameraBasis = false;
    float CameraPos[3] = { 0.0f, 0.0f, 0.0f };
    float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
    float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
    float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
};

class NL_Common
{
  public:
    enum class BudgetTier : int
    {
        Light = 0,  // All passes except SSR: 1-slice SSGI + 3-step Contact Shadows + Denoise + Composite
        Medium = 1, // All 6 passes: 2-slice Visibility-Bitmask SSGI + 4-step Contact Shadows + 8-step Hi-Z SSR + Denoise
        Full = 2    // All 6 passes: 2-slice Visibility-Bitmask SSGI + 6-step Contact Shadows + 14-step Hi-Z SSR + Denoise
    };

    void UpdateBudget(double gpuMs)
    {
        _lastGpuMs = gpuMs;

        if (_emaGpuMs <= 0.0)
            _emaGpuMs = gpuMs;
        else
            _emaGpuMs = _emaGpuMs * 0.85 + gpuMs * 0.15;

        const double maxBudgetMs = std::max(0.25, (double) Config::Instance()->NeuralLookMaxGpuMs.value_or_default());

        if (_emaGpuMs > maxBudgetMs)
        {
            _overBudgetFrames++;
            _underBudgetFrames = 0;

            if (_overBudgetFrames >= 10 && _activeTier > 0)
            {
                _activeTier--;
                _overBudgetFrames = 0;
            }
        }
        else if (_emaGpuMs < maxBudgetMs * 0.70)
        {
            _underBudgetFrames++;
            _overBudgetFrames = 0;

            if (_underBudgetFrames >= 30 && _activeTier < 2)
            {
                _activeTier++;
                _underBudgetFrames = 0;
            }
        }
        else
        {
            _overBudgetFrames = 0;
            _underBudgetFrames = 0;
        }
    }

    int ActiveTier() const { return _activeTier; }
    double LastGpuMs() const { return _lastGpuMs; }

  protected:
    static constexpr uint32_t kWorldHashCapacity = 131072u; // 131072 * 2 uints = 1 MB
    static constexpr uint32_t kSceneLightUints = 32u;
    static constexpr uint32_t kResolvedSceneLightFloat4s = 8u;
    static constexpr uint32_t kBilateralLumaBins = 8u;

    struct alignas(256) NLPrepInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t HalfWidth = 0;
        int32_t HalfHeight = 0;

        int32_t RenderWidth = 0;
        int32_t RenderHeight = 0;
        int32_t DepthWidth = 0;
        int32_t DepthHeight = 0;

        int32_t DepthSubrectBaseX = 0;
        int32_t DepthSubrectBaseY = 0;
        int32_t MvSubrectBaseX = 0;
        int32_t MvSubrectBaseY = 0;

        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;
        int32_t DisplaySizeMV = 0;
        int32_t LowResMV = 1;

        int32_t DepthIsLinear = 0;
        int32_t DepthIsReversed = 0;
        int32_t HasGBufferNormals = 0;
        int32_t InColorEncodeMode = 0;

        int32_t HasReactiveMask = 0;
        int32_t HasTransparencyMask = 0;
        int32_t HasExposureTex = 0;
        int32_t HasCameraBasis = 0;

        int32_t WorldHashEnabled = 1;
        int32_t ResetHistory = 0;
        int32_t FrameIndex = 0;
        int32_t _pad0 = 0;

        float DepthLinearA = 100.0f;
        float DepthLinearB = 1000.0f;
        float DepthLinearC = 999.9f;
        float SkyDepthThreshold = 500.0f;

        float TanHalfFov = 0.57735027f;
        float TanHalfFovAspect = 1.02640048f;
        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;

        float MotionTextureScale = 1.0f;
        float JitterOffsetX = 0.0f;
        float JitterOffsetY = 0.0f;
        float PreExposure = 1.0f;

        float CameraPos[3] = { 0.0f, 0.0f, 0.0f };
        float _padCam0 = 0.0f;
        float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float _padCam1 = 0.0f;
        float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float _padCam2 = 0.0f;
        float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
        float _padCam3 = 0.0f;
    };

    struct alignas(256) NLPyramidInternalConstants
    {
        int32_t HalfWidth = 0;
        int32_t HalfHeight = 0;
        int32_t Mip0Width = 0;
        int32_t Mip0Height = 0;

        int32_t Mip1Width = 0;
        int32_t Mip1Height = 0;
        int32_t Mip2Width = 0;
        int32_t Mip2Height = 0;

        int32_t Mip3Width = 0;
        int32_t Mip3Height = 0;
        int32_t Mip4Width = 0;
        int32_t Mip4Height = 0;

        int32_t HasCameraBasis = 0;
        int32_t FrameIndex = 0;
        int32_t _pad0 = 0;
        int32_t _pad1 = 0;

        float SkyDepthThreshold = 500.0f;
        float TanHalfFov = 0.57735027f;
        float TanHalfFovAspect = 1.02640048f;
        float _pad2 = 0.0f;

        float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float _padCam0 = 0.0f;
        float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float _padCam1 = 0.0f;
        float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
        float _padCam2 = 0.0f;
    };

    struct alignas(256) NLNeuralPriorInternalConstants
    {
        int32_t EighthWidth = 0;
        int32_t EighthHeight = 0;
        int32_t Mip3Width = 0;
        int32_t Mip3Height = 0;

        int32_t GridWidth = 0;
        int32_t GridHeight = 0;
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;

        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;
        int32_t DisplaySizeMV = 0;
        int32_t LowResMV = 1;

        int32_t RenderWidth = 0;
        int32_t RenderHeight = 0;
        int32_t MvSubrectBaseX = 0;
        int32_t MvSubrectBaseY = 0;

        int32_t ResetHistory = 0;
        int32_t FrameIndex = 0;
        int32_t HasCameraBasis = 0;
        int32_t TotalTiles = 1;

        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;
        float SkyDepthThreshold = 500.0f;
        float _pad0 = 0.0f;

        float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float _padCam0 = 0.0f;
        float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float _padCam1 = 0.0f;
        float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
        float _padCam2 = 0.0f;
    };

    struct alignas(256) NLGiInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t GiWidth = 0;
        int32_t GiHeight = 0;

        int32_t RenderWidth = 0;
        int32_t RenderHeight = 0;
        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;

        int32_t MvSubrectBaseX = 0;
        int32_t MvSubrectBaseY = 0;
        int32_t DisplaySizeMV = 0;
        int32_t LowResMV = 1;

        int32_t ResetHistory = 0;
        int32_t FrameIndex = 0;
        int32_t RayCount = 2;
        int32_t StepCount = 5;

        int32_t SsrEnabled = 1;
        int32_t SsrSteps = 12;
        int32_t HasCameraBasis = 0;
        int32_t WorldHashEnabled = 1;

        float SkyDepthThreshold = 500.0f;
        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;
        float TanHalfFov = 0.57735027f;

        float TanHalfFovAspect = 1.02640048f;
        float GiRadius = 96.0f;
        float GiBias = 0.04f;
        float ContactShadowSteps = 6.0f;

        float CameraPos[3] = { 0.0f, 0.0f, 0.0f };
        float _padCam0 = 0.0f;
        float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float _padCam1 = 0.0f;
        float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float _padCam2 = 0.0f;
        float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
        float _padCam3 = 0.0f;
    };

    struct alignas(256) NLDenoiseInternalConstants
    {
        int32_t GiWidth = 0;
        int32_t GiHeight = 0;
        int32_t StepSize = 1;
        int32_t SsrEnabled = 1;

        float SkyDepthThreshold = 500.0f;
        float PhiDepth = 0.045f;
        float PhiNormal = 32.0f;
        float PhiLuma = 1.25f;
    };

    struct alignas(256) NLCompositeInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t GiWidth = 0;
        int32_t GiHeight = 0;

        int32_t RenderWidth = 0;
        int32_t RenderHeight = 0;
        int32_t DepthWidth = 0;
        int32_t DepthHeight = 0;

        int32_t DepthSubrectBaseX = 0;
        int32_t DepthSubrectBaseY = 0;
        int32_t MvSubrectBaseX = 0;
        int32_t MvSubrectBaseY = 0;

        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;
        int32_t DisplaySizeMV = 0;
        int32_t LowResMV = 1;

        int32_t DepthIsLinear = 0;
        int32_t DepthIsReversed = 0;
        int32_t InColorEncodeMode = 0;
        int32_t OutColorEncodeMode = 0;

        int32_t GiEnabled = 1;
        int32_t SsrEnabled = 1;
        int32_t HasGBufferNormals = 0;
        int32_t HasGBufferAlbedo = 0;

        int32_t HasGBufferRoughness = 0;
        int32_t HasGBufferMetallic = 0;
        int32_t HasGBufferSubsurface = 0;
        int32_t HasCameraBasis = 0;

        int32_t WorldHashEnabled = 1;
        int32_t DebugView = 0;
        int32_t FrameIndex = 0;
        int32_t _pad0 = 0;

        float DepthLinearA = 100.0f;
        float DepthLinearB = 1000.0f;
        float DepthLinearC = 999.9f;
        float SkyDepthThreshold = 500.0f;

        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;
        float JitterOffsetX = 0.0f;
        float JitterOffsetY = 0.0f;

        float TanHalfFov = 0.57735027f;
        float TanHalfFovAspect = 1.02640048f;
        float GiIntensity = 1.00f;
        float AoIntensity = 0.85f;

        float SsrIntensity = 0.85f;
        float RelightIntensity = 0.75f;
        float VolumetricGlow = 0.45f;
        float NeuralDetail = 0.55f;

        float SpecularRelief = 0.50f;
        float ToneContrast = 0.32f;
        float SssIntensity = 0.75f;
        float ContactShadowIntensity = 0.70f;

        float SplitScreenPos = 0.0f;
        float FusedRcasSharpness = 0.0f;
        float PreExposure = 1.0f;
        float _pad1 = 0.0f;

        float CameraPos[3] = { 0.0f, 0.0f, 0.0f };
        float _padCam0 = 0.0f;
        float CameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float _padCam1 = 0.0f;
        float CameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float _padCam2 = 0.0f;
        float CameraFwd[3] = { 0.0f, 0.0f, 1.0f };
        float _padCam3 = 0.0f;
    };

    static_assert(sizeof(NLPrepInternalConstants) % 16 == 0);
    static_assert(sizeof(NLPyramidInternalConstants) % 16 == 0);
    static_assert(sizeof(NLNeuralPriorInternalConstants) % 16 == 0);
    static_assert(sizeof(NLGiInternalConstants) % 16 == 0);
    static_assert(sizeof(NLDenoiseInternalConstants) % 16 == 0);
    static_assert(sizeof(NLCompositeInternalConstants) % 16 == 0);

    int _activeTier = 2;
    double _emaGpuMs = 0.0;
    double _lastGpuMs = 0.0;
    int _overBudgetFrames = 0;
    int _underBudgetFrames = 0;
    uint32_t _frameIndex = 0;

    static void ComputeTanFov(int32_t outW, int32_t outH, const NeuralLookConstants& inConst, float& outTanHalfFov,
                              float& outTanHalfFovAspect)
    {
        const float vFov = std::clamp(inConst.CameraVFov, 0.25f, 2.4f);
        const float aspect =
            (inConst.AspectRatio > 0.1f) ? inConst.AspectRatio : ((float) outW / (float) std::max(outH, 1));
        outTanHalfFov = std::tan(vFov * 0.5f);
        outTanHalfFovAspect = outTanHalfFov * aspect;
    }

    static float ComputeDepthUnitScale(const NeuralLookConstants& inConst)
    {
        // Engines like Unreal Engine pass CameraNear in centimeters (e.g. 10.0 cm).
        // Normalize to canonical meters so world-space GI/AO/Contact Shadow/Volumetric ranges are engine-invariant.
        return (inConst.CameraNear >= 2.0f) ? 0.01f : 1.0f;
    }

    static float ComputeScaledSkyThreshold(const NeuralLookConstants& inConst)
    {
        const float unitScale = ComputeDepthUnitScale(inConst);
        const float nearP = std::max(inConst.CameraNear * unitScale, 0.001f);
        const float farP = std::max(inConst.CameraFar * unitScale, nearP + 1.0f);
        // Cap below FP16 max (65504) so HalfColorDepthTex / ColorPyramidTex never overflow to +INF.
        return std::min(farP * 0.85f, 32000.0f);
    }

    static void CopyCameraBasis(const NeuralLookConstants& inConst, float outPos[3], float outRight[3], float outUp[3],
                                float outFwd[3])
    {
        if (outPos)
        {
            const float unitScale = ComputeDepthUnitScale(inConst);
            outPos[0] = inConst.CameraPos[0] * unitScale;
            outPos[1] = inConst.CameraPos[1] * unitScale;
            outPos[2] = inConst.CameraPos[2] * unitScale;
        }
        std::memcpy(outRight, inConst.CameraRight, 3 * sizeof(float));
        std::memcpy(outUp, inConst.CameraUp, 3 * sizeof(float));
        std::memcpy(outFwd, inConst.CameraFwd, 3 * sizeof(float));
    }

    static void ResolveRenderDims(int32_t outW, int32_t outH, int32_t depthW, int32_t depthH,
                                  const NeuralLookConstants& inConst, int32_t& outRenderW, int32_t& outRenderH)
    {
        outRenderW = (inConst.RenderWidth > 0) ? static_cast<int32_t>(inConst.RenderWidth)
                                               : ((depthW > 0) ? depthW : outW);
        outRenderH = (inConst.RenderHeight > 0) ? static_cast<int32_t>(inConst.RenderHeight)
                                                : ((depthH > 0) ? depthH : outH);
    }

    static void FillDepthLinearParams(const NeuralLookConstants& inConst, int32_t& outDepthLinear,
                                      int32_t& outDepthReversed, float& outLinearA, float& outLinearB,
                                      float& outLinearC, float& outSkyThreshold)
    {
        const float unitScale = ComputeDepthUnitScale(inConst);
        const float nearP = std::max(inConst.CameraNear * unitScale, 0.001f);
        const float farP = std::max(inConst.CameraFar * unitScale, nearP + 1.0f);
        outDepthLinear = inConst.DepthIsLinear ? 1 : 0;
        outDepthReversed = inConst.DepthIsReversed ? 1 : 0;
        outLinearA = nearP * farP;
        outLinearB = farP;
        outLinearC = farP - nearP;
        outSkyThreshold = std::min(farP * 0.85f, 32000.0f);
    }

    void FillPrepConstants(NLPrepInternalConstants& c, const NeuralLookConstants& inConst, bool hasNormals,
                           bool hasReactive, bool hasTransparency, bool hasExposure, bool worldHashEnabled,
                           int32_t inEncodeMode) const
    {
        ResolveRenderDims(c.OutputWidth, c.OutputHeight, c.DepthWidth, c.DepthHeight, inConst, c.RenderWidth,
                          c.RenderHeight);
        FillDepthLinearParams(inConst, c.DepthIsLinear, c.DepthIsReversed, c.DepthLinearA, c.DepthLinearB,
                              c.DepthLinearC, c.SkyDepthThreshold);

        c.DepthSubrectBaseX = inConst.DepthSubrectBaseX;
        c.DepthSubrectBaseY = inConst.DepthSubrectBaseY;
        c.MvSubrectBaseX = inConst.MvSubrectBaseX;
        c.MvSubrectBaseY = inConst.MvSubrectBaseY;

        c.LowResMV = inConst.LowResMV ? 1 : 0;
        c.DisplaySizeMV = (!inConst.LowResMV) ? 1 : 0;
        c.HasGBufferNormals = hasNormals ? 1 : 0;
        c.InColorEncodeMode = inEncodeMode;

        c.HasReactiveMask = hasReactive ? 1 : 0;
        c.HasTransparencyMask = hasTransparency ? 1 : 0;
        c.HasExposureTex = hasExposure ? 1 : 0;
        c.HasCameraBasis = inConst.HasCameraBasis ? 1 : 0;

        c.WorldHashEnabled = (worldHashEnabled && inConst.HasCameraBasis) ? 1 : 0;
        c.ResetHistory = inConst.ResetHistory ? 1 : 0;
        c.FrameIndex = static_cast<int32_t>(_frameIndex);

        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);
        c.MvScaleX = inConst.MvScaleX;
        c.MvScaleY = inConst.MvScaleY;
        c.MotionTextureScale = c.DisplaySizeMV ? 1.0f : ((float) c.MotionWidth / (float) std::max(c.OutputWidth, 1));
        c.JitterOffsetX = inConst.JitterOffsetX;
        c.JitterOffsetY = inConst.JitterOffsetY;
        c.PreExposure = std::max(inConst.PreExposure, 1e-4f);

        CopyCameraBasis(inConst, c.CameraPos, c.CameraRight, c.CameraUp, c.CameraFwd);
    }

    void FillPyramidConstants(NLPyramidInternalConstants& c, const NeuralLookConstants& inConst, int32_t outW,
                              int32_t outH) const
    {
        c.HasCameraBasis = inConst.HasCameraBasis ? 1 : 0;
        c.FrameIndex = static_cast<int32_t>(_frameIndex);
        c.SkyDepthThreshold = ComputeScaledSkyThreshold(inConst);
        ComputeTanFov(outW, outH, inConst, c.TanHalfFov, c.TanHalfFovAspect);
        CopyCameraBasis(inConst, nullptr, c.CameraRight, c.CameraUp, c.CameraFwd);
    }

    void FillNeuralPriorConstants(NLNeuralPriorInternalConstants& c, const NeuralLookConstants& inConst) const
    {
        ResolveRenderDims(c.OutputWidth, c.OutputHeight, 0, 0, inConst, c.RenderWidth, c.RenderHeight);
        c.MvSubrectBaseX = inConst.MvSubrectBaseX;
        c.MvSubrectBaseY = inConst.MvSubrectBaseY;
        c.LowResMV = inConst.LowResMV ? 1 : 0;
        c.DisplaySizeMV = (!inConst.LowResMV) ? 1 : 0;
        c.ResetHistory = inConst.ResetHistory ? 1 : 0;
        c.FrameIndex = static_cast<int32_t>(_frameIndex);
        c.HasCameraBasis = inConst.HasCameraBasis ? 1 : 0;
        c.TotalTiles = std::max(1, c.GridWidth * c.GridHeight);
        c.MvScaleX = inConst.MvScaleX;
        c.MvScaleY = inConst.MvScaleY;
        c.SkyDepthThreshold = ComputeScaledSkyThreshold(inConst);
        CopyCameraBasis(inConst, nullptr, c.CameraRight, c.CameraUp, c.CameraFwd);
    }

    void FillGiConstants(NLGiInternalConstants& c, const NeuralLookConstants& inConst, bool ssrEnabled,
                         bool worldHashEnabled) const
    {
        ResolveRenderDims(c.OutputWidth, c.OutputHeight, 0, 0, inConst, c.RenderWidth, c.RenderHeight);
        c.MvSubrectBaseX = inConst.MvSubrectBaseX;
        c.MvSubrectBaseY = inConst.MvSubrectBaseY;
        c.LowResMV = inConst.LowResMV ? 1 : 0;
        c.DisplaySizeMV = (!inConst.LowResMV) ? 1 : 0;
        c.ResetHistory = inConst.ResetHistory ? 1 : 0;
        c.FrameIndex = static_cast<int32_t>(_frameIndex);

        c.RayCount = (_activeTier == 0) ? 1 : 2;
        c.StepCount = (_activeTier >= 2) ? 6 : 4;
        c.SsrEnabled = ssrEnabled ? 1 : 0;
        c.SsrSteps = (_activeTier >= 2) ? 14 : 8;
        c.HasCameraBasis = inConst.HasCameraBasis ? 1 : 0;
        c.WorldHashEnabled = (worldHashEnabled && inConst.HasCameraBasis) ? 1 : 0;

        c.SkyDepthThreshold = ComputeScaledSkyThreshold(inConst);
        c.MvScaleX = inConst.MvScaleX;
        c.MvScaleY = inConst.MvScaleY;
        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);

        const float minDim = (float) std::min(c.OutputWidth, c.OutputHeight);
        c.GiRadius = std::clamp(minDim * 0.16f, 64.0f, 220.0f);
        c.GiBias = 0.04f;
        c.ContactShadowSteps = (_activeTier >= 2) ? 6.0f : ((_activeTier == 1) ? 4.0f : 3.0f);

        CopyCameraBasis(inConst, c.CameraPos, c.CameraRight, c.CameraUp, c.CameraFwd);
    }

    void FillDenoiseConstants(NLDenoiseInternalConstants& c, const NeuralLookConstants& inConst, int32_t giW,
                              int32_t giH, bool ssrEnabled) const
    {
        c.GiWidth = giW;
        c.GiHeight = giH;
        c.StepSize = 1;
        c.SsrEnabled = ssrEnabled ? 1 : 0;
        c.SkyDepthThreshold = ComputeScaledSkyThreshold(inConst);
        c.PhiDepth = 0.045f;
        c.PhiNormal = 32.0f;
        c.PhiLuma = 1.25f;
    }

    void FillCompositeConstants(NLCompositeInternalConstants& c, const NeuralLookConstants& inConst, bool giEnabled,
                                bool ssrEnabled, bool hasNormals, bool hasAlbedo, bool hasRoughness, bool hasMetallic,
                                bool hasSubsurface, bool worldHashEnabled, int32_t inEncodeMode,
                                int32_t outEncodeMode) const
    {
        ResolveRenderDims(c.OutputWidth, c.OutputHeight, c.DepthWidth, c.DepthHeight, inConst, c.RenderWidth,
                          c.RenderHeight);
        FillDepthLinearParams(inConst, c.DepthIsLinear, c.DepthIsReversed, c.DepthLinearA, c.DepthLinearB,
                              c.DepthLinearC, c.SkyDepthThreshold);

        c.DepthSubrectBaseX = inConst.DepthSubrectBaseX;
        c.DepthSubrectBaseY = inConst.DepthSubrectBaseY;
        c.MvSubrectBaseX = inConst.MvSubrectBaseX;
        c.MvSubrectBaseY = inConst.MvSubrectBaseY;

        c.LowResMV = inConst.LowResMV ? 1 : 0;
        c.DisplaySizeMV = (!inConst.LowResMV) ? 1 : 0;
        c.InColorEncodeMode = inEncodeMode;
        c.OutColorEncodeMode = outEncodeMode;

        c.GiEnabled = giEnabled ? 1 : 0;
        c.SsrEnabled = ssrEnabled ? 1 : 0;
        c.HasGBufferNormals = hasNormals ? 1 : 0;
        c.HasGBufferAlbedo = hasAlbedo ? 1 : 0;
        c.HasGBufferRoughness = hasRoughness ? 1 : 0;
        c.HasGBufferMetallic = hasMetallic ? 1 : 0;
        c.HasGBufferSubsurface = hasSubsurface ? 1 : 0;
        c.HasCameraBasis = inConst.HasCameraBasis ? 1 : 0;
        c.WorldHashEnabled = (worldHashEnabled && inConst.HasCameraBasis) ? 1 : 0;

        auto* cfg = Config::Instance();
        c.DebugView = cfg->NeuralLookDebugView.value_or_default();
        c.FrameIndex = static_cast<int32_t>(_frameIndex);

        c.MvScaleX = inConst.MvScaleX;
        c.MvScaleY = inConst.MvScaleY;
        c.JitterOffsetX = inConst.JitterOffsetX;
        c.JitterOffsetY = inConst.JitterOffsetY;
        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);

        c.GiIntensity = std::clamp(cfg->NeuralLookGiIntensity.value_or_default(), 0.0f, 2.0f);
        c.AoIntensity = std::clamp(cfg->NeuralLookAoIntensity.value_or_default(), 0.0f, 2.0f);
        c.SsrIntensity = std::clamp(cfg->NeuralLookSsrIntensity.value_or_default(), 0.0f, 2.0f);
        c.RelightIntensity = std::clamp(cfg->NeuralLookRelightIntensity.value_or_default(), 0.0f, 2.0f);
        c.VolumetricGlow = std::clamp(cfg->NeuralLookVolumetricGlow.value_or_default(), 0.0f, 2.0f);
        c.NeuralDetail = std::clamp(cfg->NeuralLookDetail.value_or_default(), 0.0f, 2.0f);
        c.SpecularRelief = std::clamp(cfg->NeuralLookSpecularRelief.value_or_default(), 0.0f, 2.0f);
        c.ToneContrast = std::clamp(cfg->NeuralLookToneContrast.value_or_default(), 0.0f, 1.0f);
        c.SssIntensity = std::clamp(cfg->NeuralLookSssIntensity.value_or_default(), 0.0f, 2.0f);
        c.ContactShadowIntensity = std::clamp(cfg->NeuralLookContactShadowIntensity.value_or_default(), 0.0f, 2.0f);
        c.SplitScreenPos = std::clamp(cfg->NeuralLookSplitScreen.value_or_default(), 0.0f, 1.0f);
        c.FusedRcasSharpness = std::clamp(inConst.FusedRcasSharpness, 0.0f, 1.0f);
        c.PreExposure = std::max(inConst.PreExposure, 1e-4f);

        CopyCameraBasis(inConst, c.CameraPos, c.CameraRight, c.CameraUp, c.CameraFwd);
    }
};
