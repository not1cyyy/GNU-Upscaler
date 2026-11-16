#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

// Rolling present-to-present frame time history (last ~30 s, at most 4096 frames) for the average fps and
// 1% / 0.1% lows shown in the menu. Lows are 1000 / mean frame time of the slowest 1% (0.1%) of frames.
class FrameStats
{
  public:
    struct Summary
    {
        float avgFps = 0.0f;
        float low1Fps = 0.0f;
        float low01Fps = 0.0f;
        size_t samples = 0;
    };

    static void Push(double frameMs, double nowMs)
    {
        // Ignore the first-frame zero and loading-screen style stalls
        if (frameMs <= 0.0 || frameMs > 1000.0)
            return;

        std::lock_guard<std::mutex> lock(Mutex());
        auto& s = Data();
        s.ms[s.head] = static_cast<float>(frameMs);
        s.at[s.head] = nowMs;
        s.head = (s.head + 1) % Capacity;
        if (s.count < Capacity)
            ++s.count;
        s.lastNow = nowMs;
    }

    static Summary Compute()
    {
        std::vector<float> v;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            auto& s = Data();
            v.reserve(s.count);
            for (size_t i = 0; i < s.count; ++i)
            {
                const size_t idx = (s.head + Capacity - 1 - i) % Capacity;
                if (s.lastNow - s.at[idx] > WindowMs)
                    break;
                v.push_back(s.ms[idx]);
            }
        }

        Summary out;
        if (v.size() < 30)
            return out;

        std::sort(v.begin(), v.end());

        double sum = 0.0;
        for (float f : v)
            sum += f;

        out.samples = v.size();
        out.avgFps = static_cast<float>(1000.0 / (sum / static_cast<double>(v.size())));
        out.low1Fps = 1000.0f / MeanOfWorst(v, 0.01);
        out.low01Fps = 1000.0f / MeanOfWorst(v, 0.001);
        return out;
    }

    static void Reset()
    {
        std::lock_guard<std::mutex> lock(Mutex());
        auto& s = Data();
        s.head = 0;
        s.count = 0;
    }

  private:
    static constexpr size_t Capacity = 4096;
    static constexpr double WindowMs = 30000.0;

    // Input sorted ascending
    static float MeanOfWorst(const std::vector<float>& sorted, double fraction)
    {
        const size_t n = std::max<size_t>(1, static_cast<size_t>(static_cast<double>(sorted.size()) * fraction));
        double sum = 0.0;
        for (size_t i = sorted.size() - n; i < sorted.size(); ++i)
            sum += sorted[i];
        return static_cast<float>(sum / static_cast<double>(n));
    }

    struct Storage
    {
        std::array<float, Capacity> ms {};
        std::array<double, Capacity> at {};
        size_t head = 0;
        size_t count = 0;
        double lastNow = 0.0;
    };

    static Storage& Data()
    {
        static Storage s;
        return s;
    }

    static std::mutex& Mutex()
    {
        static std::mutex m;
        return m;
    }
};
