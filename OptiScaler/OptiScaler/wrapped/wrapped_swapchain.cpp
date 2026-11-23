#include "pch.h"
#include "wrapped_swapchain.h"
#include <fsr4/FSR4Models.h>

#include <Util.h>
#include <Config.h>
#include <misc/FrameStats.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/FG_Hooks.h>
#include <with_dx12/dx11_with_dx12_sync.h>

#include <menu/menu_overlay_base.h>
#include <menu/menu_overlay_dx.h>
#include <wrapped/wrapped_factory.h>

#include <misc/FrameLimit.h>
#include <misc/DeferredRelease.h>

#include <d3d11.h>
#include <d3d12.h>
#include <misc/IdentifyGpu.h>
#include <low_latency/input/input_common.h>
#include <inputs/FG/XeFG_Inputs_Dx12.h>

#include <magic_enum.hpp>

#include <low_latency/input/input_antilag2.h>

#ifdef DXGI_DEBUG_ENABLED
#include <magic_enum.hpp>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

#ifdef ENABLE_DEBUG_LAYER_DX12
#include <d3d12sdklayers.h>
#endif
#endif

#pragma intrinsic(_ReturnAddress)

// Used RenderDoc's wrapped object as referance
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/dxgi/dxgi_wrapped.cpp

static int scCount = 0;
static double _lastFrameTime = 0;
static bool _dx11Device = false;
static bool _dx12Device = false;

const GUID IID_IUnwrappedDXGISwapChain = {
    0xe8a33b4a, 0x1405, 0x424c, { 0xae, 0x88, 0xd, 0x3e, 0x9d, 0x46, 0xc9, 0x14 }
};

static ID3D12Fence* resizeFence = nullptr;
static ID3D12Device* resizeFenceDevice = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;

static void UpdateOutputColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    auto& state = State::Instance();

    OutputColorSpace info {};
    info.dxgiColorSpace = colorSpace;

    switch (colorSpace)
    {
        // ------------------------------------------------------------
        // SDR / Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // scRGB / linear Rec.709
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        info.transfer = ColorTransfer::Linear;
        info.primaries = ColorPrimaries::Rec709;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HDR10 / PQ / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::RGB;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // HLG / Rec.2020
        // ------------------------------------------------------------

    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Full;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
        info.transfer = ColorTransfer::HLG;
        info.primaries = ColorPrimaries::Rec2020;
        info.range = ColorRange::Studio;
        info.model = ColorModel::YCbCr;
        info.valid = true;

        break;

        // ------------------------------------------------------------
        // Unknown / unsupported
        // ------------------------------------------------------------

    default:
        info.transfer = ColorTransfer::Unknown;
        info.primaries = ColorPrimaries::Unknown;
        info.range = ColorRange::Unknown;
        info.model = ColorModel::Unknown;
        info.valid = false;

        break;
    }

    state.outputColorSpace = info;

    LOG_INFO("Output color space updated: DXGI: {}, Transfer: {}, Primaries: {}, Range: {}, Model: {}, Valid: {}",
             magic_enum::enum_name(info.dxgiColorSpace), magic_enum::enum_name(info.transfer),
             magic_enum::enum_name(info.primaries), magic_enum::enum_name(info.range),
             magic_enum::enum_name(info.model), info.valid);
}

// FG and the swapchain state belong to the game's swapchain, not to other windows' swapchains
static bool IsCurrentSwapchain(IDXGISwapChain* swapchain)
{
    auto& state = State::Instance();
    return state.currentSwapchain == swapchain || state.currentWrappedSwapchain == swapchain;
}

// Game's output, the FG library's swapchain on the FG window included
static bool IsGameOutput(IDXGISwapChain* swapchain, HWND hwnd)
{
    return IsCurrentSwapchain(swapchain) ||
           (State::Instance().currentFGSwapchain != nullptr && !FGHooks::IsOtherWindowThanFG(hwnd));
}

// Menu draws on the current swapchain and the last presenting window
static bool HasMenu(IDXGISwapChain* swapchain, HWND hwnd)
{
    return IsCurrentSwapchain(swapchain) || MenuOverlayBase::Handle() == hwnd;
}

static void WaitForGPUIdle(IUnknown* object)
{
    if (object == nullptr)
        return;

    // Fence state is shared by every caller
    static std::mutex waitMutex;
    std::scoped_lock lock(waitMutex);

    ID3D12CommandQueue* queue = nullptr;

    if (object->QueryInterface(IID_PPV_ARGS(&queue)) != S_OK || queue == nullptr)
        return;

    ID3D12Device* device = nullptr;

    if (queue->GetDevice(IID_PPV_ARGS(&device)) != S_OK || device == nullptr)
    {
        LOG_ERROR("Can't get device of the command queue");
        queue->Release();
        return;
    }

    if (resizeFence == nullptr || resizeFenceDevice != device)
    {
        if (resizeFence != nullptr)
        {
            resizeFence->Release();
            resizeFence = nullptr;
        }

        resizeFenceValue = 0;
        resizeFenceDevice = nullptr;

        auto result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));

        if (result != S_OK)
        {
            LOG_ERROR("CreateFence error: {:X}", (UINT) result);
            resizeFence = nullptr;
        }
        else
        {
            resizeFenceDevice = device;
        }
    }

    device->Release();

    if (resizeFenceEvent == nullptr)
        resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (resizeFence == nullptr || resizeFenceEvent == nullptr)
    {
        queue->Release();
        return;
    }

    LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

    resizeFenceValue++;
    auto signalResult = queue->Signal(resizeFence, resizeFenceValue);
    queue->Release();

    if (signalResult != S_OK)
    {
        LOG_ERROR("Signal error: {:X}", (UINT) signalResult);
        return;
    }

    const auto deadline = GetTickCount64() + 5000; // Max 5 sec

    while (resizeFence->GetCompletedValue() < resizeFenceValue)
    {
        auto now = GetTickCount64();

        if (now >= deadline)
        {
            LOG_WARN("GPU idle wait timed out, fence value: {}, completed: {}", resizeFenceValue,
                     resizeFence->GetCompletedValue());
            break;
        }

        auto eventResult = resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);

        if (eventResult != S_OK)
        {
            LOG_ERROR("SetEventOnCompletion error: {:X}", (UINT) eventResult);
            break;
        }

        auto waitResult = WaitForSingleObject(resizeFenceEvent, static_cast<DWORD>(deadline - now));
        LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
    }
}

