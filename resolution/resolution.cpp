#include "resolution.h"

#include <cstring>
#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <climits>

namespace {
constexpr DWORD kContextRva = 0x0002A07C;
constexpr DWORD kBranchRva = 0x0002A0A4;
constexpr DWORD kAspectConstantRva = 0x00346920;
constexpr DWORD kSortCallRva = 0x00029DEB;
constexpr DWORD kMinimumImageSize = kAspectConstantRva + sizeof(float);

// Morrowind 1.6.1820: MenuVideo builds its list from renderer-supported modes,
// but excludes every mode whose height / width is not exactly 0.75.
// Retain hardware support, color-depth, minimum-size and duplicate checks.
constexpr BYTE kContext[] = {
    0x8B, 0x4E, 0x04, 0x8B, 0x16, 0x89, 0x4C, 0x24,
    0x34, 0x89, 0x5C, 0x24, 0x38, 0xDF, 0x6C, 0x24,
    0x34, 0x89, 0x54, 0x24, 0x3C, 0x89, 0x5C, 0x24,
    0x40, 0xDA, 0x74, 0x24, 0x3C, 0xD8, 0x1D, 0x20,
    0x69, 0x74, 0x00, 0xDF, 0xE0, 0xF6, 0xC4, 0x40,
};
constexpr BYTE kOriginalBranch[] = {0x0F, 0x84, 0x10, 0x01, 0x00, 0x00};
constexpr BYTE kPatchedBranch[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr BYTE kAspectConstant[] = {0x00, 0x00, 0x40, 0x3F}; // 0.75f
constexpr BYTE kSortPrefix[] = {
    0x8B, 0x54, 0x24, 0x0C, 0x8B, 0x87, 0x8C, 0x00, 0x00, 0x00,
    0x8B, 0x8F, 0x88, 0x00, 0x00, 0x00, 0x6A, 0x00, 0x52, 0x50, 0x51,
};
constexpr BYTE kOriginalSortCall[] = {0xE8, 0xA0, 0x1D, 0x00, 0x00};
constexpr BYTE kSortSuffix[] = {0x83, 0xC4, 0x10};
const char* g_status = "Resolution patch has not run.";

// MSVCP60's basic_string<char> has an allocator byte plus padding, a data
// pointer, length and capacity. It has no inline/SSO buffer. Treat rows as
// trivially copied storage while permuting them; never construct or destroy
// these objects with this DLL's modern C++ runtime.
struct LegacyModeString {
    DWORD allocator;
    const char* data;
    DWORD length, capacity;
};
static_assert(sizeof(LegacyModeString) == 16, "MSVCP60 x86 string ABI");
static_assert(std::is_trivially_copyable<LegacyModeString>::value, "raw row permutation");
struct ModeKey { UINT width = UINT_MAX, height = UINT_MAX; };

bool Readable(const void* pointer, SIZE_T length) {
    auto cursor = reinterpret_cast<ULONG_PTR>(pointer);
    if (cursor < 0x10000 || length > UINTPTR_MAX - cursor) return false;
    const auto end = cursor + length;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            (info.Protect & 0xFF) == PAGE_EXECUTE) return false;
        const auto next = reinterpret_cast<ULONG_PTR>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

bool ParseNumber(const char*& cursor, const char* end, UINT& value) {
    while (cursor < end && *cursor == ' ') ++cursor;
    const auto* begin = cursor;
    UINT number = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        if (number > (UINT_MAX - UINT(*cursor - '0')) / 10) return false;
        number = number * 10 + UINT(*cursor++ - '0');
    }
    if (cursor == begin || !number) return false;
    value = number;
    return true;
}

ModeKey ParseMode(const LegacyModeString& row) {
    ModeKey invalid;
    if (!row.length || row.length > 63 || !Readable(row.data, row.length)) return invalid;
    const char* cursor = row.data;
    const char* end = cursor + row.length;
    ModeKey key;
    if (!ParseNumber(cursor, end, key.width)) return invalid;
    while (cursor < end && *cursor == ' ') ++cursor;
    if (cursor == end || (*cursor != 'x' && *cursor != 'X')) return invalid;
    ++cursor;
    if (!ParseNumber(cursor, end, key.height)) return invalid;
    while (cursor < end && *cursor == ' ') ++cursor;
    return cursor == end ? key : invalid;
}

void __cdecl SortVideoModes(LegacyModeString* begin, LegacyModeString* end,
                            ULONG_PTR, ULONG_PTR) {
    const DWORD error = GetLastError();
    const auto firstAddress = reinterpret_cast<ULONG_PTR>(begin);
    const auto lastAddress = reinterpret_cast<ULONG_PTR>(end);
    if (lastAddress < firstAddress || (lastAddress - firstAddress) % sizeof(*begin) ||
        (lastAddress - firstAddress) / sizeof(*begin) > 65536 ||
        (begin != end && !Readable(begin, lastAddress - firstAddress))) {
        SetLastError(error);
        return;
    }
    if (begin != end) {
        std::sort(begin, end, [](const auto& left, const auto& right) {
            const auto a = ParseMode(left), b = ParseMode(right);
            return a.width < b.width || (a.width == b.width && a.height < b.height);
        });
    }
    SetLastError(error);
}

bool Fail(const char* reason) {
    g_status = reason;
    return false;
}
} // namespace

