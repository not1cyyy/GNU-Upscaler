#pragma once

#include <windows.h>
#include <cstdint>
#include <algorithm>
#include "Config.h"
#include "Logger.h"

namespace XeFGUnlock
{
inline bool Apply(HMODULE libxefgModule)
{
    if (libxefgModule == nullptr)
        return false;

    auto base = reinterpret_cast<uint8_t*>(libxefgModule);

    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const uint32_t timestamp = ntHeaders->FileHeader.TimeDateStamp;
    const uint32_t ceiling = std::clamp(Config::Instance()->FGXeFGMaxInterpolatedFrames.value_or(5), 1, 9);

    LOG_INFO("XeFG Unlock: checking provider build 0x{:08X}, target ceiling: {} ({}X)", timestamp, ceiling, ceiling + 1);

    auto patch = [&](size_t offset, const uint8_t* patchBytes, size_t patchSize, const char* name) -> bool
    {
        void* target = base + offset;
        DWORD oldProtect = 0;
        if (VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(target, patchBytes, patchSize);
            VirtualProtect(target, patchSize, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), target, patchSize);
            LOG_INFO("XeFG Unlock: {} patched at 0x{:X}", name, offset);
            return true;
        }
        LOG_ERROR("XeFG Unlock: VirtualProtect failed for {} at 0x{:X}", name, offset);
        return false;
    };

    // Build 0x69CB0F4D (Intel XeFG 1.5.x shipped with Black Myth Wukong, Stellar Blade, and standard packages)
    if (timestamp == 0x69CB0F4D)
    {
        // U1: frame-count-fallback bypass (E9 CD 00 00 00 90)
        const uint8_t u1[] = { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 };
        patch(0x20DA4F, u1, sizeof(u1), "U1/frame-count-fallback");

        // U2: model-downgrade bypass (EB 06)
        const uint8_t u2[] = { 0xEB, 0x06 };
        patch(0x1A5DE4, u2, sizeof(u2), "U2/model-downgrade");

        // U3: default-ceiling (BB <ceiling> 00 00 00)
        const uint8_t u3[] = { 0xBB, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x1A517D, u3, sizeof(u3), "U3/default-ceiling");

        // U4: override-clamp (C7 87 6C 01 00 00 <ceiling> 00 00 00)
        const uint8_t u4[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x1A45C2, u4, sizeof(u4), "U4/override-clamp");

        // U5: reported-maximum (33 C9 BA <ceiling> 00 00 00)
        const uint8_t u5[] = { 0x33, 0xC9, 0xBA, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x20973B, u5, sizeof(u5), "U5/reported-maximum");

        LOG_INFO("XeFG Unlock: all 5 patches successfully applied to provider (MFG unlocked up to {}X)", ceiling + 1);
        return true;
    }
    else
    {
        // Signature-based scanning for other builds
        const size_t codeSize = ntHeaders->OptionalHeader.SizeOfCode;
        const size_t codeBase = ntHeaders->OptionalHeader.BaseOfCode;

        // U5 pattern: 33 C9 BA 01 00 00 00 33 C9 44 8B C2
        static const uint8_t u5_sig[] = { 0x33, 0xC9, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x33, 0xC9, 0x44, 0x8B, 0xC2 };
        if (codeSize > sizeof(u5_sig))
        {
            for (size_t i = 0; i < codeSize - sizeof(u5_sig); ++i)
            {
                if (memcmp(base + codeBase + i, u5_sig, sizeof(u5_sig)) == 0)
                {
                    void* target = base + codeBase + i + 3;
                    DWORD oldProtect = 0;
                    if (VirtualProtect(target, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
                    {
                        *reinterpret_cast<uint32_t*>(target) = ceiling;
                        VirtualProtect(target, 4, oldProtect, &oldProtect);
                        FlushInstructionCache(GetCurrentProcess(), target, 4);
                        LOG_INFO("XeFG Unlock: generic U5 patched at 0x{:X} to {}", codeBase + i + 3, ceiling);
                    }
                    break;
                }
            }
        }
    }

    return false;
}
} // namespace XeFGUnlock
