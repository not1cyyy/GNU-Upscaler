#include "pch.h"
#include "IFGFeature.h"
#include <Config.h>
#include <low_latency/input/input_common.h>
#include <shaders/reproject/Reproject.h>
#include <cmath>

int IFGFeature::GetIndex() { return (_frameCount % BUFFER_COUNT); }

int IFGFeature::GetIndexWillBeDispatched() { return (NextDispatchFrame() % BUFFER_COUNT); }

UINT64 IFGFeature::StartNewFrame()
{
    _frameCount++;

    if (_lastDispatchedFrame == 0 || (_frameCount - _lastDispatchedFrame) > 2)
    {
        LOG_WARN("Frame count jumped too much! _frameCount: {}, _lastDispatchedFrame: {}", _frameCount,
                 _lastDispatchedFrame);

        _lastDispatchedFrame = _frameCount - 1;
    }

    auto fIndex = GetIndex();
    LOG_DEBUG("_frameCount: {}, fIndex: {}", _frameCount, fIndex);

    _resourceReady[fIndex].clear();
    _waitingExecute[fIndex] = false;

    _noUi[fIndex] = true;
    _noDistortionField[fIndex] = true;
    _noHudless[fIndex] = true;

    NewFrame();

    return _frameCount;
}

bool IFGFeature::IsResourceReady(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    return _resourceReady[index].contains(type);
}

bool IFGFeature::WaitingExecution(int index)
{
    if (index < 0)
        index = GetIndex();

    return _waitingExecute[index];
}
void IFGFeature::SetExecuted(int index)
{
    if (index < 0)
        index = GetIndex();

    _waitingExecute[index] = false;
}

bool IFGFeature::IsUsingUI() { return !_noUi[GetIndex()]; }
bool IFGFeature::IsUsingUIAny()
{
    for (const auto& value : _noUi)
        if (value == false)
            return true;

    return false;
}
bool IFGFeature::IsUsingDistortionField() { return !_noDistortionField[GetIndex()]; }
bool IFGFeature::IsUsingHudless(int index)
{
    if (index < 0)
        index = GetIndex();

    return !_noHudless[index];
}

bool IFGFeature::IsUsingHudlessAny()
{
    for (const auto& value : _noHudless)
        if (value == false)
            return true;

    return false;
}

bool IFGFeature::CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    if (streamlineRiid.Data1 == 0)
    {
        auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);

        if (iidResult != S_OK)
            return false;
    }

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

int IFGFeature::GetDispatchIndex(UINT64& willDispatchFrame)
{
    LOG_DEBUG("_lastDispatchedFrame: {},  _frameCount: {}", _lastDispatchedFrame, _frameCount);

    // We are in same frame
    if (_frameCount == _lastDispatchedFrame)
        return -1;

    willDispatchFrame = NextDispatchFrame();

    _lastDispatchedFrame = willDispatchFrame;
    _lastFGFrame = State::Instance().fgLastFrame;

    return (willDispatchFrame % BUFFER_COUNT);
}

// Shared by GetDispatchIndex and GetIndexWillBeDispatched so they pick the same slot
UINT64 IFGFeature::NextDispatchFrame()
{
    auto frame = _lastDispatchedFrame + 1; // By default render next one

    auto diff = _frameCount - _lastDispatchedFrame;
    if (diff > Config::Instance()->FGAllowedFrameAhead.value_or_default() || _lastDispatchedFrame == 0)
    {
        auto index = GetIndex();

        if (HasResource(FG_ResourceType::Depth, index) || HasResource(FG_ResourceType::Velocity, index) ||
            HasResource(FG_ResourceType::UIColor, index) || HasResource(FG_ResourceType::HudlessColor, index))
        {
            frame = _frameCount; // Set dispatch frame as latest one
        }
    }

    return frame;
}

bool IFGFeature::IsActive() { return _isActive || _waitingNewFrameData; }

void IFGFeature::Disable()
{
    Deactivate();
    _waitingNewFrameData = false;
}

bool IFGFeature::IsPaused() { return _targetFrame != 0 && _targetFrame >= _frameCount; }

bool IFGFeature::IsDispatched() { return _lastDispatchedFrame == _frameCount; }

bool IFGFeature::IsLowResMV() { return !_constants.flags[FG_Flags::DisplayResolutionMVs]; }

bool IFGFeature::IsAsync() { return _constants.flags[FG_Flags::Async]; }

bool IFGFeature::IsHdr() { return _constants.flags[FG_Flags::Hdr]; }

bool IFGFeature::IsJitteredMVs() { return _constants.flags[FG_Flags::JitteredMVs]; }

bool IFGFeature::IsInvertedDepth() { return _constants.flags[FG_Flags::InvertedDepth]; }

bool IFGFeature::IsInfiniteDepth() { return _constants.flags[FG_Flags::InfiniteDepth]; }

