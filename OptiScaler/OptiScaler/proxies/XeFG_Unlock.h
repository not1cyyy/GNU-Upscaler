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

    LOG_INFO("XeFG Unlock: checking provider build 0x{:08X}, ceiling: {} ({}X)", timestamp, ceiling, ceiling + 1);

    auto patch = [&](size_t rva, const uint8_t* expected, const uint8_t* patchBytes, size_t size, const char* name) -> bool
    {
        void* target = base + rva;

        // Verify the original bytes before touching memory
        if (memcmp(target, expected, size) != 0)
        {
            LOG_WARN("XeFG Unlock: {} at 0x{:X} does not match expected bytes, skipping", name, rva);
            return false;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(target, patchBytes, size);
            VirtualProtect(target, size, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), target, size);
            LOG_INFO("XeFG Unlock: {} successfully patched at 0x{:X}", name, rva);
            return true;
        }

        LOG_ERROR("XeFG Unlock: VirtualProtect failed for {} at 0x{:X}", name, rva);
        return false;
    };

    // Build 0x69CB0F4D (Intel XeFG 1.5.x shipped with Black Myth Wukong, Stellar Blade, and standard game packages)
    if (timestamp == 0x69CB0F4D)
    {
        // U1: frame-count-fallback bypass (0F 85 CC 00 00 00 -> E9 CD 00 00 00 90)
        const uint8_t u1_exp[] = { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 };
        const uint8_t u1_pat[] = { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 };
        patch(0x20DA4F, u1_exp, u1_pat, sizeof(u1_exp), "U1/frame-count-fallback");

        // U2: model-downgrade bypass (74 09 -> EB 06)
        const uint8_t u2_exp[] = { 0x74, 0x09 };
        const uint8_t u2_pat[] = { 0xEB, 0x06 };
        patch(0x1A5DE4, u2_exp, u2_pat, sizeof(u2_exp), "U2/model-downgrade");

        // U3: default-ceiling (BB 03 00 00 00 -> BB <ceiling> 00 00 00)
        const uint8_t u3_exp[] = { 0xBB, 0x03, 0x00, 0x00, 0x00 };
        const uint8_t u3_pat[] = { 0xBB, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x1A517D, u3_exp, u3_pat, sizeof(u3_exp), "U3/default-ceiling");

        // U4: override-clamp (C7 87 6C 01 00 00 01 00 00 00 -> C7 87 6C 01 00 00 <ceiling> 00 00 00)
        const uint8_t u4_exp[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
        const uint8_t u4_pat[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x1A45C2, u4_exp, u4_pat, sizeof(u4_exp), "U4/override-clamp");

        // U5: reported-maximum (B8 01 00 00 00 -> B8 <ceiling> 00 00 00)
        const uint8_t u5_exp[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
        const uint8_t u5_pat[] = { 0xB8, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        patch(0x20973B, u5_exp, u5_pat, sizeof(u5_exp), "U5/reported-maximum");

        LOG_INFO("XeFG Unlock: all 5 patches successfully applied to provider (MFG unlocked up to {}X)", ceiling + 1);
        return true;
    }

    return false;
}
} // namespace XeFGUnlock
