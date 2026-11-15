#include "pch.h"

#include "Scanner.h"
#include <cstring>
#include <cstdlib>

struct SectionRange
{
    BYTE *start, *end;
};

std::vector<SectionRange> GetExecSections(HMODULE hMod)
{
    std::vector<SectionRange> secs;

    if (hMod == nullptr)
        return secs;

    BYTE* base = reinterpret_cast<BYTE*>(hMod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return secs;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return secs;

    auto first = IMAGE_FIRST_SECTION(nt);

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        auto& s = first[i];

        // filter for executable
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
        {
            BYTE* start = base + s.VirtualAddress;
            BYTE* end = start + s.Misc.VirtualSize;
            secs.push_back({ start, end });
        }
    }

    return secs;
}

struct ParsedPattern
{
    std::vector<uint8_t> bytes;
    std::vector<bool> wildcard;
    size_t first_fixed_idx = 0;
    uint8_t first_fixed_byte = 0;
    bool has_fixed = false;
};

static ParsedPattern ParsePattern(std::string_view mask)
{
    ParsedPattern p;
    size_t i = 0;
    while (i < mask.size())
    {
        while (i < mask.size() && (mask[i] == ' ' || mask[i] == '\t' || mask[i] == '\r' || mask[i] == '\n'))
            ++i;
        if (i >= mask.size())
            break;

        if (mask[i] == '?')
        {
            p.bytes.push_back(0);
            p.wildcard.push_back(true);
            while (i < mask.size() && mask[i] == '?')
                ++i;
        }
        else
        {
            char* end = nullptr;
            unsigned long val = std::strtoul(&mask[i], &end, 16);
            p.bytes.push_back(static_cast<uint8_t>(val));
            p.wildcard.push_back(false);
            if (!p.has_fixed)
            {
                p.has_fixed = true;
                p.first_fixed_idx = p.bytes.size() - 1;
                p.first_fixed_byte = static_cast<uint8_t>(val);
            }
            while (i < mask.size() && mask[i] != ' ' && mask[i] != '\t' && mask[i] != '\r' && mask[i] != '\n')
                ++i;
        }
    }
    return p;
}

static uintptr_t FindPatternFast(uintptr_t startAddress, uintptr_t maxSize, const ParsedPattern& pat)
{
    if (pat.bytes.empty() || maxSize < pat.bytes.size())
        return NULL;

    const auto dataStart = reinterpret_cast<const uint8_t*>(startAddress);
    const size_t patLen = pat.bytes.size();
    const uint8_t* cur = dataStart;
    const uint8_t* maxSearch = dataStart + maxSize - patLen;

    if (!pat.has_fixed)
        return startAddress;

    while (cur <= maxSearch)
    {
        // memchr uses hardware SIMD (AVX2/SSE2) in CRT to skip non-matching bytes at memory bandwidth speed
        const void* hit = std::memchr(cur + pat.first_fixed_idx, pat.first_fixed_byte, (maxSearch - cur) + 1);
        if (!hit)
            break;

        const uint8_t* candidate = reinterpret_cast<const uint8_t*>(hit) - pat.first_fixed_idx;
        if (candidate > maxSearch)
            break;

        bool match = true;
        for (size_t i = 0; i < patLen; ++i)
        {
            if (!pat.wildcard[i] && candidate[i] != pat.bytes[i])
            {
                match = false;
                break;
            }
        }

        if (match)
            return reinterpret_cast<uintptr_t>(candidate);

        cur = candidate + 1;
    }

    return NULL;
}

uintptr_t scanner::GetAddressFromWholeModule(HMODULE module, const std::string_view pattern, ptrdiff_t offset,
                                             uintptr_t startAddress)
{
    if (module == nullptr)
        return NULL;

    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(module);
    auto dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    auto ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS64>(moduleBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    const uintptr_t moduleEnd = static_cast<uintptr_t>(moduleBase + ntHeaders->OptionalHeader.SizeOfImage);

    auto parsed = ParsePattern(pattern);
    if (parsed.bytes.empty())
        return NULL;

    uintptr_t address = NULL;

    if (startAddress != 0)
    {
        if (startAddress < moduleBase || startAddress > moduleEnd)
            return NULL;
        address = FindPatternFast(startAddress, moduleEnd - startAddress, parsed);
    }
    else
    {
        address = FindPatternFast(moduleBase, moduleEnd - moduleBase, parsed);
    }

    return (address != NULL) ? (address + offset) : NULL;
}

uintptr_t scanner::GetAddress(const std::wstring_view moduleName, const std::string_view pattern, ptrdiff_t offset,
                              uintptr_t startAddress)
{
    std::wstring moduleNameOwned(moduleName);
    auto module = GetModuleHandle(moduleNameOwned.c_str());
    return GetAddress(module, pattern, offset, startAddress);
}

uintptr_t scanner::GetAddress(HMODULE module, const std::string_view pattern, ptrdiff_t offset, uintptr_t startAddress)
{
    if (module == nullptr)
        return NULL;

    auto parsed = ParsePattern(pattern);
    if (parsed.bytes.empty())
        return NULL;

    uintptr_t address = NULL;
    auto sections = GetExecSections(module);

    if (startAddress != 0)
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];

            if (((uintptr_t) section->start < startAddress && (uintptr_t) section->end > startAddress))
            {
                address = FindPatternFast(startAddress, (uintptr_t) section->end - startAddress, parsed);
                if (address != NULL)
                    break;
            }
            else if ((uintptr_t) section->start > startAddress)
            {
                address = FindPatternFast((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start, parsed);
                if (address != NULL)
                    break;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < sections.size(); i++)
        {
            auto section = &sections[i];
            address = FindPatternFast((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start, parsed);
            if (address != NULL)
                break;
        }
    }

    return (address != NULL) ? (address + offset) : NULL;
}

uintptr_t scanner::GetOffsetFromInstruction(HMODULE module, const std::string_view pattern, ptrdiff_t offset)
{
    if (module == nullptr)
        return NULL;

    auto parsed = ParsePattern(pattern);
    if (parsed.bytes.empty())
        return NULL;

    uintptr_t address = NULL;
    auto sections = GetExecSections(module);

    for (size_t i = 0; i < sections.size(); i++)
    {
        auto section = &sections[i];
        address = FindPatternFast((uintptr_t) section->start, (uintptr_t) section->end - (uintptr_t) section->start, parsed);
        if (address != NULL)
            break;
    }

    if (address != NULL)
    {
        auto reloffset = *reinterpret_cast<int32_t*>(address + offset) + sizeof(int32_t);
        return (address + offset + reloffset);
    }

    return NULL;
}