bool InstallResolutionHooks(HMODULE game) {
    if (!game) {
        return Fail("Resolution patch: no executable module.");
    }

    auto* base = reinterpret_cast<BYTE*>(game);
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        dos->e_lfanew > 0x1000) {
        return Fail("Resolution patch: invalid PE header.");
    }

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.ImageBase != 0x00400000 ||
        nt->OptionalHeader.SizeOfImage < kMinimumImageSize) {
        return Fail("Resolution patch: unsupported executable layout.");
    }

    auto* branch = base + kBranchRva;
    auto* sortCall = base + kSortCallRva;
    BYTE patchedSortCall[sizeof(kOriginalSortCall)] = {0xE8};
    const DWORD relativeSort = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(&SortVideoModes) -
                                                  reinterpret_cast<ULONG_PTR>(sortCall + 5));
    std::memcpy(patchedSortCall + 1, &relativeSort, sizeof(relativeSort));
    if (std::memcmp(base + kContextRva, kContext, sizeof(kContext)) != 0 ||
        std::memcmp(base + kAspectConstantRva, kAspectConstant,
                    sizeof(kAspectConstant)) != 0) {
        return Fail("Resolution patch: expected 1.6.1820 code was not found.");
    }

    if (std::memcmp(sortCall - sizeof(kSortPrefix), kSortPrefix, sizeof(kSortPrefix)) ||
        std::memcmp(sortCall + 5, kSortSuffix, sizeof(kSortSuffix)) ||
        (std::memcmp(sortCall, kOriginalSortCall, sizeof(kOriginalSortCall)) &&
         std::memcmp(sortCall, patchedSortCall, sizeof(patchedSortCall)))) {
        return Fail("Resolution patch: expected Video mode sorting call was not found.");
    }

    if (std::memcmp(branch, kPatchedBranch, sizeof(kPatchedBranch)) == 0 &&
        std::memcmp(sortCall, patchedSortCall, sizeof(patchedSortCall)) == 0) {
        g_status = "Resolution patch: widescreen modes and safe two-field sorting already enabled.";
        return true;
    }

    if (std::memcmp(branch, kOriginalBranch, sizeof(kOriginalBranch)) != 0 &&
        std::memcmp(branch, kPatchedBranch, sizeof(kPatchedBranch)) != 0) {
        return Fail("Resolution patch: aspect-ratio branch differs; skipped.");
    }

    DWORD originalProtection = 0;
    if (!VirtualProtect(branch, sizeof(kPatchedBranch), PAGE_EXECUTE_READWRITE,
                        &originalProtection)) {
        return Fail("Resolution patch: VirtualProtect failed.");
    }
    DWORD originalSortProtection = 0;
    if (!VirtualProtect(sortCall, sizeof(patchedSortCall), PAGE_EXECUTE_READWRITE,
                        &originalSortProtection)) {
        DWORD ignored = 0;
        VirtualProtect(branch, sizeof(kPatchedBranch), originalProtection, &ignored);
        return Fail("Resolution patch: could not write Video sorting hook.");
    }

    std::memcpy(branch, kPatchedBranch, sizeof(kPatchedBranch));
    std::memcpy(sortCall, patchedSortCall, sizeof(patchedSortCall));
    const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), branch,
                                               sizeof(kPatchedBranch));
    const BOOL sortFlushed = FlushInstructionCache(GetCurrentProcess(), sortCall,
                                                  sizeof(patchedSortCall));
    DWORD unusedProtection = 0;
    const BOOL restored = VirtualProtect(branch, sizeof(kPatchedBranch),
                                          originalProtection, &unusedProtection);
    const BOOL sortRestored = VirtualProtect(sortCall, sizeof(patchedSortCall),
                                             originalSortProtection, &unusedProtection);
    if (!flushed || !restored || !sortFlushed || !sortRestored) {
        return Fail("Resolution patch was written, but cache/protection update failed.");
    }

    g_status = "Resolution patch: driver-supported widescreen modes and safe two-field Video sorting enabled.";
    return true;
}

const char* ResolutionHookStatus() {
    return g_status;
}

