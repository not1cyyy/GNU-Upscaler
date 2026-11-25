#pragma once

#include "SysUtils.h"
#include <Config.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

struct NeuralLookConstants
{
    float MvScaleX = 1.0f;
    float MvScaleY = 1.0f;

    float CameraNear = 0.1f;
    float CameraFar = 1000.0f;
    float CameraVFov = 1.04719755f; // ~60 deg in radians
    float AspectRatio = 16.0f / 9.0f;
    float FusedRcasSharpness = 0.0f;

    bool DepthIsLinear = false;
    bool DepthIsReversed = false;
    bool IsHdr = false;
    bool ResetHistory = false;
};

class NL_Common
{
  public:
    enum class BudgetTier : int
    {
        Light = 0,  // Prep + Composite + Relight + Neural Detail only (skip SSGI/SSR)
        Medium = 1, // Prep + 3-ray Checkerboard SSGI + 6-step SSR + Composite
        Full = 2    // Prep + 4-ray Checkerboard SSGI + 8-step SSR + Composite
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
    struct alignas(256) NLPrepInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t HalfWidth = 0;
        int32_t HalfHeight = 0;

        int32_t DepthWidth = 0;
        int32_t DepthHeight = 0;
        int32_t DepthIsLinear = 0;
        int32_t DepthIsReversed = 0;

        float DepthLinearA = 100.0f;
        float DepthLinearB = 1000.0f;
        float DepthLinearC = 999.9f;
        float SkyDepthThreshold = 500.0f;

        float TanHalfFov = 0.57735027f;
        float TanHalfFovAspect = 1.02640048f;
        int32_t HasGBufferNormals = 0;
        int32_t InColorEncodeMode = 0;
    };

    struct alignas(256) NLGiInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t GiWidth = 0;
        int32_t GiHeight = 0;

        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;
        int32_t DisplaySizeMV = 0;
        int32_t ResetHistory = 0;

        float SkyDepthThreshold = 500.0f;
        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;
        float MotionTextureScale = 1.0f;

        float TanHalfFov = 0.57735027f;
        float TanHalfFovAspect = 1.02640048f;
        float GiRadius = 96.0f;
        float GiBias = 0.05f;

        int32_t RayCount = 4;
        int32_t StepCount = 5;
        int32_t FrameIndex = 0;
        int32_t SsrEnabled = 1;

