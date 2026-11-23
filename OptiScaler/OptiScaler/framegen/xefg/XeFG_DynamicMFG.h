#pragma once

#include <framegen/FG_DynamicController.h>

// XeFG-configured wrapper around the shared FG_DynamicController
class XeFG_DynamicMFG : public FG_DynamicController
{
  public:
    using Result = FG_DynamicController::Result;

    // presentWaitMs: what the game waited in the output's present path this frame
    Result Update(int targetCount, int framesToInterpolate, bool generationOff, double presentWaitMs, HWND window);
};
