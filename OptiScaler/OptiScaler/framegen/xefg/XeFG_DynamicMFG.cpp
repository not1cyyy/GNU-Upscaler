#include "pch.h"
#include "XeFG_DynamicMFG.h"

#include <Config.h>

XeFG_DynamicMFG::Result XeFG_DynamicMFG::Update(int targetCount, int framesToInterpolate, bool generationOff,
                                                double presentWaitMs, HWND window)
{
    const auto* config = Config::Instance();

    FG_DynamicController::Options opt {};
    opt.enabled = config->FGXeFGDynamicMFG.value_or_default() && !config->FGFollowsGame();
    opt.useTargetAsCap = config->FGXeFGTargetAsCapDMFG.value_or_default();
    opt.allow1X = config->FGXeFGDynamicAllow1X.value_or_default();
    opt.targetCount = targetCount;
    opt.currentCount = framesToInterpolate;
    opt.generationOff = generationOff;
    opt.targetFps = config->FGXeFGFramerateTargetDMFG.value_or_default();
    opt.minBaseFps = config->FGXeFGMinBaseFps.value_or_default();
    opt.rounding = config->FGXeFGRoundingDMFG.value_or_default();
    opt.presentWaitMs = presentWaitMs;
    opt.window = window;
    opt.updateCapState = true;

    return FG_DynamicController::Update(opt);
}
