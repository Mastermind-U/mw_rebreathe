#include "shared.h"
#include "mmio_probe.h"
#include <cstdio>
#include <filesystem>
#include <thread>
#include <atomic>
#include <random>
#include <cerrno>
namespace fs = std::filesystem;
static decltype(&ReadFile) DirectRead;
static std::atomic<unsigned> checks{0}, errors{0};
static bool Check(bool value, const char* what) {
    ++checks;
    if (!value) { ++errors; fprintf(stderr, "FAIL %s Win32=%lu\n", what, GetLastError()); }
    return value;
}
static void Compare(const fs::path& p, uint64_t size, unsigned seed) {
    HANDLE cached = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE direct = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!Check(cached != INVALID_HANDLE_VALUE && direct != INVALID_HANDLE_VALUE, "open pair")) return;
    std::mt19937 random(seed);
    std::vector<uint8_t> a(9 * 1024 * 1024), b(a.size());
    for (unsigned i = 0; i < 24; ++i) {
        uint64_t offset = i == 0 ? 0 : i == 1 ? size : i == 2 ? size + 17 : i == 3 ? (size > 65530 ? 65530 : 0) : uint64_t(random()) % std::max<uint64_t>(size, 1);
        DWORD amount = i == 4 ? DWORD(a.size()) : DWORD(random() % 131073);
        LARGE_INTEGER pos; pos.QuadPart = offset;
        SetFilePointerEx(cached, pos, nullptr, FILE_BEGIN); SetFilePointerEx(direct, pos, nullptr, FILE_BEGIN);
        DWORD na = 0, nb = 0;
        BOOL ra = ReadFile(cached, a.data(), amount, &na, nullptr);
        BOOL rb = DirectRead(direct, b.data(), amount, &nb, nullptr);
        Check(ra == rb && na == nb && !memcmp(a.data(), b.data(), na), "cached bytes/EOF equal direct kernel read");
        LARGE_INTEGER zero{}, pa{}, pb{}; SetFilePointerEx(cached, zero, &pa, FILE_CURRENT); SetFilePointerEx(direct, zero, &pb, FILE_CURRENT);
        Check(pa.QuadPart == pb.QuadPart, "file position matches kernel");
    }
    DWORD n = 99; Check(ReadFile(cached, nullptr, 0, &n, nullptr) && n == 0, "zero-length read");
    Check(CloseHandle(cached) && CloseHandle(direct), "close pair");
}
int wmain() {
    wchar_t executable[32768]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!pathLength || pathLength >= std::size(executable)) {
        fprintf(stderr, "FAIL cannot derive probe executable path Win32=%lu\n", GetLastError());
        ExitProcess(1);
    }
    const auto bin = fs::path(executable).parent_path();
    if (bin.empty() || bin.parent_path().empty()) {
        fprintf(stderr, "FAIL probe executable has no package directory\n");
        ExitProcess(1);
    }
    const auto logs = bin.parent_path() / L"logs";
    DirectRead = reinterpret_cast<decltype(&ReadFile)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ReadFile"));
    auto root = fs::current_path(); std::vector<fs::path> files;
    for (auto name : {L"Morrowind.bsa", L"Morrowind.esm", L"Bloodmoon.bsa", L"Tribunal.bsa"}) files.push_back(root / L"Data Files" / name);
    for (auto& p : files) Compare(p, fs::file_size(p), unsigned(checks.load() + 10));
    auto big = files.front(); auto size = fs::file_size(big);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) threads.emplace_back([&, i] { Compare(big, size, 200 + i); });
    for (auto& t : threads) t.join();
    auto legacy = LoadLibraryW(L"msvcrt.dll");
    using Stats = DWORD (WINAPI*)(CacheCounters*);
    auto stats = reinterpret_cast<Stats>(GetProcAddress(GetModuleHandleW(L"modernizer.dll"), "ModernizeStats"));
    CacheCounters before{}, after{};
    Check(stats && stats(&before) == 0 && before.reads > 100 && before.bytes > 1000000, "injected cache counters prove reads");
    using Open = void* (__cdecl*)(const char*, const char*);
    using Read = size_t (__cdecl*)(void*, size_t, size_t, void*);
    using Close = int (__cdecl*)(void*);
    auto open = reinterpret_cast<Open>(GetProcAddress(legacy, "fopen"));
    auto read = reinterpret_cast<Read>(GetProcAddress(legacy, "fread"));
    auto close = reinterpret_cast<Close>(GetProcAddress(legacy, "fclose"));
    auto f = open("Data Files\\Morrowind.esm", "rb"); char magic[4]{};
    Check(f && read(magic, 1, 4, f) == 4 && !memcmp(magic, "TES3", 4), "legacy msvcrt fopen/fread"); if (f) close(f);
    if (stats) stats(&after);
    const bool legacyRam = after.bytes > before.bytes && after.opens > before.opens;
    Check(legacyRam, "legacy CRT actually uses RAM cache");
    fs::path wave;
    uintmax_t waveSize = 0;
    for (const auto& resource : fs::recursive_directory_iterator(root / L"Data Files" / L"Sound")) {
        if (resource.is_regular_file() && Lower(resource.path().extension().wstring()) == L".wav" &&
            resource.file_size() > waveSize && resource.file_size() < 16 * 1024 * 1024) {
            wave = resource.path(); waveSize = resource.file_size();
        }
    }
    const auto waveResult = MorrowindModernMmio::Probe(wave.c_str(), stats);
    const bool waveRam = waveResult.PassedBytes() && waveResult.UsedRam();
    Check(waveRam, "real WAV through WinMM/MMIO: full bytes, seek, EOF and RAM counters");
    if (stats) stats(&after);
    auto fallback = logs / L"probe-fallback.bin";
    HANDLE h = CreateFileW(fallback.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    DWORD n; const char text[] = "uncached writable handle"; WriteFile(h, text, sizeof(text), &n, nullptr); SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    char got[sizeof(text)]{}; Check(ReadFile(h, got, sizeof(got), &n, nullptr) && n == sizeof(got) && !memcmp(text, got, n), "writable non-resource fallback"); CloseHandle(h);
    n = 99; SetLastError(0); BOOL invalid = ReadFile(INVALID_HANDLE_VALUE, got, 1, &n, nullptr);
    Check(!invalid && GetLastError() == ERROR_INVALID_HANDLE, "invalid handle preserves native error");
    auto result = logs / L"probe-results.json";
    auto report = _wfopen(result.c_str(), L"wb");
    if (!report) {
        fprintf(stderr, "FAIL cannot create probe report %ls errno=%d\n", result.c_str(), errno);
        ExitProcess(1);
    }
    const int written = fprintf(report, "{\"checks\":%u,\"failures\":%u,\"cached_reads\":%llu,\"cached_bytes\":%llu,\"legacy_crt_cache_verified\":%s,\"mmio_wav_cache_verified\":%s,\"mmio_wav_bytes\":%llu,\"mmio_cached_reads\":%llu}\n",
        checks.load(), errors.load(), after.reads, after.bytes, legacyRam ? "true" : "false", waveRam ? "true" : "false", waveResult.fileBytes, waveResult.cachedReads);
    const int closed = fclose(report);
    if (written < 0 || closed != 0) {
        fprintf(stderr, "FAIL cannot write complete probe report %ls errno=%d\n", result.c_str(), errno);
        ExitProcess(1);
    }
    printf("%u checks, %u failures\n", checks.load(), errors.load());
    ExitProcess(errors ? 1 : 0);
}
