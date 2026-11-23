#pragma once

#include <d3d12.h>
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/DeferredRelease.h>

// Manages a dedicated D3D12_COMMAND_LIST_TYPE_COMPUTE queue, ring-buffered command allocators/lists,
// and a cross-queue fence so shader passes can execute on async compute hardware without blocking
// the direct graphics pipe.
class AsyncCompute_Dx12
{
  public:
    static constexpr UINT SlotCount = 4;

    explicit AsyncCompute_Dx12(ID3D12Device* device) { Init(device); }

    ~AsyncCompute_Dx12() { Release(); }

    bool IsInit() const { return _init; }

    static bool IsComputeCompatibleState(D3D12_RESOURCE_STATES state)
    {
        constexpr D3D12_RESOURCE_STATES graphicsOnlyStates =
            D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
            D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

        return (state & graphicsOnlyStates) == 0;
    }

    // Records and submits compute work on _computeQueue ordered after prior directQueue work,
    // returning the compute fence value without blocking directQueue yet so callers can overlap work.
    template <typename RecordFn>
    bool BeginDispatch(ID3D12CommandQueue* directQueue, RecordFn&& recordCompute, UINT64& outDoneVal,
                       bool forceEnable = false)
    {
        outDoneVal = 0;

        if (!_init || directQueue == nullptr ||
            (!forceEnable && !Config::Instance()->Dx12AsyncCompute.value_or_default()))
        {
            return false;
        }

        const UINT nextSlot = (_slot + 1) % SlotCount;

        if (_slotFenceValues[nextSlot] > 0 && _fence->GetCompletedValue() < _slotFenceValues[nextSlot])
        {
            if (FAILED(_fence->SetEventOnCompletion(_slotFenceValues[nextSlot], _fenceEvent)))
                return false;

            if (WaitForSingleObject(_fenceEvent, 5000) != WAIT_OBJECT_0)
                return false;
        }

        if (FAILED(_allocators[nextSlot]->Reset()))
            return false;

        if (FAILED(_cmdLists[nextSlot]->Reset(_allocators[nextSlot], nullptr)))
            return false;

        const bool recorded = recordCompute(_cmdLists[nextSlot]);
        const HRESULT closeHr = _cmdLists[nextSlot]->Close();
        if (!recorded || FAILED(closeHr))
            return false;

        const UINT64 readyVal = ++_fenceCounter;
        const UINT64 doneVal = ++_fenceCounter;

        if (FAILED(directQueue->Signal(_fence, readyVal)))
            return false;

        if (FAILED(_computeQueue->Wait(_fence, readyVal)))
            return false;

        ID3D12CommandList* lists[] = { _cmdLists[nextSlot] };
        _computeQueue->ExecuteCommandLists(1, lists);

        if (FAILED(_computeQueue->Signal(_fence, doneVal)))
            return false;

        _slotFenceValues[nextSlot] = doneVal;
        _slot = nextSlot;
        outDoneVal = doneVal;
        return true;
    }

    bool WaitOnDirectQueue(ID3D12CommandQueue* directQueue, UINT64 doneVal)
    {
        if (!_init || directQueue == nullptr || doneVal == 0)
            return false;

        return SUCCEEDED(directQueue->Wait(_fence, doneVal));
    }

    // Executes recordCompute(ID3D12GraphicsCommandList*) on the async compute queue,
    // bracketed by directQueue->Signal -> computeQueue->Wait and computeQueue->Signal -> directQueue->Wait.
    template <typename RecordFn>
    bool Dispatch(ID3D12CommandQueue* directQueue, RecordFn&& recordCompute, bool forceEnable = false)
    {
        UINT64 doneVal = 0;
        if (!BeginDispatch(directQueue, std::forward<RecordFn>(recordCompute), doneVal, forceEnable))
            return false;

        return WaitOnDirectQueue(directQueue, doneVal);
    }

    void Release()
    {
        _init = false;

        if (!State::Instance().isShuttingDown)
        {
            if (_fence != nullptr && _fenceEvent != nullptr && _fenceCounter > 0 &&
                _fence->GetCompletedValue() < _fenceCounter)
            {
                if (SUCCEEDED(_fence->SetEventOnCompletion(_fenceCounter, _fenceEvent)))
                    WaitForSingleObject(_fenceEvent, 1000);
            }

            for (UINT i = 0; i < SlotCount; ++i)
            {
                DeferredRelease::Release(_cmdLists[i]);
                DeferredRelease::Release(_allocators[i]);
                _cmdLists[i] = nullptr;
                _allocators[i] = nullptr;
                _slotFenceValues[i] = 0;
            }

            DeferredRelease::Release(_computeQueue);
            DeferredRelease::Release(_fence);
        }

        _computeQueue = nullptr;
        _fence = nullptr;

        if (_fenceEvent != nullptr)
        {
            CloseHandle(_fenceEvent);
            _fenceEvent = nullptr;
        }
    }

  private:
    bool Init(ID3D12Device* device)
    {
        if (device == nullptr)
            return false;

        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_computeQueue))))
        {
            LOG_WARN("AsyncCompute_Dx12: CreateCommandQueue(COMPUTE) failed");
            Release();
            return false;
        }
        _computeQueue->SetName(L"OptiScaler_AsyncComputeQueue");

        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
        {
            LOG_WARN("AsyncCompute_Dx12: CreateFence failed");
            Release();
            return false;
        }
        _fence->SetName(L"OptiScaler_AsyncComputeFence");

        _fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_fenceEvent == nullptr)
        {
            LOG_WARN("AsyncCompute_Dx12: CreateEvent failed");
            Release();
            return false;
        }

        for (UINT i = 0; i < SlotCount; ++i)
        {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&_allocators[i]))))
            {
                LOG_WARN("AsyncCompute_Dx12: CreateCommandAllocator[{}] failed", i);
                Release();
                return false;
            }

            if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, _allocators[i], nullptr,
                                                 IID_PPV_ARGS(&_cmdLists[i]))))
            {
                LOG_WARN("AsyncCompute_Dx12: CreateCommandList[{}] failed", i);
                Release();
                return false;
            }

            if (FAILED(_cmdLists[i]->Close()))
            {
                Release();
                return false;
            }
        }

        _init = true;
        LOG_INFO("AsyncCompute_Dx12: Initialized dedicated D3D12 compute queue");
        return true;
    }

    ID3D12CommandQueue* _computeQueue = nullptr;
    ID3D12CommandAllocator* _allocators[SlotCount] = {};
    ID3D12GraphicsCommandList* _cmdLists[SlotCount] = {};
    ID3D12Fence* _fence = nullptr;
    HANDLE _fenceEvent = nullptr;
    UINT64 _slotFenceValues[SlotCount] = {};
    UINT64 _fenceCounter = 0;
    UINT _slot = 0;
    bool _init = false;
};