#ifdef DXGI_DEBUG_ENABLED
void ReportDXGILiveObjects()
{
    IDXGIDebug1* dxgiDebug = nullptr;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        dxgiDebug->Release();
    }
}

void ReadDxgiInfoQueue()
{
    IDXGIInfoQueue* dxgiInfoQueue = nullptr;
    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)) == S_OK)
    {
        UINT64 msgCount = dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < msgCount; ++i)
        {
            SIZE_T msgLen = 0;
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &msgLen);
            std::vector<char> buf(msgLen);
            auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &msgLen);

            auto description = std::string(msg->pDescription, msg->DescriptionByteLength);
            LOG_DEBUG("DXGI Debug Message: Category: {}, Severity: {}, ID: {}, Description: {}",
                      magic_enum::enum_name(msg->Category), magic_enum::enum_name(msg->Severity), msg->ID, description);
        }
    }
}

#ifdef ENABLE_DEBUG_LAYER_DX12
void ReportD3D12LiveObjects(ID3D12Device* device)
{
    ID3D12DebugDevice* debugDevice = nullptr;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugDevice->Release();
    }
}
#endif
#endif

struct DxSwapchainGpuSync
{
    ComPtr<ID3D12Fence> fence12;
    ID3D12Device* fence12Device = nullptr;
    HANDLE fence12Event = nullptr;
    UINT64 fence12Seq = 0;
    uint64_t fence12SubmitNs[2] {};

    static constexpr size_t kQuerySlots = 4;
    ComPtr<ID3D11Query> queries11[kQuerySlots];
    uint64_t submitNs11[kQuerySlots] {};
    bool pending11[kQuerySlots] {};
    size_t head11 = 0;
    ID3D11Device* query11Device = nullptr;
    ComPtr<ID3D11DeviceContext> ctx11;

    ~DxSwapchainGpuSync() { Reset(); }

    void Reset()
    {
        if (fence12Event != nullptr)
        {
            CloseHandle(fence12Event);
            fence12Event = nullptr;
        }
        fence12.Reset();
        fence12Device = nullptr;
        fence12Seq = 0;
        fence12SubmitNs[0] = 0;
        fence12SubmitNs[1] = 0;

        for (size_t i = 0; i < kQuerySlots; ++i)
        {
            queries11[i].Reset();
            submitNs11[i] = 0;
            pending11[i] = false;
        }
        head11 = 0;
        ctx11.Reset();
        query11Device = nullptr;
    }