void IFGFeature::SetFrameCount(UINT64 frameId)
{
    // Only change frame count, if it's lower than current one
    // Or higher than allowed frame ahead
    // if (frameId < _frameCount || (frameId - _frameCount) >
    // Config::Instance()->FGAllowedFrameAhead.value_or_default())
    //{
    //    LOG_DEBUG("Old: {}, New: {}", _frameCount, frameId);
    //    _frameCount = frameId;
    //}
    // else if (frameId != _frameCount)
    //{
    //    LOG_TRACE("Prevented setting frame count! Old: {}, New: {}", _frameCount, frameId);
    //}

    // XeFG needs increasing frame ids, so it's only moved forward until its context is destroyed
    if (frameId < _frameCount && State::Instance().activeFgOutput == FGOutput::XeFG && !_allowLowerFrameId)
    {
        LOG_TRACE("Prevented setting frame count! Old: {}, New: {}", _frameCount, frameId);
        return;
    }

    _allowLowerFrameId = false;
    _frameCount = frameId;
}

void IFGFeature::SetJitter(float x, float y, int index)
{
    if (index < 0)
        index = GetIndex();

    _jitterX[index] = x;
    _jitterY[index] = y;
}

void IFGFeature::SetMVScale(float x, float y, int index)
{
    if (index < 0)
        index = GetIndex();

    _mvScaleX[index] = x;
    _mvScaleY[index] = y;
}

void IFGFeature::SetCameraValues(float nearValue, float farValue, float vFov, float aspectRatio, float meterFactor,
                                 int index)
{
    if (index < 0)
        index = GetIndex();

    _cameraFar[index] = farValue;
    _cameraNear[index] = nearValue;
    _cameraVFov[index] = vFov;
    _cameraAspectRatio[index] = aspectRatio;
    _meterFactor[index] = meterFactor;
}

void IFGFeature::SetCameraData(float cameraPosition[3], float cameraUp[3], float cameraRight[3], float cameraForward[3],
                               int index)
{
    if (index < 0)
        index = GetIndex();

    std::memcpy(_cameraPosition[index], cameraPosition, 3 * sizeof(float));
    std::memcpy(_cameraUp[index], cameraUp, 3 * sizeof(float));
    std::memcpy(_cameraRight[index], cameraRight, 3 * sizeof(float));
    std::memcpy(_cameraForward[index], cameraForward, 3 * sizeof(float));
}

void IFGFeature::SetFrameTimeDelta(double delta, int index)
{
    if (index < 0)
        index = GetIndex();

    _ftDelta[index] = delta;
}

void IFGFeature::SetReset(UINT reset, int index)
{
    if (index < 0)
        index = GetIndex();

    _reset[index] = reset;
}

void IFGFeature::SetInterpolationRect(UINT64 width, UINT height, int index)
{
    if (index < 0)
        index = GetIndex();

    _interpolationWidth[index] = width;
    _interpolationHeight[index] = height;
}

void IFGFeature::GetInterpolationRect(UINT64& width, UINT& height, int index)
{
    if (index < 0)
        index = GetIndex();

    width = _interpolationWidth[index];
    height = _interpolationHeight[index];
}

void IFGFeature::SetInterpolationPos(UINT left, UINT top, int index)
{
    if (index < 0)
        index = GetIndex();

    _interpolationLeft[index] = left;
    _interpolationTop[index] = top;
}

void IFGFeature::GetInterpolationPos(UINT& left, UINT& top, int index)
{
    if (index < 0)
        index = GetIndex();

    if (_interpolationLeft[index].has_value())
        left = _interpolationLeft[index].value();
    else
        left = 0;

    if (_interpolationTop[index].has_value())
        top = _interpolationTop[index].value();
    else
        top = 0;
}

void IFGFeature::ResetCounters()
{
    _targetFrame = _frameCount;
    _hasLastCamera = false;
}

void IFGFeature::UpdateTarget()
{
    // Reduced from 10 frames to 2 frames for near-instant mode changes without noticeable pause/stutter
    _targetFrame = _frameCount + 2;
    //_lastDispatchedFrame = 0;
    LOG_DEBUG("Current frame: {} target frame: {}", _frameCount, _targetFrame);
}

UINT64 IFGFeature::FrameCount() { return _frameCount; }

UINT64 IFGFeature::LastDispatchedFrame() { return _lastDispatchedFrame; }

UINT64 IFGFeature::TargetFrame() { return _targetFrame; }

void IFGFeature::SetResourceReady(FG_ResourceType type, int index)
{
    if (index < 0)
        index = GetIndex();

    _resourceReady[index][type] = true;
    _resourceFrame[type] = _frameCount;
}

UINT IFGFeature::GetInterpolatedFrameCount() const
{
    // Dynamic MFG's 1X: no frames generated while the output keeps its count
    if (_generationOff)
        return 0;

    return _framesToInterpolate < 0 ? 1 : _framesToInterpolate;
}

