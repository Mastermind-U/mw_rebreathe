// Isolated regression reproducer: no injection and no game startup.
// Compiles the real cache adapters into this test process.
#include "../src/cache_hook.cpp"
#include <type_traits>

static_assert(std::is_same_v<decltype(&MorrowindModernMmio::ReadLegacy), decltype(&_lread)>, "_lread ABI");
static_assert(std::is_same_v<decltype(&MorrowindModernMmio::SeekLegacy), decltype(&_llseek)>, "_llseek ABI");
static_assert(std::is_same_v<decltype(&MorrowindModernMmio::CloseLegacy), decltype(&_lclose)>, "_lclose ABI");

bool InstallResolutionHooks(HMODULE) { return true; }
bool InstallExperimentalHooks(HMODULE) { return true; }
const char* ResolutionHookStatus() { return "test"; }
void FlushExperimentalTrace() {}
bool InstallDisplayHooks(HMODULE) { return true; }
const char* DisplayHookStatus() { return "test"; }
bool InstallUiScaleHooks(HMODULE) { return true; }
const char* UiScaleHookStatus() { return "test"; }
void SetDisplayLogCallback(void (*)(const char*)) {}
bool InstallTransitionTrace(HMODULE) { return true; }
void FlushTransitionTrace() {}
const char* TransitionTraceStatus() { return "test"; }
bool InstallMapTrace(HMODULE) { return true; }
void FlushMapTrace() {}
const char* MapTraceStatus() { return "test"; }
bool InstallMapTileTrace(HMODULE) { return true; }
void FlushMapTileTrace() {}
const char* MapTileTraceStatus() { return "test"; }
bool InstallAudioTrace(HMODULE) { return true; }
void FlushAudioTrace() {}
const char* AudioTraceStatus() { return "test"; }
bool InstallMusicVolumeHooks(HMODULE) { return true; }
const char* MusicVolumeHookStatus() { return "test"; }
void SetMusicVolumeLogCallback(void (*)(const char*)) {}
bool InstallMenuStyle(HMODULE) { return true; }
const char* MenuStyleStatus() { return "test"; }
void SetMenuStyleLogCallback(void (*)(const char*)) {}
bool InstallOptionsLayoutHooks(HMODULE) { return true; }
const char* OptionsLayoutHookStatus() { return "test"; }
bool InstallFillbarFontHooks(HMODULE) { return true; }
const char* FillbarFontHookStatus() { return "test"; }
bool InstallScreenshotHooks(HMODULE) { return true; }
const char* ScreenshotHookStatus() { return "test"; }
void SetScreenshotLogCallback(void (*)(const char*)) {}
bool InstallBarrierWaitHooks(HMODULE) { return true; }
const char* BarrierWaitHookStatus() { return "test"; }

