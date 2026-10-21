#pragma once
#include "iat.h"
#include <mmsystem.h>
#include <cstdio>
#include <cstring>

// MMIO uses WinMMBase's legacy file APIs on current x86 Windows. Keep the
// intervention to these two multimedia modules and pass through the existing
// resource whitelist, metadata checks, bounded RAM views and fallback behavior.
namespace MorrowindModernMmio {
using ReadFunction = BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using OpenFunction = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using SeekFunction = DWORD (WINAPI*)(HANDLE, LONG, PLONG, DWORD);
using CloseFunction = BOOL (WINAPI*)(HANDLE);
using StatsFunction = DWORD (WINAPI*)(CacheCounters*);
struct Bindings { ReadFunction read; OpenFunction open; SeekFunction seek; CloseFunction close; };
inline Bindings bindings = {};

inline UINT WINAPI ReadLegacy(HFILE file, LPVOID buffer, UINT count) {
    DWORD read = 0;
    return bindings.read(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(file)), buffer, count, &read, nullptr)
        ? read : static_cast<UINT>(HFILE_ERROR);
}
inline LONG WINAPI SeekLegacy(HFILE file, LONG offset, int origin) {
    return static_cast<LONG>(bindings.seek(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(file)), offset, nullptr, static_cast<DWORD>(origin)));
}
inline HFILE WINAPI CloseLegacy(HFILE file) {
    return bindings.close(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(file))) ? 0 : HFILE_ERROR;
}

struct Installation { unsigned modules = 0, slots = 0; bool canRead = false; };
inline Installation Install(Bindings functions) {
    Installation result;
    if (!functions.read || !functions.open || !functions.seek || !functions.close) return result;
    bindings = functions;
    for (const wchar_t* name : {L"winmm.dll", L"winmmbase.dll"}) {
        // Resolve only the operating system's x86 multimedia libraries.
        HMODULE module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) continue;
        ++result.modules;
        result.slots += PatchImport(module, "CreateFileW", reinterpret_cast<void*>(functions.open));
        result.slots += PatchImport(module, "CloseHandle", reinterpret_cast<void*>(functions.close));
        const bool nativeRead = PatchImport(module, "ReadFile", reinterpret_cast<void*>(functions.read));
        const bool legacyRead = PatchImport(module, "_lread", reinterpret_cast<void*>(&ReadLegacy));
        result.slots += nativeRead; result.slots += legacyRead;
        result.canRead |= nativeRead || legacyRead;
        result.slots += PatchImport(module, "SetFilePointer", reinterpret_cast<void*>(functions.seek));
        result.slots += PatchImport(module, "_llseek", reinterpret_cast<void*>(&SeekLegacy));
        result.slots += PatchImport(module, "_lclose", reinterpret_cast<void*>(&CloseLegacy));
        // The hooks remain installed until process exit; keep the owning DLLs loaded.
    }
    return result;
}

struct ProbeResult {
    bool opened = false, sameBytes = false, sameSeekBytes = false, eof = false, statsAvailable = false;
    uint64_t fileBytes = 0, cachedReads = 0, cachedBytes = 0, cachedOpens = 0;
    bool PassedBytes() const { return opened && sameBytes && sameSeekBytes && eof; }
    bool UsedRam() const { return statsAvailable && cachedReads > 0 && cachedBytes >= fileBytes && cachedOpens > 0; }
};