int IFGFeature::GetMaxInterpolationCount() const { return _maxInterpolationCount; }

bool IFGFeature::GetDMFGSupport() const { return _supportsDMFG; }

void IFGFeature::FilloutReprojectionData(FilloutData& data, int fIndex)
{
    auto& input = InputCollection::getInstance();
    auto previousIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;

    data.diffThreshold = 0.01f;
    data.mouseDeltaSinceSim = input.readDeltaSinceSim(_frameCount);
    data.mouseDeltaSimToSim = input.readSimsDelta(_frameCount);
    data.lateLatch = Config::Instance()->ReprojectionLateLatch.value_or_default();
    data.simStartMouse = input.readCumulativeAtSim(_frameCount);
    data.screenWidth = (uint32_t) _interpolationWidth[fIndex];
    data.screenHeight = (uint32_t) _interpolationHeight[fIndex];
    data.invertedDepth = _constants.flags[FG_Flags::InvertedDepth];

    data.cameraVFov = _cameraVFov[fIndex];
    data.cameraAspectRatio = _cameraAspectRatio[fIndex];
    std::memcpy(data.cameraUp, _cameraUp[fIndex], 3 * sizeof(float));
    std::memcpy(data.cameraRight, _cameraRight[fIndex], 3 * sizeof(float));
    std::memcpy(data.cameraForward, _cameraForward[fIndex], 3 * sizeof(float));
    std::memcpy(data.cameraPrevForward, _cameraForward[previousIndex], 3 * sizeof(float));
    std::memcpy(data.cameraPosition, _cameraPosition[fIndex], 3 * sizeof(float));
    data.cameraNear = _cameraNear[fIndex];
    data.cameraFar = _cameraFar[fIndex];

    data.hasPrevCamera = _frameCount > 1;
    std::memcpy(data.cameraPrevUp, _cameraUp[previousIndex], 3 * sizeof(float));
    std::memcpy(data.cameraPrevRight, _cameraRight[previousIndex], 3 * sizeof(float));
    std::memcpy(data.cameraPrevPosition, _cameraPosition[previousIndex], 3 * sizeof(float));

    data.frameId = _frameCount;
    data.reset = _reset[fIndex] != 0;

    data.mvScaleX = _mvScaleX[fIndex];
    data.mvScaleY = _mvScaleY[fIndex];
}

bool IFGFeature::DetectCameraCut(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT)
        return false;

    const float* pos = _cameraPosition[fIndex];
    const float* fwd = _cameraForward[fIndex];

    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(pos[i]) || !std::isfinite(fwd[i]))
            return false;
    }

    const float fwdLenSq = fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2];
    if (fwdLenSq <= 1e-6f)
        return false;

    const float invLen = 1.0f / std::sqrt(fwdLenSq);
    const float nx = fwd[0] * invLen;
    const float ny = fwd[1] * invLen;
    const float nz = fwd[2] * invLen;

    bool cutDetected = false;
    if (_hasLastCamera)
    {
        const float dx = pos[0] - _lastCameraPosition[0];
        const float dy = pos[1] - _lastCameraPosition[1];
        const float dz = pos[2] - _lastCameraPosition[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Normalize position delta to meters when engine provides viewSpaceToMetersFactor
        const float meterFactor = _meterFactor[fIndex];
        const float posThreshold = (meterFactor > 0.0001f) ? (25.0f / meterFactor) : 500.0f;

        const float forwardDot =
            nx * _lastCameraForward[0] + ny * _lastCameraForward[1] + nz * _lastCameraForward[2];

        // Publish angular velocity (rad/s) for motion-adaptive DRS boost
        const double angleRad = std::acos(std::clamp(static_cast<double>(forwardDot), -1.0, 1.0));
        const double frameMs =
            std::clamp(State::Instance().lastFGFrameTime > 0.0 ? State::Instance().lastFGFrameTime : 16.6, 1.0, 200.0);
        State::Instance().cameraAngularSpeed.store(angleRad / (frameMs * 0.001), std::memory_order_relaxed);

        // Trigger optical-flow history reset on >25m teleport or >70 deg single-frame camera snap
        if (dist > posThreshold || forwardDot < 0.342f)
        {
            cutDetected = true;
            LOG_DEBUG("Auto camera-cut reset triggered (dist: {:.2f}, threshold: {:.2f}, dot: {:.3f})", dist,
                      posThreshold, forwardDot);
        }
    }

    _lastCameraPosition[0] = pos[0];
    _lastCameraPosition[1] = pos[1];
    _lastCameraPosition[2] = pos[2];
    _lastCameraForward[0] = nx;
    _lastCameraForward[1] = ny;
    _lastCameraForward[2] = nz;
    _hasLastCamera = true;

    return cutDetected;
}
