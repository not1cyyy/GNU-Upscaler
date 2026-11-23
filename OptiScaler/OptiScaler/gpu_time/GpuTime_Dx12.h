#pragma once
#include "SysUtils.h"
#include <d3d12.h>

class GpuTime_Dx12
{
    static constexpr int QUERY_BUFFER_COUNT = 3;

    ID3D12QueryHeap* _queryHeap = nullptr;
    ID3D12Resource* _readbackBuffer = nullptr;
    UINT64* _mappedTimestamps = nullptr;
    ID3D12CommandQueue* _cachedQueue = nullptr;
    UINT64 _cachedFrequency = 0;
    std::array<bool, QUERY_BUFFER_COUNT> _trigger {};

    int _currentFrameIndex = 0;
    bool _init = false;
    bool _recording = false;

  public:
    GpuTime_Dx12(ID3D12Device* device);
    ~GpuTime_Dx12();

    void Start(ID3D12GraphicsCommandList* cmdList);
    void End(ID3D12GraphicsCommandList* cmdList);

    std::optional<double> ReadGpuTime(ID3D12CommandQueue* commandQueue);
};

class ScopedGpuTime_Dx12
{
    GpuTime_Dx12* _gpuTime;
    ID3D12GraphicsCommandList* _cmdList;

  public:
    ScopedGpuTime_Dx12(GpuTime_Dx12* gpuTime, ID3D12GraphicsCommandList* cmdList) : _gpuTime(gpuTime), _cmdList(cmdList)
    {
        if (_gpuTime && _cmdList)
            _gpuTime->Start(_cmdList);
    }

    ~ScopedGpuTime_Dx12()
    {
        if (_gpuTime && _cmdList)
            _gpuTime->End(_cmdList);
    }
};
