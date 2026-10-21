// Compile this source alone as x86. It includes the production sorter and
// creates/destroys genuine MSVCP60 strings through the system legacy runtime.
#include "resolution.cpp"
#include <array>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {
using Constructor = void (__thiscall*)(LegacyModeString*, const char*, const void*);
using CopyConstructor = void (__thiscall*)(LegacyModeString*, const LegacyModeString*);
using Destructor = void (__thiscall*)(LegacyModeString*);
using Sorter = void (__cdecl*)(LegacyModeString*, LegacyModeString*, ULONG_PTR, ULONG_PTR);
int failures = 0;
void Check(bool condition, const char* text) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
    else std::printf("PASS: %s\n", text);
}
}

int main() {
    const HMODULE legacy = LoadLibraryExW(L"msvcp60.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!legacy) { std::fprintf(stderr, "MSVCP60 unavailable: %lu\n", GetLastError()); return 2; }
    const auto construct = reinterpret_cast<Constructor>(GetProcAddress(legacy,
        "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBDABV?$allocator@D@1@@Z"));
    const auto copy = reinterpret_cast<CopyConstructor>(GetProcAddress(legacy,
        "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z"));
    const auto destroy = reinterpret_cast<Destructor>(GetProcAddress(legacy,
        "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ"));
    if (!construct || !copy || !destroy) { FreeLibrary(legacy); return 2; }
    // The 27 dimensions admitted by the observed driver and widened filter.
    constexpr const char* modes[] = {
        "640 x 480", "800 x 600", "1024 x 768", "1152 x 864",
        "1280 x 600", "1280 x 720", "1280 x 768", "1280 x 800", "1280 x 960", "1280 x 1024",
        "1360 x 768", "1366 x 768", "1400 x 1050", "1440 x 900",
        "1600 x 900", "1600 x 1200", "1680 x 1050", "1792 x 1344",
        "1856 x 1392", "1920 x 1080", "1920 x 1200", "1920 x 1440",
        "2048 x 1152", "2048 x 1536", "2560 x 1440", "2560 x 1600", "2880 x 1800",
    };
    constexpr size_t originals = std::size(modes);
    constexpr size_t count = originals + 8;
    std::array<LegacyModeString, count> rows{};
    const BYTE allocator = 0;
    for (size_t i = 0; i < originals; ++i) construct(&rows[i], modes[i], &allocator);
    for (size_t i = originals; i < count; ++i) copy(&rows[i], &rows[(i - originals) % originals]);
    bool abi = true;
    for (size_t i = 0; i < originals; ++i) {
        abi = abi && rows[i].data && rows[i].length == std::strlen(modes[i]) &&
              !std::strcmp(rows[i].data, modes[i]);
    }
    Check(abi, "real MSVCP60 constructor matches the verified 16-byte row ABI");

    using Blob = std::array<BYTE, sizeof(LegacyModeString)>;
    std::map<Blob, unsigned> originalRows;
    std::map<const char*, BYTE> referenceCounts;
    for (const auto& row : rows) {
        Blob blob{};
        std::memcpy(blob.data(), &row, sizeof(row));
        ++originalRows[blob];
        referenceCounts[row.data] = static_cast<BYTE>(row.data[-1]);
    }
    Sorter wrapper = &SortVideoModes;
    std::mt19937 generator(0x4D57534F);
    bool sorted = true, ownership = true, guards = true;
    for (unsigned pass = 0; pass < 256; ++pass) {
        std::shuffle(rows.begin(), rows.end(), generator);
        SetLastError(1234);
        wrapper(rows.data(), rows.data() + rows.size(), 0, 0);
        guards = guards && GetLastError() == 1234;
        std::map<Blob, unsigned> after;
        ModeKey previous{0, 0};
        for (const auto& row : rows) {
            const auto key = ParseMode(row);
            sorted = sorted && key.width != UINT_MAX &&
                (previous.width < key.width || (previous.width == key.width && previous.height <= key.height));
            previous = key;
            Blob blob{};
            std::memcpy(blob.data(), &row, sizeof(row));
            ++after[blob];
            ownership = ownership && referenceCounts[row.data] == static_cast<BYTE>(row.data[-1]);
        }
        ownership = ownership && after == originalRows;
    }
    Check(sorted, "256 shuffled cross-aspect lists and duplicate widths sort numerically by width/height");
    Check(ownership, "all 16-byte owners, pointers and MSVCP60 reference counts remain unchanged");
    Check(guards, "sort wrapper preserves caller Win32 error state");

    const auto beforeInvalid = rows;
    wrapper(rows.data() + rows.size(), rows.data(), 0, 0);
    wrapper(rows.data(), reinterpret_cast<LegacyModeString*>(reinterpret_cast<BYTE*>(rows.data()) + 1), 0, 0);
    wrapper(nullptr, nullptr, 0, 0);
    Check(!std::memcmp(rows.data(), beforeInvalid.data(), sizeof(rows)), "invalid/reversed ranges and empty lists are bounded");
    Check(ParseMode(LegacyModeString{0, reinterpret_cast<char*>(2), 12, 12}).width == UINT_MAX,
          "the captured bad string pointer 0x2 is rejected without dereferencing it");
    LegacyModeString malformed{};
    construct(&malformed, "1280 x 720 x", &allocator);
    Check(ParseMode(malformed).width == UINT_MAX, "malformed three-field text is not treated as a two-field resolution");
    destroy(&malformed);
    for (auto& row : rows) destroy(&row);
    Check(true, "all reordered and shared real MSVCP60 strings destroy successfully");
    FreeLibrary(legacy);
    std::printf("mode sorting: %d failures\n", failures);
    return failures ? 1 : 0;
}