        int32_t SsrSteps = 8;
        int32_t _pad0 = 0;
        int32_t _pad1 = 0;
        int32_t _pad2 = 0;
    };

    struct alignas(256) NLCompositeInternalConstants
    {
        int32_t OutputWidth = 0;
        int32_t OutputHeight = 0;
        int32_t GiWidth = 0;
        int32_t GiHeight = 0;

        int32_t DepthWidth = 0;
        int32_t DepthHeight = 0;
        int32_t MotionWidth = 0;
        int32_t MotionHeight = 0;

        int32_t DepthIsLinear = 0;
        int32_t DepthIsReversed = 0;
        int32_t DisplaySizeMV = 0;
        int32_t ColorSpaceMode = 0;

        float DepthLinearA = 100.0f;
        float DepthLinearB = 1000.0f;
        float DepthLinearC = 999.9f;
        float SkyDepthThreshold = 500.0f;

        float MvScaleX = 1.0f;
        float MvScaleY = 1.0f;
        float MotionTextureScale = 1.0f;
        float DepthTextureScale = 1.0f;

        float GiIntensity = 1.00f;
        float AoIntensity = 0.85f;
        float NeuralDetail = 0.80f;
        float SpecularRelief = 0.70f;

        float ToneContrast = 0.32f;
        int32_t InColorEncodeMode = 0;

        int32_t GiEnabled = 1;
        int32_t HasGBufferNormals = 0;
        int32_t HasGBufferAlbedo = 0;
        int32_t HasGBufferRoughness = 0;

        int32_t DebugView = 0;
        int32_t OutColorEncodeMode = 0;
        float SsrIntensity = 0.85f;
        float RelightIntensity = 0.75f;

        float VolumetricGlow = 0.45f;
        float SplitScreenPos = 0.0f;
        int32_t SsrEnabled = 1;
        float TanHalfFov = 0.57735027f;

        float TanHalfFovAspect = 1.02640048f;
        int32_t FrameIndex = 0;
        float FusedRcasSharpness = 0.0f;
        int32_t _pad2 = 0;
    };

    static_assert(sizeof(NLPrepInternalConstants) % 16 == 0);
    static_assert(sizeof(NLGiInternalConstants) % 16 == 0);
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

    static void FillSharedDepthMotion(int32_t outW, int32_t outH, int32_t depthW, int32_t depthH, int32_t motionW,
                                      int32_t motionH, const NeuralLookConstants& inConst, int32_t& outDepthLinear,
                                      int32_t& outDepthReversed, int32_t& outDisplaySizeMV, float& outLinearA,
                                      float& outLinearB, float& outLinearC, float& outSkyThreshold, float& outMvScaleX,
                                      float& outMvScaleY, float& outMotionTexScale, float& outDepthTexScale)
    {
        const float nearP = std::max(inConst.CameraNear, 0.001f);
        const float farP = std::max(inConst.CameraFar, nearP + 1.0f);

        outDepthLinear = inConst.DepthIsLinear ? 1 : 0;
        outDepthReversed = inConst.DepthIsReversed ? 1 : 0;
        outDisplaySizeMV = (motionW == outW) ? 1 : 0;

        outLinearA = nearP * farP;
        outLinearB = farP;
        outLinearC = farP - nearP;
        outSkyThreshold = farP * 0.85f;

        outMvScaleX = inConst.MvScaleX;
        outMvScaleY = inConst.MvScaleY;
        outMotionTexScale = outDisplaySizeMV ? 1.0f : ((float) motionW / (float) std::max(outW, 1));
        outDepthTexScale = (float) depthW / (float) std::max(outW, 1);
    }

    void FillPrepConstants(NLPrepInternalConstants& c, const NeuralLookConstants& inConst, bool hasNormals,
                           int32_t inEncodeMode) const
    {
        const float nearP = std::max(inConst.CameraNear, 0.001f);
        const float farP = std::max(inConst.CameraFar, nearP + 1.0f);

        c.DepthIsLinear = inConst.DepthIsLinear ? 1 : 0;
        c.DepthIsReversed = inConst.DepthIsReversed ? 1 : 0;
        c.DepthLinearA = nearP * farP;
        c.DepthLinearB = farP;
        c.DepthLinearC = farP - nearP;
        c.SkyDepthThreshold = farP * 0.85f;

        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);
        c.HasGBufferNormals = hasNormals ? 1 : 0;
        c.InColorEncodeMode = inEncodeMode;
    }

    void FillGiConstants(NLGiInternalConstants& c, const NeuralLookConstants& inConst, bool ssrEnabled) const
    {
        const float farP = std::max(inConst.CameraFar, std::max(inConst.CameraNear, 0.001f) + 1.0f);
        c.DisplaySizeMV = (c.MotionWidth == c.OutputWidth) ? 1 : 0;
        c.ResetHistory = inConst.ResetHistory ? 1 : 0;
        c.SkyDepthThreshold = farP * 0.85f;
        c.MvScaleX = inConst.MvScaleX;
        c.MvScaleY = inConst.MvScaleY;
        c.MotionTextureScale = c.DisplaySizeMV ? 1.0f : ((float) c.MotionWidth / (float) std::max(c.OutputWidth, 1));

        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);

        const float minDim = (float) std::min(c.OutputWidth, c.OutputHeight);
        c.GiRadius = std::clamp(minDim * 0.11f, 40.0f, 144.0f);
        c.GiBias = 0.05f;

        c.RayCount = (_activeTier >= 2) ? 4 : 3;
        c.StepCount = (_activeTier >= 2) ? 5 : 4;
        c.FrameIndex = static_cast<int32_t>(_frameIndex);
        c.SsrEnabled = ssrEnabled ? 1 : 0;
        c.SsrSteps = (_activeTier >= 2) ? 12 : 8;
    }

    void FillCompositeConstants(NLCompositeInternalConstants& c, const NeuralLookConstants& inConst, bool giEnabled,
                                bool ssrEnabled, bool hasNormals, bool hasAlbedo, bool hasRoughness,
                                int32_t inEncodeMode, int32_t outEncodeMode) const
    {
        FillSharedDepthMotion(c.OutputWidth, c.OutputHeight, c.DepthWidth, c.DepthHeight, c.MotionWidth, c.MotionHeight,
                              inConst, c.DepthIsLinear, c.DepthIsReversed, c.DisplaySizeMV, c.DepthLinearA,
                              c.DepthLinearB, c.DepthLinearC, c.SkyDepthThreshold, c.MvScaleX, c.MvScaleY,
                              c.MotionTextureScale, c.DepthTextureScale);

        auto* cfg = Config::Instance();
        c.GiIntensity = std::clamp(cfg->NeuralLookGiIntensity.value_or_default(), 0.0f, 2.0f);
        c.AoIntensity = std::clamp(cfg->NeuralLookAoIntensity.value_or_default(), 0.0f, 2.0f);
        c.NeuralDetail = std::clamp(cfg->NeuralLookDetail.value_or_default(), 0.0f, 2.0f);
        c.SpecularRelief = std::clamp(cfg->NeuralLookSpecularRelief.value_or_default(), 0.0f, 2.0f);
        c.ToneContrast = std::clamp(cfg->NeuralLookToneContrast.value_or_default(), 0.0f, 1.0f);
        c.SsrIntensity = std::clamp(cfg->NeuralLookSsrIntensity.value_or_default(), 0.0f, 2.0f);
        c.RelightIntensity = std::clamp(cfg->NeuralLookRelightIntensity.value_or_default(), 0.0f, 2.0f);
        c.VolumetricGlow = std::clamp(cfg->NeuralLookVolumetricGlow.value_or_default(), 0.0f, 2.0f);
        c.SplitScreenPos = std::clamp(cfg->NeuralLookSplitScreen.value_or_default(), 0.0f, 1.0f);

        c.GiEnabled = giEnabled ? 1 : 0;
        c.SsrEnabled = ssrEnabled ? 1 : 0;
        ComputeTanFov(c.OutputWidth, c.OutputHeight, inConst, c.TanHalfFov, c.TanHalfFovAspect);
        c.FrameIndex = static_cast<int32_t>(_frameIndex);
        c.FusedRcasSharpness = std::clamp(inConst.FusedRcasSharpness, 0.0f, 1.0f);
        c.HasGBufferNormals = hasNormals ? 1 : 0;
        c.HasGBufferAlbedo = hasAlbedo ? 1 : 0;
        c.HasGBufferRoughness = hasRoughness ? 1 : 0;
        c.DebugView = cfg->NeuralLookDebugView.value_or_default();
        c.InColorEncodeMode = inEncodeMode;
        c.OutColorEncodeMode = outEncodeMode;
    }
};
