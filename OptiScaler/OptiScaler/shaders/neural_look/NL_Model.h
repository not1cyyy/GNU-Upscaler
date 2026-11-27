#pragma once

#include "NL_DefaultModel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace NLModel
{
inline constexpr uint32_t kMagic = 0x4D4C4E47u; // "GNLM"
inline constexpr uint32_t kVersion = 3u;
inline constexpr uint32_t kFloat4Count = kNLDefaultModelFloat4Count; // 1841
inline constexpr uint32_t kFloatCount = kFloat4Count * 4u;          // 7364

struct alignas(4) Header
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t numFloat4s = 0;
    uint32_t crc32 = 0;
    char name[32] {};
    uint32_t reserved[4] {};
};

static_assert(sizeof(Header) == 64, "NLModel::Header must be exactly 64 bytes");

inline uint32_t ComputeCrc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
        {
            const uint32_t mask = static_cast<uint32_t>(-static_cast<int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline void LoadDefaultWeights(std::vector<float>& outWeights)
{
    outWeights.assign(kNLDefaultModelWeights.begin(), kNLDefaultModelWeights.end());
}

inline bool LoadModelFile(const std::filesystem::path& filePath, std::vector<float>& outWeights, std::string* outName = nullptr)
{
    std::ifstream f(filePath, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return false;

    const auto fileSize = static_cast<size_t>(f.tellg());
    constexpr size_t kExpectedBytes = sizeof(Header) + kFloatCount * sizeof(float);
    if (fileSize != kExpectedBytes)
        return false;

    f.seekg(0, std::ios::beg);
    Header hdr {};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(Header));
    if (!f || hdr.magic != kMagic || hdr.version != kVersion || hdr.numFloat4s != kFloat4Count)
        return false;

    std::vector<float> weights(kFloatCount);
    f.read(reinterpret_cast<char*>(weights.data()), kFloatCount * sizeof(float));
    if (!f)
        return false;

    const uint32_t actualCrc = ComputeCrc32(reinterpret_cast<const uint8_t*>(weights.data()), kFloatCount * sizeof(float));
    if (actualCrc != hdr.crc32)
        return false;

    for (float v : weights)
    {
        if (!std::isfinite(v))
            return false;
    }

    outWeights = std::move(weights);
    if (outName)
    {
        hdr.name[31] = '\0';
        *outName = std::string(hdr.name);
    }
    return true;
}

inline std::vector<std::pair<std::string, std::filesystem::path>> DiscoverModelPacks(const std::filesystem::path& dllDir)
{
    std::vector<std::pair<std::string, std::filesystem::path>> packs;
    packs.emplace_back("default", std::filesystem::path {});

    if (dllDir.empty())
        return packs;

    std::error_code ec;
    const std::filesystem::path searchDirs[2] = { dllDir / "neural_look", dllDir / "models" };

    for (const auto& dir : searchDirs)
    {
        if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec))
            continue;

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;

            const auto& p = entry.path();
            if (p.extension() == ".nlmodel")
            {
                const std::string stem = p.stem().string();
                auto it = std::find_if(packs.begin(), packs.end(), [&](const auto& kv) { return kv.first == stem; });
                if (it == packs.end())
                    packs.emplace_back(stem, p);
            }
        }
    }

    std::sort(packs.begin() + 1, packs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return packs;
}

inline void ResolveModelWeights(const std::string& packName, const std::filesystem::path& dllDir,
                                std::vector<float>& outWeights)
{
    if (!packName.empty() && packName != "default")
    {
        const auto packs = DiscoverModelPacks(dllDir);
        for (const auto& [name, path] : packs)
        {
            if (name == packName && !path.empty() && LoadModelFile(path, outWeights))
                return;
        }
    }

    LoadDefaultWeights(outWeights);
}
} // namespace NLModel