    void SyncD3D12(ID3D12CommandQueue* cq, ID3D12Device* dev, int syncDepth)
    {
        if (cq == nullptr || dev == nullptr || syncDepth < 0 || State::Instance().isShuttingDown)
            return;

        if (!fence12 || fence12Device != dev)
        {
            fence12.Reset();
            fence12Device = nullptr;
            fence12Seq = 0;
            fence12SubmitNs[0] = 0;
            fence12SubmitNs[1] = 0;
            if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12))))
            {
                fence12.Reset();
                return;
            }
            fence12Device = dev;
        }

        if (fence12Event == nullptr)
        {
            fence12Event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fence12Event == nullptr)
                return;
        }

        const UINT64 seq = ++fence12Seq;
        const uint64_t submitNs = Util::GetTimestamp();
        fence12SubmitNs[seq & 1ULL] = submitNs;
        if (FAILED(cq->Signal(fence12.Get(), seq)))
            return;

        if (syncDepth == 0)
        {
            const UINT64 completed = fence12->GetCompletedValue();
            if (completed == UINT64_MAX)
                return;

            const bool wasBusy = (completed < seq);
            if (wasBusy)
            {
                if (FAILED(fence12->SetEventOnCompletion(seq, fence12Event)))
                    return;
                WaitForSingleObject(fence12Event, 100);
            }
            const uint64_t completeNs = Util::GetTimestamp();
            FrameLimit::recordGpuCompletion(submitNs, completeNs, 0, wasBusy);
        }
        else
        {
            if (seq <= 1)
            {
                FrameLimit::recordGpuCompletion(submitNs, submitNs, 1, false);
                return;
            }

            const UINT64 waitSeq = seq - 1;
            const UINT64 completed = fence12->GetCompletedValue();
            if (completed == UINT64_MAX)
                return;

            const bool wasBusy = (completed < waitSeq);
            if (wasBusy)
            {
                if (FAILED(fence12->SetEventOnCompletion(waitSeq, fence12Event)))
                    return;
                WaitForSingleObject(fence12Event, 100);
            }
            const uint64_t completeNs = Util::GetTimestamp();
            FrameLimit::recordGpuCompletion(fence12SubmitNs[waitSeq & 1ULL], completeNs, 1, wasBusy);
        }
    }

    void SyncD3D11(ID3D11Device* dev, int syncDepth)
    {
        if (dev == nullptr || syncDepth < 0 || State::Instance().isShuttingDown)
            return;

        if (!ctx11 || query11Device != dev)
        {
            for (size_t i = 0; i < kQuerySlots; ++i)
            {
                queries11[i].Reset();
                submitNs11[i] = 0;
                pending11[i] = false;
            }
            head11 = 0;
            ctx11.Reset();
            query11Device = nullptr;

            dev->GetImmediateContext(&ctx11);
            if (!ctx11)
                return;
            query11Device = dev;
        }

        auto& curQuery = queries11[head11];
        if (!curQuery)
        {
            const D3D11_QUERY_DESC qd { D3D11_QUERY_EVENT, 0 };
            if (FAILED(dev->CreateQuery(&qd, &curQuery)))
            {
                curQuery.Reset();
                return;
            }
        }

        const size_t curIdx = head11;
        head11 = (head11 + 1) % kQuerySlots;

        const uint64_t submitNs = Util::GetTimestamp();
        ctx11->End(curQuery.Get());
        if (syncDepth == 0)
            ctx11->Flush();

        auto waitQuery = [this](ID3D11Query* q, bool& outBusy) -> bool
        {
            BOOL done = FALSE;
            HRESULT hr = ctx11->GetData(q, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            outBusy = (hr == S_FALSE) || (hr == S_OK && !done);
            if (hr == S_OK && done)
                return true;
            if (hr != S_FALSE && hr != S_OK)
                return false;

            const uint64_t startNs = Util::GetTimestamp();
            uint32_t spins = 0;
            while ((hr = ctx11->GetData(q, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE ||
                   (hr == S_OK && !done))
            {
                if (Util::GetTimestamp() - startNs > 100'000'000ULL)
                    return false;
                if (++spins > 256)
                {
                    spins = 0;
                    Sleep(0);
                }
                else
                {
                    YieldProcessor();
                }
            }
            return (hr == S_OK && done);
        };

        if (syncDepth == 0)
        {
            pending11[curIdx] = false;
            bool wasBusy = false;
            if (waitQuery(curQuery.Get(), wasBusy))
            {
                const uint64_t completeNs = Util::GetTimestamp();
                FrameLimit::recordGpuCompletion(submitNs, completeNs, 0, wasBusy);
            }
        }
        else
        {
            pending11[curIdx] = true;
            submitNs11[curIdx] = submitNs;

            const size_t prevIdx = (curIdx + kQuerySlots - 1) % kQuerySlots;
            if (pending11[prevIdx] && queries11[prevIdx])
            {
                pending11[prevIdx] = false;
                bool wasBusy = false;
                if (waitQuery(queries11[prevIdx].Get(), wasBusy))
                {
                    const uint64_t completeNs = Util::GetTimestamp();
                    FrameLimit::recordGpuCompletion(submitNs11[prevIdx], completeNs, 1, wasBusy);
                }
            }
            else
            {
                FrameLimit::recordGpuCompletion(submitNs, submitNs, 1, false);
            }
        }
    }
};

static void RecordDxgiFrameStats(IDXGISwapChain* swapChain, HWND hwnd, UINT syncInterval, uint64_t submitNs)
{
    if (swapChain == nullptr)
        return;

    DXGI_FRAME_STATISTICS stats {};
    if (SUCCEEDED(swapChain->GetFrameStatistics(&stats)) && stats.SyncQPCTime.QuadPart > 0)
    {
        static UINT lastSyncCount = 0;
        if (stats.SyncRefreshCount != lastSyncCount)
        {
            lastSyncCount = stats.SyncRefreshCount;
            const uint64_t syncNs = Util::QpcToNs(stats.SyncQPCTime.QuadPart);
            const int hz = Util::GetActiveRefreshRate(hwnd);
            const uint64_t refreshNs = hz > 0 ? (1'000'000'000ULL / static_cast<uint64_t>(hz)) : 0ULL;
            FrameLimit::recordScanoutTimestamp(submitNs, syncNs, refreshNs, 0, syncInterval > 0, false);
        }
    }
}

void WrappedIDXGISwapChain4::PresentCache::ResetDevice()
{
    if (d3d11Device != nullptr)
    {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }

    if (d3d12Device != nullptr)
    {
        d3d12Device->Release();
        d3d12Device = nullptr;
    }

    if (d3d12RawQueue != nullptr)
    {
        d3d12RawQueue->Release();
        d3d12RawQueue = nullptr;
    }

    d3d12Queue = nullptr;
    sourceDevice = nullptr;
    isD3D11 = false;
    deviceResolved = false;
    d3d12Hooked = false;
}

void WrappedIDXGISwapChain4::ApplyWaitableLatencyClamp(bool isCurrent)
{
    if (!_presentCache.isWaitable || _real2 == nullptr)
        return;

    const int maxInFlight =
        (isCurrent && State::Instance().activeFgOutput == FGOutput::NoFG) ? FrameLimit::maxInFlightPresents() : 0;
    if (maxInFlight > 0)
    {
        const UINT gameMax = _presentCache.hasGameLatency ? _presentCache.gameRequestedLatency : 3u;
        const UINT desired =
            std::min(gameMax > 0 ? gameMax : static_cast<UINT>(maxInFlight), static_cast<UINT>(maxInFlight));
        if (desired != _presentCache.appliedLatency)
        {
            if (SUCCEEDED(_real2->SetMaximumFrameLatency(desired)))
                _presentCache.appliedLatency = desired;
        }
    }
    else if (_presentCache.appliedLatency != 0)
    {
        const UINT restore = _presentCache.hasGameLatency ? std::max(1u, _presentCache.gameRequestedLatency) : 3u;
        if (SUCCEEDED(_real2->SetMaximumFrameLatency(restore)))
            _presentCache.appliedLatency = 0;
    }
}

static HRESULT LocalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                            bool isCurrent, DxSwapchainGpuSync* gpuSync, WrappedIDXGISwapChain4* ownerSC)
{
    if (State::Instance().isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return pSwapChain->Present(SyncInterval, Flags);
        else
            return ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);
    }

    // This lock will delay/prevent release of buffers while present is ongoing
    std::shared_lock<std::shared_mutex> dx11wDx12PresentLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 &&
        State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        dx11wDx12PresentLock.lock();
    }

    LOG_DEBUG("{}", State::Instance().frameCount);

    HRESULT presentResult;

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        double ftDelta = 0.0;

        // Other windows' presents would shorten the game's frame time
        if (isCurrent)
        {
            auto now = Util::MillisecondsNow();

            if (_lastFrameTime != 0)
                ftDelta = now - _lastFrameTime;

            _lastFrameTime = now;

            // FGHooks::FGPresent already records base frame times when an FG swapchain wraps this swapchain
            if (State::Instance().currentFGSwapchain == nullptr)
            {
                State::Instance().presentFrameTime = ftDelta;
                FrameStats::Push(ftDelta, now);
            }

            if (State::Instance().currentFG == nullptr)
                State::Instance().lastFGFrameTime = ftDelta;
        }

        LOG_DEBUG("SyncInterval: {}, Flags: {:X}, Frametime: {:0.3f} ms", SyncInterval, Flags, ftDelta);

        // Cache swapchain desc once per resize instead of calling DXGI GetDesc every frame
        if (ownerSC != nullptr)
        {
            auto& cache = ownerSC->GetPresentCache();
            if (!cache.hasDesc && pSwapChain->GetDesc(&cache.desc) == S_OK)
                cache.hasDesc = true;

            if (isCurrent)
            {
                if (cache.hasDesc)
                    State::Instance().currentSwapchainDesc = cache.desc;
                else
                    LOG_WARN("Can't get swapchain desc!");
            }
        }
        else if (isCurrent && pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
        {
            LOG_WARN("Can't get swapchain desc!");
        }
    }

    auto* cache = ownerSC != nullptr ? &ownerSC->GetPresentCache() : nullptr;
    if (cache != nullptr && (!cache->deviceResolved || cache->sourceDevice != pDevice))
    {
        cache->ResetDevice();
        cache->sourceDevice = pDevice;
        cache->deviceResolved = true;

        ID3D11Device* qDevice = nullptr;
        ID3D12CommandQueue* qQueue = nullptr;
        if (pDevice != nullptr && pDevice->QueryInterface(IID_PPV_ARGS(&qDevice)) == S_OK)
        {
            cache->isD3D11 = true;
            cache->d3d11Device = qDevice;

            if (!_dx11Device)
                LOG_DEBUG("D3D11Device captured");

            _dx11Device = true;
        }
        else if (pDevice != nullptr && pDevice->QueryInterface(IID_PPV_ARGS(&qQueue)) == S_OK)
        {
            cache->d3d12RawQueue = qQueue;

            if (!_dx12Device)
                LOG_DEBUG("D3D12CommandQueue captured");

            ID3D12CommandQueue* realQueue = nullptr;
            if (Util::CheckForRealObject(__FUNCTION__, qQueue, (IUnknown**) &realQueue))
                cache->d3d12Queue = realQueue;
            else
                cache->d3d12Queue = qQueue;

            ID3D12Device* qDevice12 = nullptr;
            if (cache->d3d12Queue->GetDevice(IID_PPV_ARGS(&qDevice12)) == S_OK)
            {
                cache->d3d12Device = qDevice12;

                if (!_dx12Device)
                    LOG_DEBUG("D3D12Device captured");

                _dx12Device = true;
                D3D12Hooks::HookDevice(qDevice12);
                cache->d3d12Hooked = true;
            }
        }
    }

    ID3D11Device* device = cache != nullptr ? cache->d3d11Device : nullptr;
    ID3D12Device* device12 = cache != nullptr ? cache->d3d12Device : nullptr;
    ID3D12CommandQueue* cq = cache != nullptr ? cache->d3d12Queue : nullptr;
    bool isD3D11 = cache != nullptr ? cache->isD3D11 : false;

    if (cache != nullptr)
    {
        if (isD3D11)
        {
            if (isCurrent)
            {
                State::Instance().swapchainApi = DX11;
                State::Instance().currentD3D11Device = device;
            }
        }
        else if (cq != nullptr)
        {
            if (isCurrent)
            {
                State::Instance().swapchainApi = DX12;

                if (State::Instance().currentCommandQueue == nullptr)
                    State::Instance().currentCommandQueue = cq;

                if (device12 != nullptr)
                    State::Instance().currentD3D12Device = device12;
            }

            if (device12 != nullptr && !cache->d3d12Hooked)
            {
                D3D12Hooks::HookDevice(device12);
                cache->d3d12Hooked = true;
            }
        }
    }

    auto fg = State::Instance().currentFG;
    if (willPresent && fg != nullptr)
        ReflexHooks::update(fg->IsActive(), false);
    else
        ReflexHooks::update(false, false);

    InputCommon::update();

    // Upscaler GPU time computation
    const bool needDetailedTimes = MenuOverlayBase::IsVisible();
    if (willPresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()) &&
        Util::ShouldSampleUpscalerGpuTime(needDetailedTimes))
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            // If using DX11 with DX12 interop
            if (State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12)
            {
                if (State::Instance().currentD3D11Device != nullptr &&
                    (currentFeature->Api() == API::DX11 || currentFeature->IsWithDx12()))
                {
                    ID3D11DeviceContext* context = nullptr;
                    State::Instance().currentD3D11Device->GetImmediateContext(&context);

                    if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context);
                        upscalerTimeOpt.has_value() && needDetailedTimes)
                        currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                    context->Release();
                }
            }
            // If DX12
            else if (cq != nullptr && currentFeature->Api() == API::DX12 && !currentFeature->IsWithDx12())
            {
                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(cq);
                    upscalerTimeOpt.has_value() && needDetailedTimes)
                    currentFeature->ReadDetailedGpuTimes(cq, State::Instance().detailedGpuTimes);
            }
            // If DX11
            else if (device != nullptr && (currentFeature->Api() == API::DX11 || currentFeature->IsWithDx12()))
            {
                ID3D11DeviceContext* context = nullptr;
                device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context);
                    upscalerTimeOpt.has_value() && needDetailedTimes)
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }

            if (upscalerTimeOpt.has_value())
            {
                auto upscalerTime = upscalerTimeOpt.value();
                // filter out possibly wrong measured high values
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().lastUpscalerGpuMs = upscalerTime;
                    State::Instance().frameTimeMutex.unlock();
                    FSR4Models::EvaluateGpuBudget(upscalerTime);
                }
            }
        }
    }

    // Fallback when FGPresent is not hooked for V-sync
    if (willPresent && Config::Instance()->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  Config::Instance()->ForceVsync.value(), Config::Instance()->VsyncInterval.value_or_default(),
                  State::Instance().SCAllowTearing, State::Instance().realExclusiveFullscreen);

        if (!Config::Instance()->ForceVsync.value())
        {
            SyncInterval = 0;

            // This swapchain's flags, the global ones can belong to a released or another swapchain
            bool allowTearing = false;
            if (cache != nullptr && cache->hasDesc)
            {
                allowTearing = (cache->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 && cache->desc.Windowed;
            }
            else
            {
                DXGI_SWAP_CHAIN_DESC scDesc {};
                allowTearing = pSwapChain->GetDesc(&scDesc) == S_OK &&
                               (scDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0 && scDesc.Windowed;
            }

            if (allowTearing)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            // Remove allow tearing
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    ID3D12CommandQueue* fgPresentQueue = nullptr;
    uint32_t fgFrameMultiplier = 1;

    if (willPresent)
    {
        // Tick feature to let it know if it's frozen
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            if (auto currentFg = State::Instance().currentFG; currentFg != nullptr)
                currentFeature->TickFrozenCheck(std::max(currentFg->GetInterpolatedFrameCount(), 1u));
            else
                currentFeature->TickFrozenCheck();
        }

        // Draw overlay
        MenuOverlayDx::Present(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP);

        if (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();
            InputAntiLag2::injectAl2Context(pSwapChain, fgIsActive);

            // For the Reflex output (and AntiLag 2 with XeFG), the generated and real frames' presents as DLSSG
            // reports them
            if (fgIsActive && cache != nullptr && cache->d3d12RawQueue != nullptr)
            {
                fgPresentQueue = cache->d3d12RawQueue;
                fgFrameMultiplier = fg->GetInterpolatedFrameCount() + 1;
                InputCommon::fg_output_present(fgPresentQueue, false, fgFrameMultiplier);
            }
        }
        else
        {
            // The game's DLSSG presents through here
            InputCommon::dlssg_present();
        }

        if (isCurrent)
            State::Instance().frameCount++;
    }

    if (willPresent && ownerSC != nullptr)
        ownerSC->ApplyWaitableLatencyClamp(isCurrent);

    const bool shouldLimit = willPresent && isCurrent && !State::Instance().reflexLimitsFps &&
                             State::Instance().activeFgOutput == FGOutput::NoFG && !InputCommon::can_limit_fps();
    const bool shouldGpuSync =
        willPresent && isCurrent && State::Instance().activeFgOutput == FGOutput::NoFG && gpuSync != nullptr;
    const int gpuSyncDepth = shouldGpuSync ? FrameLimit::activeGpuSyncDepth(false) : -1;

    if (willPresent && isCurrent && State::Instance().activeFgOutput == FGOutput::NoFG)
        FrameLimit::markCpuWorkEnd();

    if (gpuSyncDepth >= 0)
    {
        if (isD3D11 && device != nullptr)
            gpuSync->SyncD3D11(device, gpuSyncDepth);
        else if (!isD3D11 && cq != nullptr && device12 != nullptr)
            gpuSync->SyncD3D12(cq, device12, gpuSyncDepth);
    }

    if (shouldLimit)
        FrameLimit::prePresent(false);

    LOG_DEBUG("Calling original present");

    const uint64_t submitNs = (shouldLimit || gpuSyncDepth >= 0) ? Util::GetTimestamp() : 0ULL;

    // swapchain present
    if (pPresentParameters == nullptr)
        presentResult = pSwapChain->Present(SyncInterval, Flags);
    else
        presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

    if ((shouldLimit || gpuSyncDepth >= 0) && SUCCEEDED(presentResult))
        RecordDxgiFrameStats(pSwapChain, hWnd, SyncInterval, submitNs);

    if (shouldLimit)
        FrameLimit::postPresent(false);

    DeferredRelease::Process();

    if (fgPresentQueue != nullptr)
        InputCommon::fg_output_present(fgPresentQueue, true, fgFrameMultiplier);

    // DXGI_STATUS_OCCLUDED etc. are not errors
    if (SUCCEEDED(presentResult))
    {
        LOG_DEBUG("Original present result: {:X}", (UINT) presentResult);
    }
    else
    {
        LOG_ERROR("Original present result: {:X}", (UINT) presentResult);

        if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
        {
            if (isD3D11)
            {
                if (State::Instance().currentD3D11Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D11Device);
            }
            else if (State::Instance().currentD3D12Device != nullptr)
            {
                Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
            }
        }
    }

    return presentResult;
}

static void UpdateSCBuffers(IDXGISwapChain* real, IDXGISwapChain1* real1, UINT bufferCount)
{
    if (bufferCount == 0 && real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (real1->GetDesc1(&desc) == S_OK)
            bufferCount = desc.BufferCount;
    }

    std::vector<IUnknown*> buffers;

    for (UINT i = 0; i < bufferCount; i++)
    {
        IUnknown* buffer = nullptr;

        // Same pointer the game creates its views with
        if (real->GetBuffer(i, __uuidof(ID3D12Resource), (void**) &buffer) == S_OK)
        {
            buffers.push_back(buffer);
            buffer->Release();
        }
    }

    State::Instance().SetSCBuffers(std::move(buffers));
}

// OptiScaler FG's swapchain wraps this one, the game renders to FG's buffers which keep their format
static bool UseForceHDR()
{
    return Config::Instance()->ForceHDR.value_or_default() && State::Instance().currentFGSwapchain == nullptr;
}

// Only D3D12 views of the buffers are forced to the new format
static void ForceHDRFormat(IUnknown* device, DXGI_FORMAT& format)
{
    if (!UseForceHDR() || device == nullptr)
        return;

    ID3D12CommandQueue* cq = nullptr;
    if (device->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
        return;

    cq->Release();
    format = Config::Instance()->UseHDR10.value_or_default() ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                             : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

static void SetForcedHDRColorSpace(IDXGISwapChain3* swapchain)
{
    LOG_INFO("Force HDR on");

    if (swapchain == nullptr || Config::Instance()->SkipColorSpace.value_or_default())
        return;

    DXGI_COLOR_SPACE_TYPE hdrCS = Config::Instance()->UseHDR10.value_or_default()
                                      ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                      : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    UINT css = 0;
    auto result = swapchain->CheckColorSpaceSupport(hdrCS, &css);

    if (result != S_OK)
    {
        LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
        return;
    }

    if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
    {
        result = swapchain->SetColorSpace1(hdrCS);

        if (result != S_OK)
        {
            LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
            return;
        }
    }

    LOG_INFO("HDR color space is set");
}

WrappedIDXGISwapChain4::WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags,
                                               bool isUWP, IUnknown* owner)
    : _real(real), _owner(owner != nullptr ? owner : real), _device(pDevice), _handle(hWnd), _refcount(1), _uwp(isUWP)
{
    _id = ++scCount;
    _lastFlags = flags;
    _presentCache.isWaitable = (flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
    _gpuSync = new DxSwapchainGpuSync();

    _real->QueryInterface(IID_PPV_ARGS(&_real1));
    if (_real1 != nullptr)
        _real1->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real2));
    if (_real2 != nullptr)
        _real2->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real3));
    if (_real3 != nullptr)
        _real3->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real4));
    if (_real4 != nullptr)
        _real4->Release();

    _real->AddRef();
    auto refCount = _real->Release();

    CheckForHdrOutput();

    LOG_INFO("{} created, real: {:X}, refCount: {}", _id, (UINT64) real, refCount);
}

