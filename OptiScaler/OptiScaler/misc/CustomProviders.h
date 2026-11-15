#pragma once

#include "Config.h"

#include <misc/IdentifyGpu.h>
#include <plugins/PluginApi.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

// Dlls in custom/<name>/<kind> of the Opti folder, each kind a folder has is an entry in the upscaler or FG list:
//   custom/<name>/ffx      DX12 FFX upscaler
//   custom/<name>/ffx_vk   Vulkan FFX upscaler
//   custom/<name>/dlss     NGX runtime that runs DLSS
//   custom/<name>/dlssg    nvngx_dlssg replacement, an FG Nvngx entry
// The dll can have any name. When the folder has more dlls, like ones the dll loads itself, the standard name
// is used (amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_upscaler_vk.dll then amd_fidelityfx_vk.dll, nvngx.dll,
// nvngx_dlssg.dll), else the first one. A Vulkan dll with another name is taken as amd_fidelityfx_upscaler_vk.dll.
namespace CustomProviders
{
inline std::filesystem::path Root()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"custom";
}

inline std::filesystem::path Folder(const std::string& name) { return Root() / string_to_wstring(name); }

// The dll in custom/<name>/<kind>, empty when there's none
inline std::filesystem::path DllPath(const std::string& name, const wchar_t* kind,
                                     std::initializer_list<const wchar_t*> standardNames)
{
    auto folder = Folder(name) / kind;
    std::error_code error;

    for (auto standardName : standardNames)
    {
        if (std::filesystem::exists(folder / standardName, error))
            return folder / standardName;
    }

    for (auto& entry : std::filesystem::directory_iterator(folder, error))
    {
        if (_wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
            return entry.path();
    }

    return {};
}

inline std::filesystem::path FfxDllPath(const std::string& name)
{
    return DllPath(name, L"ffx", { L"amd_fidelityfx_upscaler_dx12.dll" });
}

inline std::filesystem::path FfxVkDllPath(const std::string& name)
{
    return DllPath(name, L"ffx_vk", { L"amd_fidelityfx_upscaler_vk.dll", L"amd_fidelityfx_vk.dll" });
}

// The legacy FFX SDK 1.x dll takes FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, the rest the V2 backend type
inline bool IsLegacyFfxVkDll(const std::filesystem::path& path)
{
    return _wcsicmp(path.filename().c_str(), L"amd_fidelityfx_vk.dll") == 0;
}

inline std::filesystem::path NgxDllPath(const std::string& name) { return DllPath(name, L"dlss", { L"nvngx.dll" }); }

inline std::filesystem::path DlssgDllPath(const std::string& name)
{
    return DllPath(name, L"dlssg", { L"nvngx_dlssg.dll" });
}

// nullptr when the path is empty or the dll can't be loaded
inline HMODULE LoadDll(const std::string& name, const std::filesystem::path& path)
{
    auto dll = path.empty() ? nullptr : NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, NULL);

    if (dll == nullptr)
    {
        LOG_ERROR("Can't load custom {} from {}", name, wstring_to_string(Folder(name).wstring()));
        return nullptr;
    }

    LOG_INFO("Loaded custom {} from {}", name, wstring_to_string(path.wstring()));

    // Folder name is the plugin name, the dlls themselves mostly have the standard names
    PluginApi::InitPlugin(dll, (PFN_OptiPluginInit) KernelBaseProxy::GetProcAddress_()(dll, OPTI_PLUGIN_INIT_NAME),
                          name);

    return dll;
}

struct Names
{
    std::vector<std::string> ffx;
    std::vector<std::string> ffxVk;
    std::vector<std::string> dlss;
    std::vector<std::string> dlssg;
};

// Scanned once
inline const Names& Scan()
{
    static const auto names = []
    {
        Names found;
        std::error_code error;

        for (auto& entry : std::filesystem::directory_iterator(Root(), error))
        {
            auto name = wstring_to_string(entry.path().filename().wstring());

            if (!FfxDllPath(name).empty())
                found.ffx.push_back(name);

            if (!FfxVkDllPath(name).empty())
                found.ffxVk.push_back(name);

            if (!NgxDllPath(name).empty())
                found.dlss.push_back(name);

            if (!DlssgDllPath(name).empty())
                found.dlssg.push_back(name);
        }

        return found;
    }();

    return names;
}

// Folder names aren't case sensitive
inline bool Contains(const std::vector<std::string>& names, const std::string& name)
{
    return std::ranges::any_of(names, [&name](const std::string& n) { return _stricmp(n.c_str(), name.c_str()) == 0; });
}

inline bool IsFfx(const std::string& name) { return Contains(Scan().ffx, name); }
inline bool IsFfxVk(const std::string& name) { return Contains(Scan().ffxVk, name); }
inline bool IsDlss(const std::string& name) { return Contains(Scan().dlss, name); }
inline bool IsDlssg(const std::string& name) { return Contains(Scan().dlssg, name); }