inline ProbeResult Probe(const wchar_t* path, StatsFunction stats) {
    ProbeResult result;
    auto realRead = reinterpret_cast<ReadFunction>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadFile"));
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size = {};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return result;
    }
    std::vector<unsigned char> expected(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool expectedOk = realRead && realRead(file, expected.data(), static_cast<DWORD>(expected.size()), &read, nullptr) && read == expected.size();
    CloseHandle(file);
    if (!expectedOk) return result;
    result.fileBytes = expected.size();
    char ansi[32768] = {};
    BOOL replacement = FALSE;
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path, -1, ansi, 32768, nullptr, &replacement) || replacement) return result;
    HMODULE winmm = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!winmm) return result;
    using MmioOpen = HMMIO (WINAPI*)(LPSTR, LPMMIOINFO, DWORD);
    using MmioRead = LONG (WINAPI*)(HMMIO, HPSTR, LONG);
    using MmioSeek = LONG (WINAPI*)(HMMIO, LONG, int);
    using MmioClose = MMRESULT (WINAPI*)(HMMIO, UINT);
    auto open = reinterpret_cast<MmioOpen>(GetProcAddress(winmm, "mmioOpenA"));
    auto mmread = reinterpret_cast<MmioRead>(GetProcAddress(winmm, "mmioRead"));
    auto seek = reinterpret_cast<MmioSeek>(GetProcAddress(winmm, "mmioSeek"));
    auto close = reinterpret_cast<MmioClose>(GetProcAddress(winmm, "mmioClose"));
    if (!open || !mmread || !seek || !close) return result;
    CacheCounters before = {}, after = {};
    result.statsAvailable = stats && stats(&before) == 0;
    HMMIO stream = open(ansi, nullptr, MMIO_READ | MMIO_ALLOCBUF);
    result.opened = stream != nullptr;
    if (!stream) return result;
    std::vector<unsigned char> actual(expected.size());
    size_t offset = 0;
    while (offset < actual.size()) {
        const LONG count = static_cast<LONG>((std::min)(actual.size() - offset, size_t(4093)));
        const LONG received = mmread(stream, reinterpret_cast<HPSTR>(actual.data() + offset), count);
        if (received <= 0 || received > count) break;
        offset += received;
    }
    result.sameBytes = offset == expected.size() && std::memcmp(actual.data(), expected.data(), expected.size()) == 0;
    unsigned char selected[31] = {};
    const LONG selectedCount = static_cast<LONG>((std::min)(sizeof(selected), expected.size() > 7 ? expected.size() - 7 : size_t(0)));
    result.sameSeekBytes = expected.size() > 7 && seek(stream, 7, SEEK_SET) == 7 &&
        mmread(stream, reinterpret_cast<HPSTR>(selected), selectedCount) == selectedCount &&
        std::memcmp(selected, expected.data() + 7, selectedCount) == 0;
    char end = 0;
    result.eof = seek(stream, static_cast<LONG>(expected.size()), SEEK_SET) == static_cast<LONG>(expected.size()) && mmread(stream, &end, 1) == 0;
    result.eof = close(stream, 0) == MMSYSERR_NOERROR && result.eof;
    if (result.statsAvailable && stats(&after) == 0) {
        result.cachedReads = after.reads - before.reads;
        result.cachedBytes = after.bytes - before.bytes;
        result.cachedOpens = after.opens - before.opens;
    } else result.statsAvailable = false;
    return result;
}
}

#ifdef MW_MODERN_MMIO_SELF_TEST
#include <filesystem>
int wmain() {
    namespace fs = std::filesystem;
    using namespace MorrowindModernMmio;
    auto stats = reinterpret_cast<StatsFunction>(GetProcAddress(GetModuleHandleW(L"modernizer.dll"), "ModernizeStats"));
    if (!stats) { std::puts("MMIO test requires cache injection (--probe in isolated package)."); return 2; }
    fs::path chosen;
    uintmax_t chosenSize = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fs::current_path() / L"Data Files" / L"Sound")) {
        if (entry.is_regular_file() && Lower(entry.path().extension().wstring()) == L".wav" &&
            entry.file_size() > chosenSize && entry.file_size() < 16 * 1024 * 1024) {
            chosen = entry.path(); chosenSize = entry.file_size();
        }
    }
    if (chosen.empty()) return 3;
    auto baseline = Probe(chosen.c_str(), stats);
    // The injected executable's IAT already points to the real cache adapters.
    // Taking addresses of these dllimport functions retrieves those adapter targets.
    auto installed = Install({&ReadFile, &CreateFileW, &SetFilePointer, &CloseHandle});
    auto cached = Probe(chosen.c_str(), stats);
    const bool passed = baseline.PassedBytes() && cached.PassedBytes() && cached.UsedRam() && installed.canRead;
    std::printf("WAV bytes=%llu baseline(cache_reads=%llu bytes=%llu opens=%llu) whitelisted(cache_reads=%llu bytes=%llu opens=%llu) modules=%u slots=%u result=%s\n",
                cached.fileBytes, baseline.cachedReads, baseline.cachedBytes, baseline.cachedOpens,
                cached.cachedReads, cached.cachedBytes, cached.cachedOpens, installed.modules, installed.slots, passed ? "passed" : "FAILED");
    auto reportPath = Environment(L"MW_MODERN_LOG") + L".mmio.json";
    FILE* report = nullptr;
    _wfopen_s(&report, reportPath.c_str(), L"wb");
    if (report) {
        std::fprintf(report, "{\"passed\":%s,\"wav_bytes\":%llu,\"baseline_reads\":%llu,\"baseline_bytes\":%llu,\"cached_reads\":%llu,\"cached_bytes\":%llu,\"cached_opens\":%llu,\"module_count\":%u,\"patched_slots\":%u}\n",
                     passed ? "true" : "false", cached.fileBytes, baseline.cachedReads, baseline.cachedBytes,
                     cached.cachedReads, cached.cachedBytes, cached.cachedOpens, installed.modules, installed.slots);
        std::fclose(report);
    }
    return passed ? 0 : 1;
}
#endif