int wmain() {
    InitializeCriticalSection(&g_lock);
    wchar_t directory[32768]{};
    if (!GetCurrentDirectoryW(DWORD(std::size(directory)), directory)) return 2;
    const auto prefix = std::wstring(directory) + L"\\cache-handle-test-" + std::to_wstring(GetCurrentProcessId());
    const auto firstPath = prefix + L"-A.bin", secondPath = prefix + L"-B.bin";
    const char firstBytes[] = "AAAAAAAAAAAAAAAA", secondBytes[] = "BBBBBBBBBBBBBBBB";
    for (int i = 0; i < 2; ++i) {
        const auto& path = i ? secondPath : firstPath;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        DWORD written = 0;
        if (file == INVALID_HANDLE_VALUE || !WriteFile(file, i ? secondBytes : firstBytes, 16, &written, nullptr) || written != 16) return 3;
        CloseHandle(file);
    }
    CacheHeader header{};
    CacheEntry entry{};
    header.magic = kCacheMagic; header.version = kCacheVersion; header.count = 1;
    header.totalBytes = 65536; header.payloadBytes = 16;
    entry.size = 16;
    wcscpy_s(entry.relative, L"cached-A.bin");
    HANDLE stat = CreateFileW(firstPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION identity{};
    if (stat == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(stat, &identity)) return 4;
    entry.writeTime = FileTimeValue(identity.ftLastWriteTime);
    CloseHandle(stat);
    // Make both files identical in size and timestamps. Rejecting B must rely
    // on its actual volume/file ID, not accidentally on different timestamps.
    HANDLE secondStat = CreateFileW(secondPath.c_str(), GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION secondIdentity{};
    if (secondStat == INVALID_HANDLE_VALUE ||
        !SetFileTime(secondStat, &identity.ftCreationTime, &identity.ftLastAccessTime, &identity.ftLastWriteTime) ||
        !GetFileInformationByHandle(secondStat, &secondIdentity)) return 4;
    CloseHandle(secondStat);
    if (identity.dwVolumeSerialNumber == secondIdentity.dwVolumeSerialNumber &&
        identity.nFileIndexHigh == secondIdentity.nFileIndexHigh && identity.nFileIndexLow == secondIdentity.nFileIndexLow) return 4;
    std::puts("fixture: same_size_and_timestamps=1 different_file_id=1");
    g_paths.emplace(Lower(firstPath), &entry);
    g_header = &header;
    g_data = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 65536, nullptr);
    auto* seed = static_cast<char*>(MapViewOfFile(g_data, FILE_MAP_WRITE, 0, 0, 65536));
    if (!seed) return 5;
    std::memcpy(seed, firstBytes, 16);
    UnmapViewOfFile(seed);
    using NativeClose = BOOL (WINAPI*)(HANDLE);
    auto realClose = reinterpret_cast<NativeClose>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CloseHandle"));
    const char* names[] = {"hooked-noncached-open", "hooked-other-creation", "unhooked-open", "hooked-write-only"};
    bool complete = true, incorrect = false;
    for (unsigned scenario = 0; scenario < std::size(names); ++scenario) {
        bool reused = false;
        for (unsigned attempt = 0; attempt < 256 && !reused; ++attempt) {
            HANDLE first = CacheCreateFileW(firstPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (first == INVALID_HANDLE_VALUE || !g_handles.count(first)) return 6;
            char baseline[16]{}; DWORD baselineCount = 0;
            const auto baselineHits = g_hits;
            if (!CacheReadFile(first, baseline, 16, &baselineCount, nullptr) || baselineCount != 16 ||
                std::memcmp(baseline, firstBytes, 16) || g_hits != baselineHits + 1) return 8;
            realClose(first); // An unhooked module closes the tracked resource.
            HANDLE second;
            if (scenario == 2) second = CreateFileW(secondPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            else second = CacheCreateFileW(secondPath.c_str(), scenario == 3 ? GENERIC_WRITE : GENERIC_READ,
                                          FILE_SHARE_READ, nullptr, scenario == 1 ? OPEN_ALWAYS : OPEN_EXISTING, 0, nullptr);
            if (second == INVALID_HANDLE_VALUE) return 7;
            reused = first == second;
            if (reused) {
                char actual[16]{}; DWORD received = 0;
                const auto before = g_hits, staleBefore = g_staleHandles;
                const BOOL success = CacheReadFile(second, actual, 16, &received, nullptr);
                const bool mismatch = scenario == 3 ? success || GetLastError() != ERROR_ACCESS_DENIED :
                    !success || received != 16 || std::memcmp(actual, secondBytes, 16) != 0;
                incorrect |= mismatch;
                std::printf("%s reused=%d cache_hit=%d stale_read_rejected=%d success=%d bytes=%lu actual=%c correct=%d\n",
                            names[scenario], reused, g_hits != before, g_staleHandles != staleBefore, success, received,
                            actual[0] ? actual[0] : '-', !mismatch);
            }
            CacheCloseHandle(second);
            g_handles.clear();
        }
        complete &= reused;
    }
    for (auto& window : g_windows) if (window.view) UnmapViewOfFile(window.view);
    CloseHandle(g_data); g_data = nullptr;
    g_paths.clear(); g_header = nullptr;
    DeleteFileW(firstPath.c_str()); DeleteFileW(secondPath.c_str());
    std::printf("stale_handles=%llu\n", g_staleHandles);
    std::puts(!complete ? "INCONCLUSIVE: Windows did not recycle the file handle" : incorrect ? "REPRODUCED: stale cache identity returned bytes of another file" : "PASS: all recycled handles returned the correct data/error; legacy MMIO callback ABI matches Windows SDK");
    return !complete ? 2 : incorrect ? 1 : 0;
}