// Picks the best installed custom/ FFX (FSR 4) variant matching the active GPU's FP8/INT8 architecture
// without overriding an explicit user-pinned CustomUpscaler or non-FFX Dx12Upscaler choice.
inline void AutoSelectForGpu(const GpuInformation& gpu)
{
    auto cfg = Config::Instance();

    // Respect explicit user-pinned custom folder or explicit non-FFX upscaler
    if (cfg->CustomUpscaler.has_value())
        return;

    if (cfg->Dx12Upscaler.has_value() && cfg->Dx12Upscaler.value() != Upscaler::FFX)
        return;

    const bool ffxSelected = cfg->Dx12Upscaler.value_or_default() == Upscaler::FFX;
    if (!ffxSelected && gpu.vendorId != VendorId::AMD)
        return;

    const auto& candidates = Scan().ffx;
    if (candidates.empty())
        return;

    FSR4Support effectiveSupport =
        gpu.realFsr4Support != FSR4Support::None ? gpu.realFsr4Support : gpu.fsr4Support;
    if (effectiveSupport == FSR4Support::None)
        effectiveSupport = cfg->Fsr4GpuSupport.value_or_default();

    // Determine target tier when Fsr4GpuSupport/ASIC detection still hasn't resolved on AMD
    if (effectiveSupport == FSR4Support::None && gpu.vendorId == VendorId::AMD)
    {
        if (gpu.amdHwGeneration >= device_info::HwGeneration::kGfx12)
            effectiveSupport = FSR4Support::FP8;
        else if (gpu.amdHwGeneration == device_info::HwGeneration::kGfx11)
            effectiveSupport = FSR4Support::INT8;
        else if (gpu.amdHwGeneration == device_info::HwGeneration::kGfx10_3)
            effectiveSupport = FSR4Support::INT8_2;
    }

    if (effectiveSupport == FSR4Support::None)
        return;

    auto toLower = [](std::string s)
    {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const std::string* bestCandidate = nullptr;
    int bestScore = 0;

    for (const auto& name : candidates)
    {
        const std::string lower = toLower(name);
        const bool isFp8 = lower.find("fp8") != std::string::npos;
        const bool isInt8 = lower.find("int8") != std::string::npos;
        const bool isLite = lower.find("lite") != std::string::npos;
        const bool isLossy = lower.find("lossy") != std::string::npos;
        const bool isExact = lower.find("exact") != std::string::npos;

        int score = 0;
        if (effectiveSupport == FSR4Support::FP8)
        {
            if (isFp8)
                score += 100;
            else if (isInt8)
                score += 10;

            if (isExact)
                score += 20;
        }
        else if (effectiveSupport == FSR4Support::INT8Lite)
        {
            if (isFp8)
                score -= 100;
            else if (isInt8)
                score += 100;
            else
                score += 40;

            if (isLite)
                score += 40;
            if (isLossy)
                score += 20;
            if (isExact)
                score += 10;
        }
        else
        {
            // INT8 / INT8_2 (RDNA 3 etc.)
            if (isFp8)
                score -= 100;
            else if (isInt8)
                score += 100;
            else
                score += 40;

            if (isLossy)
                score += 30;
            else if (!isLite && !isExact)
                score += 25;
            if (isLite)
                score += 15;
            if (isExact)
                score += 10;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestCandidate = &name;
        }
    }

    if (bestCandidate != nullptr)
    {
        cfg->CustomUpscaler.set_volatile_value(*bestCandidate);
        cfg->Dx12Upscaler.set_volatile_value(Upscaler::FFX);
        LOG_INFO("Auto-selected custom FSR4 provider '{}' for GPU '{}'", *bestCandidate, gpu.name);
    }
}

// Dx12Upscaler from the ini as a custom folder's kind, a folder with both is used as FFX
inline void ResolveDx12Upscaler()
{
    auto cfg = Config::Instance();
    auto name = cfg->CustomUpscaler.value_or("");

    if (name.empty())
        return;

    if (IsFfx(name))
    {
        cfg->Dx12Upscaler.set_from_config(Upscaler::FFX);
    }
    else if (IsDlss(name))
    {
        cfg->Dx12Upscaler.set_from_config(Upscaler::DLSS);
    }
    else
    {
        LOG_WARN("Dx12Upscaler={} isn't an upscaler or a folder in {}", name, wstring_to_string(Root().wstring()));
        cfg->CustomUpscaler.reset();
    }
}

// VulkanUpscaler from the ini as a custom folder, only FFX ones run on Vulkan
inline void ResolveVulkanUpscaler()
{
    auto cfg = Config::Instance();
    auto name = cfg->CustomUpscalerVk.value_or("");

    if (name.empty())
        return;

    if (IsFfxVk(name))
    {
        cfg->VulkanUpscaler.set_from_config(Upscaler::FFX);
    }
    else
    {
        LOG_WARN("VulkanUpscaler={} isn't an upscaler or a folder with ffx_vk in {}", name,
                 wstring_to_string(Root().wstring()));
        cfg->CustomUpscalerVk.reset();
    }
}

// FGNvngxReplacement from the ini as a custom folder with dlssg
inline void ResolveFGNvngx()
{
    auto& name = State::Instance().fgNvngxCustom;

    if (name.empty())
        return;

    if (!IsDlssg(name))
    {
        LOG_WARN("FGNvngxReplacement={} isn't a replacement or a folder with dlssg in {}", name,
                 wstring_to_string(Root().wstring()));
        name.clear();
        Config::Instance()->FGNvngxReplacement.reset();
    }
}
} // namespace CustomProviders