WrappedIDXGISwapChain4::~WrappedIDXGISwapChain4()
{
    _presentCache.ResetDevice();

    if (_gpuSync != nullptr)
    {
        delete _gpuSync;
        _gpuSync = nullptr;
    }
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain1*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain2*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain3*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain4*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(WrappedIDXGISwapChain4))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    else if (riid == __uuidof(IUnknown))
    {
        AddRef();
        *ppvObject = (IUnknown*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIObject))
    {
        AddRef();
        *ppvObject = (IDXGIObject*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIDeviceSubObject))
    {
        AddRef();
        *ppvObject = (IDXGIDeviceSubObject*) this;
        return S_OK;
    }
    else if (XeFGInputs::IsXeFGSwapChainIID(riid))
    {
        // Only checked for, never called
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::AddRef()
{
    ULONG count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        {
#ifdef USE_LOCAL_MUTEX
            OwnedLockGuard lock(_localMutex, 999);
#endif

            const bool isCurrent = IsCurrentSwapchain(this);

            // Menu's last command list can still be running
            if (HasMenu(this, _handle))
            {
                WaitForGPUIdle(_device);
                MenuOverlayDx::CleanupRenderTarget(true, _handle);
            }

            if (State::Instance().currentSwapchain == this)
                State::Instance().currentSwapchain = nullptr;

            if (State::Instance().currentWrappedSwapchain == this)
                State::Instance().currentWrappedSwapchain = nullptr;

            if (State::Instance().currentRealSwapchain == _real)
                State::Instance().currentRealSwapchain = nullptr;

            if (isCurrent && State::Instance().currentCommandQueue == _device)
                State::Instance().currentCommandQueue = nullptr;

            if (isCurrent)
                State::Instance().SetSCBuffers({});

            auto fg = State::Instance().currentFG;
            if (isCurrent && fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
            {
                fg->Deactivate();
                fg->ReleaseSwapchain(_handle);

                if (State::Instance().currentFGSwapchain != nullptr)
                    State::Instance().currentFGSwapchain = nullptr;
            }

            _presentCache.ResetDevice();

            if (_gpuSync != nullptr)
            {
                delete _gpuSync;
                _gpuSync = nullptr;
            }

            auto refCount = _owner->Release();

            // Disabled for now, cause issues with some games
            /*
            IDXGISwapChain* skSC = nullptr;
            if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
            {
                skSC->Release();
                LOG_DEBUG("Found SK swapchain, skip releasing of main swapchain");
            }
            else
            {
                // Release real swapchain, otherwise it can cause issues when re-creating swapchain with same handle
                while (refCount > 0)
                {
                    LOG_DEBUG("Waiting for real swapchain to be released, refCount: {}", refCount);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    refCount = _real->Release();
                }
            }
            */

            LOG_DEBUG("Owner released, refCount: {}", refCount);
        }

        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetParent(REFIID riid, void** ppParent)
{
    auto result = _real->GetParent(riid, ppParent);

    if (result == S_OK && Config::Instance()->DxgiFactoryWrapping.value_or_default())
        WrappedIDXGIFactory7::WrapParent(riid, ppParent, _ReturnAddress());

    return result;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(REFIID riid, void** ppDevice)
{
    return _real->GetDevice(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 4);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        auto isCurrent = IsCurrentSwapchain(this) || State::Instance().currentSwapchain == nullptr;
        if (isCurrent)
        {
            FrameLimit::markDxgiPresent();
            FrameLimit::onFgOutputPresent();
        }

        result = LocalPresent(_real, SyncInterval, Flags, nullptr, _device, _handle, _uwp, isCurrent, _gpuSync, this);
    }
    else
    {
        result = _real->Present(SyncInterval, Flags);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    auto result = _real->GetBuffer(Buffer, riid, ppSurface);
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    HRESULT result = S_OK;

    // Unlock the locked FG, currentFG can change meanwhile
    auto lockedFG = State::Instance().currentFG;
    bool ffxLock = false;

    {
        if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {

            if (lockedFG != nullptr && lockedFG->IsActive() && !lockedFG->IsPaused() &&
                lockedFG->Mutex.getOwner() != 3 && !lockedFG->Mutex.isLockedByThisThread())
            {
                LOG_TRACE("Waiting ffxMutex 7, current: {}", lockedFG->Mutex.getOwner());
                lockedFG->Mutex.lock(7);
                ffxLock = true;
                LOG_TRACE("Accuired ffxMutex: {}", lockedFG->Mutex.getOwner());
            }
            else
            {
                LOG_TRACE("Skipping ffxMutex, owner is already 3");
            }
        }

        result = _real->SetFullscreenState(Fullscreen, pTarget);

        if (result == S_OK && IsCurrentSwapchain(this))
            State::Instance().realExclusiveFullscreen = Fullscreen;

        if (FAILED(result))
            LOG_ERROR("result: {:X}", (UINT) result);
        else
            LOG_DEBUG("result: {:X}", result);
    }

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFG->Mutex.getOwner());
        lockedFG->Mutex.unlockThis(7);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return _real->GetDesc(pDesc); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                                DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_DEBUG("");

    // Owner 7 is a mode switch waiting for the window to handle this resize
    // Unlock the locked FG, currentFG can change meanwhile
    auto lockedFG = State::Instance().currentFG;
    bool ffxLock = false;
    if (lockedFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
        !(lockedFG->Mutex.getOwner() == 7 && InSendMessage()) && !lockedFG->Mutex.isLockedByThisThread())
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFG->Mutex.getOwner());
        lockedFG->Mutex.lock(3);
        ffxLock = true;
        LOG_TRACE("Accuired ffxMutex: {}", lockedFG->Mutex.getOwner());
    }

    HRESULT result;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    // Menu's last command list can still be running
    WaitForGPUIdle(_device);
    if (_gpuSync != nullptr)
        _gpuSync->Reset();

    if (IsCurrentSwapchain(this))
    {
        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
    }

    if (HasMenu(this, _handle))
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    // V-Sync override creates flip model with ALLOW_TEARING, game's resize params must follow it
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC scDesc {};
        if (_real->GetDesc(&scDesc) == S_OK)
        {
            LOG_DEBUG("Overriding flags");

            // ALLOW_TEARING can't change on resize
            SwapChainFlags = (SwapChainFlags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) |
                             (scDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);

            // Flip model needs 2 buffers, 0 keeps the current count
            if (BufferCount == 1 && (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                                     scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
            {
                BufferCount = 2;
            }
        }
    }

    if (IsCurrentSwapchain(this))
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {0}, Width: {1}, Height: {2}, NewFormat: {3}, SwapChainFlags: {4:X}", BufferCount, Width,
              Height, (UINT) NewFormat, SwapChainFlags);

    // Release swapchain backbuffers to prevent errors when resizing
    /*

    const bool outputRequiresRelease =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (outputRequiresRelease && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing backbuffers of main swapchain");
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    */

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    if (IsCurrentSwapchain(this))
        ForceHDRFormat(_device, NewFormat);

    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    // Game recreates its views before the next present, resource tracking filters them by swapchain size
    if (SUCCEEDED(result))
    {
        _presentCache.hasDesc = false;
        _presentCache.isWaitable = (_lastFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
        _presentCache.appliedLatency = 0;

        if (IsCurrentSwapchain(this) && _real->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
    }

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (SUCCEEDED(result) && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (SUCCEEDED(result) && UseForceHDR() && IsGameOutput(this, _handle))
        SetForcedHDRColorSpace(_real3);

    if (SUCCEEDED(result) && UseForceHDR() && IsCurrentSwapchain(this))
        UpdateSCBuffers(_real, _real1, BufferCount);

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFG->Mutex.getOwner());
        lockedFG->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    const HRESULT result = _real->ResizeTarget(pNewTargetParameters);
    if (SUCCEEDED(result))
        _presentCache.hasDesc = false;
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _real1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(UINT SyncInterval, UINT Flags,
                                                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (_real1 == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 5);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        auto isCurrent = IsCurrentSwapchain(this) || State::Instance().currentSwapchain == nullptr;
        if (isCurrent)
        {
            FrameLimit::markDxgiPresent();
            FrameLimit::onFgOutputPresent();
        }

        result = LocalPresent(_real1, SyncInterval, Flags, pPresentParameters, _device, _handle, _uwp, isCurrent,
                              _gpuSync, this);
    }
    else
    {
        result = _real1->Present1(SyncInterval, Flags, pPresentParameters);
    }

    return result;
}

BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(void)
{
    return _real1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(UINT MaxLatency)
{
    _presentCache.hasGameLatency = true;
    _presentCache.gameRequestedLatency = MaxLatency;

    const int maxInFlight = (State::Instance().activeFgOutput == FGOutput::NoFG && IsCurrentSwapchain(this))
                                ? FrameLimit::maxInFlightPresents()
                                : 0;
    UINT effective = MaxLatency;
    if (maxInFlight > 0)
        effective = std::min(MaxLatency > 0 ? MaxLatency : static_cast<UINT>(maxInFlight),
                             static_cast<UINT>(maxInFlight));

    const HRESULT hr = _real2->SetMaximumFrameLatency(effective);
    if (SUCCEEDED(hr))
    {
        _presentCache.isWaitable = true;
        _presentCache.appliedLatency = (maxInFlight > 0) ? effective : 0;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    HANDLE handle = _real2->GetFrameLatencyWaitableObject();
    if (handle != nullptr)
    {
        _presentCache.isWaitable = true;
        ApplyWaitableLatencyClamp(IsCurrentSwapchain(this));
    }
    FrameLimit::registerWaitableHandle(handle);
    return handle;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCurrentBackBufferIndex(void)
{
    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         UINT* pColorSpaceSupport)
{
    if (_real3 == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    auto result = _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    // On Linux/Wine/Proton, ensure PRESENT support is reported for HDR10 and scRGB color spaces
    // so that game engines (such as Unreal Engine in Stellar Blade) unlock the in-game HDR toggle.
    if (pColorSpaceSupport != nullptr &&
        (ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
         ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))
    {
        *pColorSpaceSupport |= DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
        return S_OK;
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    LOG_INFO("WrappedIDXGISwapChain4::SetColorSpace1: {}", (UINT) ColorSpace);
    HRESULT result = S_OK;

    if (_real3 != nullptr)
        result = _real3->SetColorSpace1(ColorSpace);

    LOG_INFO("Real SetColorSpace1 result: {:X}", (UINT) result);

    if (IsGameOutput(this, _handle))
    {
        UpdateOutputColorSpace(ColorSpace);
        CheckForHdrOutput();
        LOG_INFO("Output HDR Active: {}", State::Instance().hdrOutputActive);
        MenuOverlayDx::ApplyThemeStyle();
    }

    // Always return S_OK to the game engine. Under Wine/Proton/VKD3D, if the underlying swapchain
    // returns DXGI_ERROR_UNSUPPORTED, returning failure causes Unreal Engine to fatally abort
    // via check(SUCCEEDED(hr)) with a 'SwapChain->SetColorSpace1 failed' dialog.
    return S_OK;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                                 DXGI_FORMAT Format, UINT SwapChainFlags,
                                                                 const UINT* pCreationNodeMask,
                                                                 IUnknown* const* ppPresentQueue)
{
    LOG_DEBUG("");

    // Owner 7 is a mode switch waiting for the window to handle this resize
    // Unlock the locked FG, currentFG can change meanwhile
    auto lockedFG = State::Instance().currentFG;
    bool ffxLock = false;
    if (lockedFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default() &&
        !(lockedFG->Mutex.getOwner() == 7 && InSendMessage()) && !lockedFG->Mutex.isLockedByThisThread())
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", lockedFG->Mutex.getOwner());
        lockedFG->Mutex.lock(3);
        ffxLock = true;
        LOG_TRACE("Accuired ffxMutex: {}", lockedFG->Mutex.getOwner());
    }

    // Menu's last command list can still be running
    WaitForGPUIdle(_device);
    if (_gpuSync != nullptr)
        _gpuSync->Reset();

    HRESULT result = E_FAIL;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (IsCurrentSwapchain(this))
    {
        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
    }

    if (HasMenu(this, _handle))
        MenuOverlayDx::CleanupRenderTarget(true, _handle);

    // V-Sync override creates flip model with ALLOW_TEARING, game's resize params must follow it
    if (Config::Instance()->OverrideVsync.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC scDesc {};
        if (_real->GetDesc(&scDesc) == S_OK)
        {
            LOG_DEBUG("Overriding flags");

            // ALLOW_TEARING can't change on resize
            SwapChainFlags = (SwapChainFlags & ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) |
                             (scDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);

            // Flip model needs 2 buffers, 0 keeps the current count
            if (BufferCount == 1 && (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                                     scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
            {
                BufferCount = 2;
            }
        }
    }

    if (IsCurrentSwapchain(this))
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    if (IsCurrentSwapchain(this))
        ForceHDRFormat(_device, Format);

    bool skResized = false;

    // Release swapchain backbuffers to prevent errors when resizing
    const bool isUsingOptiFgFeature =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (isUsingOptiFgFeature && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            skResized = true;
            LOG_DEBUG(
                "Found SK swapchain, skip releasing backbuffersand using ResizeBuffers instead of ResizeBuffers1");

#ifdef DXGI_DEBUG_ENABLED
            ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
            ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
            else
            {
                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
        }
        /*
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }
        */

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    if (skResized)
    {
        LOG_DEBUG("Resized with ResizeBuffers, result: {:X}", (UINT) result);
    }
    else if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }

    // Present queue only changes with a successful ResizeBuffers1, SK path resizes without it
    if (SUCCEEDED(result) && !skResized && ppPresentQueue != nullptr && *ppPresentQueue != nullptr)
    {
        // Unwrapped like on creation, Release matches currentCommandQueue with it
        IUnknown* realQueue = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, *ppPresentQueue, &realQueue))
            realQueue = *ppPresentQueue;

        if (_device != realQueue)
            _presentCache.ResetDevice();

        _device = realQueue;

        if (IsCurrentSwapchain(this))
            State::Instance().currentCommandQueue = (ID3D12CommandQueue*) realQueue;
    }

    // Game recreates its views before the next present, resource tracking filters them by swapchain size
    if (SUCCEEDED(result))
    {
        _presentCache.hasDesc = false;
        _presentCache.isWaitable = (_lastFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
        _presentCache.appliedLatency = 0;

        if (IsCurrentSwapchain(this) && _real->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
    }

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (SUCCEEDED(result) && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (SUCCEEDED(result) && UseForceHDR() && IsGameOutput(this, _handle))
        SetForcedHDRColorSpace(_real3);

    if (SUCCEEDED(result) && UseForceHDR() && IsCurrentSwapchain(this))
        UpdateSCBuffers(_real, _real1, BufferCount);

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", lockedFG->Mutex.getOwner());
        lockedFG->Mutex.unlockThis(3);
    }

    CheckForHdrOutput();

    return result;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                                 void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}
