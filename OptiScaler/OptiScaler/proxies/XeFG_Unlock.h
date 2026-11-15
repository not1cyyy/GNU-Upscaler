#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include "Config.h"
#include "Logger.h"

namespace XeFGUnlock
{
// Scans an in-memory buffer for a byte pattern with wildcard '?' masking
inline uint8_t* FindPattern(uint8_t* start, size_t size, const uint8_t* pattern, const char* mask)
{
    const size_t patternLen = strlen(mask);
    if (size < patternLen)
        return nullptr;

    for (size_t i = 0; i <= size - patternLen; ++i)
    {
        bool found = true;
        for (size_t j = 0; j < patternLen; ++j)
        {
            if (mask[j] == 'x' && start[i + j] != pattern[j])
            {
                found = false;
                break;
            }
        }
        if (found)
            return start + i;
    }
    return nullptr;
}

inline bool Apply(HMODULE libxefgModule)
{
    if (libxefgModule == nullptr)
        return false;

    static HMODULE s_lastPatchedModule = nullptr;
    if (s_lastPatchedModule == libxefgModule)
        return true;

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

    // Locate the .text (or executable) section for dynamic pattern searching
    uint8_t* textStart = nullptr;
    size_t textSize = 0;

    auto section = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (memcmp(section->Name, ".text", 5) == 0 || (section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
        {
            textStart = base + section->VirtualAddress;
            textSize = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
            break;
        }
    }

    if (!textStart || textSize == 0)
    {
        textStart = base;
        textSize = ntHeaders->OptionalHeader.SizeOfImage;
    }

    auto patch = [&](void* target, const uint8_t* expected, const uint8_t* patchBytes, size_t size, const char* name) -> bool
    {
        if (target == nullptr)
            return false;

        const size_t rva = reinterpret_cast<uint8_t*>(target) - base;

        // Verify original bytes before modifying memory if expected bytes are provided
        if (expected != nullptr && memcmp(target, expected, size) != 0)
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

    // Resolves patch site: first tries fixed RVA fast-path; falls back to AOB pattern scanning across .text section.
    auto resolveAndPatch = [&](size_t fallbackRva,
                               const uint8_t* expected,
                               const uint8_t* patchBytes,
                               size_t patchSize,
                               const uint8_t* sigPattern,
                               const char* sigMask,
                               size_t sigTargetOffset,
                               const char* name) -> bool
    {
        // 1. Try fast-path fixed RVA if within module image bounds
        if (fallbackRva + patchSize <= ntHeaders->OptionalHeader.SizeOfImage)
        {
            void* directTarget = base + fallbackRva;
            if (expected == nullptr || memcmp(directTarget, expected, patchSize) == 0)
            {
                return patch(directTarget, expected, patchBytes, patchSize, name);
            }
        }

        // 2. Dynamic AOB Pattern Fallback (resilient against new DLL versions and Wine/Proton relocations)
        if (sigPattern != nullptr && sigMask != nullptr && textStart != nullptr && textSize > 0)
        {
            uint8_t* searchPtr = textStart;
            size_t remaining = textSize;
            while (remaining > 0)
            {
                uint8_t* match = FindPattern(searchPtr, remaining, sigPattern, sigMask);
                if (match == nullptr)
                    break;

                void* dynamicTarget = match + sigTargetOffset;
                if (expected == nullptr || memcmp(dynamicTarget, expected, patchSize) == 0)
                {
                    const size_t foundRva = reinterpret_cast<uint8_t*>(dynamicTarget) - base;
                    LOG_INFO("XeFG Unlock: {} located via AOB pattern scan at 0x{:X}", name, foundRva);
                    return patch(dynamicTarget, expected, patchBytes, patchSize, name);
                }

                const size_t advance = static_cast<size_t>((match + 1) - searchPtr);
                if (advance >= remaining)
                    break;
                searchPtr += advance;
                remaining -= advance;
            }
        }

        LOG_WARN("XeFG Unlock: {} could not be located via RVA or AOB signature scan", name);
        return false;
    };

    int successCount = 0;

    // U1: frame-count-fallback bypass (0F 85 CC 00 00 00 -> E9 CD 00 00 00 90)
    // Sig: 84 C0 0F 85 ?? ?? ?? ?? 83 FB 01 (target is at +2)
    {
        const uint8_t u1_exp[] = { 0x0F, 0x85, 0xCC, 0x00, 0x00, 0x00 };
        const uint8_t u1_pat[] = { 0xE9, 0xCD, 0x00, 0x00, 0x00, 0x90 };
        const uint8_t u1_sig[] = { 0x84, 0xC0, 0x0F, 0x85, 0x00, 0x00, 0x00, 0x00, 0x83, 0xFB, 0x01 };
        if (resolveAndPatch(0x20DA4F, u1_exp, u1_pat, sizeof(u1_exp), u1_sig, "xxxx????xxx", 2, "U1/frame-count-fallback"))
            successCount++;
    }

    // U2: model-downgrade bypass (74 09 -> EB 06)
    // Sig: 80 79 4D 00 74 09 80 79 64 00 74 03 B0 01 C3 (target is at +4)
    {
        const uint8_t u2_exp[] = { 0x74, 0x09 };
        const uint8_t u2_pat[] = { 0xEB, 0x06 };
        const uint8_t u2_sig[] = { 0x80, 0x79, 0x4D, 0x00, 0x74, 0x09, 0x80, 0x79, 0x64, 0x00, 0x74, 0x03, 0xB0, 0x01, 0xC3 };
        if (resolveAndPatch(0x1A5DE4, u2_exp, u2_pat, sizeof(u2_exp), u2_sig, "xxxx?xxxx?xxxxx", 4, "U2/model-downgrade"))
            successCount++;
    }

    // U3: default-ceiling (BB 03 00 00 00 -> BB <ceiling> 00 00 00)
    // Sig: 48 89 5C 24 08 57 48 83 EC 20 48 8B F9 BB 03 00 00 00 (target is at +13)
    {
        const uint8_t u3_exp[] = { 0xBB, 0x03, 0x00, 0x00, 0x00 };
        const uint8_t u3_pat[] = { 0xBB, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        const uint8_t u3_sig[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xBB, 0x03, 0x00, 0x00, 0x00 };
        if (resolveAndPatch(0x1A517D, u3_exp, u3_pat, sizeof(u3_exp), u3_sig, "xxxxxxxxxxxxxx?xxx", 13, "U3/default-ceiling"))
            successCount++;
    }

    // U4: override-clamp (C7 87 6C 01 00 00 01 00 00 00 -> C7 87 6C 01 00 00 <ceiling> 00 00 00)
    // Sig: 84 C0 75 11 C7 87 6C 01 00 00 01 00 00 00 (target is at +4)
    {
        const uint8_t u4_exp[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
        const uint8_t u4_pat[] = { 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        const uint8_t u4_sig[] = { 0x84, 0xC0, 0x75, 0x11, 0xC7, 0x87, 0x6C, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
        if (resolveAndPatch(0x1A45C2, u4_exp, u4_pat, sizeof(u4_exp), u4_sig, "xxx?xxxxxx?xxx", 4, "U4/override-clamp"))
            successCount++;
    }

    // U5: reported-maximum (B8 01 00 00 00 -> B8 <ceiling> 00 00 00)
    // Sig: B8 01 00 00 00 89 47 20 33 C0 (target is at +0)
    {
        const uint8_t u5_exp[] = { 0xB8, 0x01, 0x00, 0x00, 0x00 };
        const uint8_t u5_pat[] = { 0xB8, static_cast<uint8_t>(ceiling), 0x00, 0x00, 0x00 };
        const uint8_t u5_sig[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0x89, 0x47, 0x20, 0x33, 0xC0 };
        if (resolveAndPatch(0x20973B, u5_exp, u5_pat, sizeof(u5_exp), u5_sig, "x?xxxxxxxx", 0, "U5/reported-maximum"))
            successCount++;
    }

    if (successCount > 0)
        s_lastPatchedModule = libxefgModule;

    LOG_INFO("XeFG Unlock: {}/5 patches applied (MFG unlocked up to {}X, ceiling: {})", successCount, ceiling + 1, ceiling);
    return successCount == 5;
}
} // namespace XeFGUnlock
