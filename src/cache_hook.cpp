#include "shared.h"
#include "iat.h"
#include "mmio_probe.h"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cstdio>

// Imported functions in this DLL stay untouched. Only the game's import tables
// and its legacy CRT/video decoder are redirected, avoiding recursive logging.
static CRITICAL_SECTION g_lock;
static HANDLE g_data = nullptr, g_manifest = nullptr;
static const CacheHeader* g_header = nullptr;
static std::unordered_map<std::wstring, const CacheEntry*> g_paths;
struct CachedHandle {
    const CacheEntry* entry = nullptr;
    BY_HANDLE_FILE_INFORMATION identity{};
};
static std::unordered_map<HANDLE, CachedHandle> g_handles;
static std::unordered_set<const CacheEntry*> g_fontReads;
static uint64_t g_hits = 0, g_bytes = 0, g_fallback = 0, g_opens = 0;
static uint64_t g_staleHandles = 0;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static std::wstring g_root;
static size_t g_granularity = 65536;
struct Window { uint64_t offset = UINT64_MAX; size_t length = 0; void* view = nullptr; uint64_t used = 0; };
static Window g_windows[4];
static uint64_t g_clock = 0;
static wchar_t g_dumpPath[32768]{};
static volatile LONG g_firstCrash = 0;
// The DLL's CRT destroys C++ maps before earlier-loaded WinMMBase detaches.
// Its IAT callbacks must become pure Win32 pass-through before that happens.
static volatile LONG g_stopping = 0;
static HANDLE g_crashMapping = nullptr, g_crashReady = nullptr, g_crashAcknowledgement = nullptr;
static CrashRequest* g_crashRequest = nullptr;
static bool Stopping() { return InterlockedCompareExchange(&g_stopping, 0, 0) != 0; }
struct Guard {
    bool locked = true;
    Guard() { EnterCriticalSection(&g_lock); }
    void Release() { if (locked) { locked = false; LeaveCriticalSection(&g_lock); } }
    ~Guard() { Release(); }
};

extern bool InstallResolutionHooks(HMODULE);
extern const char* ResolutionHookStatus();
extern bool InstallDisplayHooks(HMODULE);
extern const char* DisplayHookStatus();
extern bool InstallUiScaleHooks(HMODULE);
extern const char* UiScaleHookStatus();
extern void SetDisplayLogCallback(void (*)(const char*));
extern bool InstallAudioTrace(HMODULE);
extern void FlushAudioTrace();
extern const char* AudioTraceStatus();
extern bool InstallMusicVolumeHooks(HMODULE);
extern const char* MusicVolumeHookStatus();
extern void SetMusicVolumeLogCallback(void (*)(const char*));
extern bool InstallMenuStyle(HMODULE);
extern const char* MenuStyleStatus();
extern void SetMenuStyleLogCallback(void (*)(const char*));
extern bool InstallOptionsLayoutHooks(HMODULE);
extern const char* OptionsLayoutHookStatus();
extern bool InstallFillbarFontHooks(HMODULE);
extern const char* FillbarFontHookStatus();
extern bool InstallScreenshotHooks(HMODULE);
extern const char* ScreenshotHookStatus();
extern void SetScreenshotLogCallback(void (*)(const char*));

static void Log(const char* message) {
    if (g_log != INVALID_HANDLE_VALUE) { DWORD n; WriteFile(g_log, message, DWORD(strlen(message)), &n, nullptr); FlushFileBuffers(g_log); }
}
static void LogLine(const char* message) { Log(message); Log("\r\n"); }
static void Stats() {
    char b[256];
    sprintf_s(b, "cache reads=%llu bytes=%llu cached_opens=%llu fallback_reads=%llu stale_handles=%llu\r\n", g_hits, g_bytes, g_opens, g_fallback, g_staleHandles);
    Log(b);
}
static void OpenCrashBroker(const std::wstring& session) {
    g_crashMapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (session + L"_crash").c_str());
    if (g_crashMapping) g_crashRequest = static_cast<CrashRequest*>(MapViewOfFile(g_crashMapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CrashRequest)));
    g_crashReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, (session + L"_crash_ready").c_str());
    g_crashAcknowledgement = OpenEventW(SYNCHRONIZE, FALSE, (session + L"_crash_ack").c_str());
    if (g_crashRequest && g_crashRequest->magic == kCrashMagic && g_crashRequest->version == kCrashVersion &&
        g_crashReady && g_crashAcknowledgement) return;
    if (g_crashRequest) UnmapViewOfFile(g_crashRequest);
    if (g_crashMapping) CloseHandle(g_crashMapping);
    if (g_crashReady) CloseHandle(g_crashReady);
    if (g_crashAcknowledgement) CloseHandle(g_crashAcknowledgement);
    g_crashRequest = nullptr; g_crashMapping = g_crashReady = g_crashAcknowledgement = nullptr;
}
static char* CrashHex(char* cursor, DWORD value) {
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned shift = 28;; shift -= 4) {
        *cursor++ = digits[(value >> shift) & 15];
        if (!shift) return cursor;
    }
}
static void RawFirstException(EXCEPTION_POINTERS* exception) {
    // Last-resort diagnostic only. Never run DbgHelp or the C/C++ runtime on
    // a possibly damaged target heap. The path was allocated before gameplay.
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD header[]{kCrashMagic, kCrashVersion, GetCurrentProcessId(), GetCurrentThreadId(),
        sizeof(EXCEPTION_RECORD), sizeof(CONTEXT)};
    DWORD written = 0;
    WriteFile(file, header, sizeof(header), &written, nullptr);
    WriteFile(file, exception->ExceptionRecord, sizeof(EXCEPTION_RECORD), &written, nullptr);
    WriteFile(file, exception->ContextRecord, sizeof(CONTEXT), &written, nullptr);
    CloseHandle(file);
}
static LONG WINAPI CrashReport(EXCEPTION_POINTERS* exception) {
    // POD guard protects both first-dump ownership and recursive exceptions.
    if (InterlockedCompareExchange(&g_firstCrash, 1, 0)) return EXCEPTION_EXECUTE_HANDLER;
    char message[] = "UNHANDLED exception=00000000 address=00000000 EIP=00000000 ESP=00000000\r\n";
    CrashHex(message + 20, exception->ExceptionRecord->ExceptionCode);
    CrashHex(message + 37, DWORD(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress)));
    CrashHex(message + 50, exception->ContextRecord->Eip);
    CrashHex(message + 63, exception->ContextRecord->Esp);
    DWORD written = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, message, sizeof(message) - 1, &written, nullptr);
    bool saved = false;
    if (g_crashRequest && g_crashReady && g_crashAcknowledgement) {
        g_crashRequest->processId = GetCurrentProcessId();
        g_crashRequest->threadId = GetCurrentThreadId();
        g_crashRequest->exceptionPointers = uint32_t(reinterpret_cast<uintptr_t>(exception));
        g_crashRequest->exceptionCode = exception->ExceptionRecord->ExceptionCode;
        g_crashRequest->exceptionAddress = uint32_t(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress));
        g_crashRequest->instructionPointer = exception->ContextRecord->Eip;
        g_crashRequest->stackPointer = exception->ContextRecord->Esp;
        InterlockedExchange(&g_crashRequest->state, 2);
        if (SetEvent(g_crashReady) && WaitForSingleObject(g_crashAcknowledgement, kCrashWaitMs) == WAIT_OBJECT_0)
            saved = InterlockedCompareExchange(&g_crashRequest->dumpSucceeded, 1, 1) == 1;
    }
    if (!saved) RawFirstException(exception);
    return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD WINAPI PeriodicStats(void*) {
    uint64_t lastReads = UINT64_MAX, lastFallbacks = UINT64_MAX;
    for (;;) {
        Sleep(2000);
        if (Stopping()) return 0;
        Guard guard;
        if (Stopping()) return 0;
        if (lastReads != g_hits || lastFallbacks != g_fallback) {
            Stats(); lastReads = g_hits; lastFallbacks = g_fallback;
        }
    }
}
static bool SameFile(const BY_HANDLE_FILE_INFORMATION& first, const BY_HANDLE_FILE_INFORMATION& second) {
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
        first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow &&
        first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow &&
        FileTimeValue(first.ftCreationTime) == FileTimeValue(second.ftCreationTime) &&
        FileTimeValue(first.ftLastWriteTime) == FileTimeValue(second.ftLastWriteTime);
}
static void Track(HANDLE h, DWORD access, DWORD flags, DWORD creation) {
    if (Stopping() || h == INVALID_HANDLE_VALUE) return;
    Guard guard;
    // Handle values can be recycled after a close in an unhooked module.
    // Every intercepted successful open invalidates the previous association,
    // including opens that are ineligible for caching.
    g_staleHandles += g_handles.erase(h);
    if (creation != OPEN_EXISTING || (access & (GENERIC_WRITE | DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA)) ||
        !(access & GENERIC_READ) || (flags & (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE))) return;
    wchar_t b[32768];
    DWORD n = GetFinalPathNameByHandleW(h, b, DWORD(std::size(b)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= std::size(b)) return;
    std::wstring p = b;
    if (p.rfind(L"\\\\?\\", 0) == 0) p.erase(0, 4);
    p = Lower(p);
    auto found = g_paths.find(p);
    if (found == g_paths.end()) return;
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) return;
    auto entry = found->second;
    if (((uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != entry->size ||
        FileTimeValue(info.ftLastWriteTime) != entry->writeTime) return;
    g_handles.emplace(h, CachedHandle{entry, info});
    ++g_opens;
}
static HANDLE WINAPI CacheCreateFileA(LPCSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (Stopping()) return CreateFileA(p, a, s, sa, c, f, t);
    HANDLE h = CreateFileA(p, a, s, sa, c, f, t); DWORD error = GetLastError();
    Track(h, a, f, c);
    SetLastError(error); return h;
}
static HANDLE WINAPI CacheCreateFileW(LPCWSTR p, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    if (Stopping()) return CreateFileW(p, a, s, sa, c, f, t);
    HANDLE h = CreateFileW(p, a, s, sa, c, f, t); DWORD error = GetLastError();
    Track(h, a, f, c);
    SetLastError(error); return h;
}
static const uint8_t* Map(uint64_t offset, size_t length) {
    if (Stopping() || !length) return nullptr;
    for (auto& w : g_windows) {
        if (w.view && offset >= w.offset && offset - w.offset <= w.length && length <= w.length - size_t(offset - w.offset)) {
            w.used = ++g_clock; return static_cast<uint8_t*>(w.view) + size_t(offset - w.offset);
        }
    }
    auto w = &g_windows[0];
    for (auto& x : g_windows) if (x.used < w->used) w = &x;
    if (w->view) UnmapViewOfFile(w->view);
    w->view = nullptr; w->length = 0;
    uint64_t aligned = offset - offset % g_granularity;
    size_t need = size_t(offset - aligned) + length;
    size_t span = size_t(std::min<uint64_t>(std::max<size_t>(need, 8 * 1024 * 1024), g_header->totalBytes - aligned));
    w->view = MapViewOfFile(g_data, FILE_MAP_READ, DWORD(aligned >> 32), DWORD(aligned), span);
    if (!w->view) return nullptr;
    w->offset = aligned; w->length = span; w->used = ++g_clock;
    return static_cast<uint8_t*>(w->view) + size_t(offset - aligned);
}
static BOOL WINAPI CacheReadFile(HANDLE h, LPVOID buffer, DWORD count, LPDWORD read, LPOVERLAPPED overlapped) {
    if (Stopping()) return ReadFile(h, buffer, count, read, overlapped);
    DWORD before = GetLastError();
    Guard guard;
    auto it = g_handles.find(h);
    if (it == g_handles.end() || overlapped || !read || (!buffer && count)) {
        ++g_fallback; guard.Release(); SetLastError(before); return ReadFile(h, buffer, count, read, overlapped);
    }
    *read = 0;
    BY_HANDLE_FILE_INFORMATION identity{};
    if (!GetFileInformationByHandle(h, &identity) || !SameFile(it->second.identity, identity)) {
        // An unhooked close followed by an unhooked open bypasses Track too.
        // Check the live object before ever copying cached resource bytes.
        g_handles.erase(it);
        ++g_staleHandles;
        ++g_fallback; guard.Release(); SetLastError(before); return ReadFile(h, buffer, count, read, overlapped);
    }
    LARGE_INTEGER zero{}, pos;
    if (!SetFilePointerEx(h, zero, &pos, FILE_CURRENT) || pos.QuadPart < 0) {
        ++g_fallback; guard.Release(); SetLastError(before); return ReadFile(h, buffer, count, read, overlapped);
    }
    const auto entry = it->second.entry;
    uint64_t at = uint64_t(pos.QuadPart);
    DWORD n = DWORD(std::min<uint64_t>(count, at < entry->size ? entry->size - at : 0));
    // Bound views/copies even if a caller asks for a very large read.
    DWORD copied = 0;
    while (copied < n) {
        DWORD part = std::min<DWORD>(n - copied, 8 * 1024 * 1024);
        const auto source = Map(entry->offset + at + copied, part);
        if (!source) { ++g_fallback; guard.Release(); SetLastError(before); return ReadFile(h, buffer, count, read, overlapped); }
        memcpy(static_cast<uint8_t*>(buffer) + copied, source, part);
        copied += part;
    }
    pos.QuadPart += n;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) return FALSE;
    *read = n; ++g_hits; g_bytes += n;
    if (n && !_wcsnicmp(entry->relative, L"Fonts\\", 6) && g_fontReads.insert(entry).second) {
        char font[2048]{}; WideCharToMultiByte(CP_UTF8, 0, entry->relative, -1, font, int(sizeof(font)), nullptr, nullptr);
        Log("Font resource served from RAM: "); Log(font); Log("\r\n");
    }
    if ((g_hits & 4095) == 1) Stats();
    SetLastError(before); return TRUE;
}
static DWORD WINAPI CacheSetFilePointer(HANDLE h, LONG distance, PLONG high, DWORD method) {
    if (Stopping()) return SetFilePointer(h, distance, high, method);
    Guard guard; return SetFilePointer(h, distance, high, method);
}
static BOOL WINAPI CacheCloseHandle(HANDLE h) {
    if (Stopping()) return CloseHandle(h);
    Guard guard; auto found = g_handles.find(h);
    BOOL result = CloseHandle(h);
    if (result && found != g_handles.end()) g_handles.erase(found);
    return result;
}
static VOID WINAPI CacheExitProcess(UINT code) {
    if (Stopping()) ExitProcess(code);
    { Guard guard; Stats(); }
    FlushAudioTrace();
    InterlockedExchange(&g_stopping, 1);
    ExitProcess(code);
}

extern "C" __declspec(dllexport) DWORD WINAPI ModernizeStats(CacheCounters* out) {
    if (!out) return 1;
    if (Stopping()) return ERROR_OPERATION_ABORTED;
    Guard guard; *out = {g_hits, g_bytes, g_opens, g_fallback}; return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI ModernizeStart(void*) {
    if (Stopping()) return ERROR_OPERATION_ABORTED;
    InitializeCriticalSection(&g_lock);
    auto session = Environment(L"MW_MODERN_SESSION");
    auto logfile = Environment(L"MW_MODERN_LOG");
    g_log = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto dumpPath = logfile + L".first-exception-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L".bin";
    if (dumpPath.size() < std::size(g_dumpPath)) wcscpy_s(g_dumpPath, dumpPath.c_str());
    OpenCrashBroker(session);
    SetUnhandledExceptionFilter(CrashReport);
    Log(g_crashRequest ? "First-crash capture: external x86 runner\r\n" : "First-crash capture: raw exception/context fallback\r\n");
    Log("mw_rebreathe 0.2.0 stable profile x86 starting\r\n");
    g_manifest = OpenFileMappingW(FILE_MAP_READ, FALSE, (session + L"_index").c_str());
    if (!g_manifest) { Log("ERROR cache manifest unavailable\r\n"); return 1; }
    g_header = static_cast<const CacheHeader*>(MapViewOfFile(g_manifest, FILE_MAP_READ, 0, 0, 0));
    if (!g_header || g_header->magic != kCacheMagic || g_header->version != kCacheVersion || g_header->count > 1000000) {
        Log("ERROR invalid cache manifest\r\n"); return 2;
    }
    g_data = OpenFileMappingW(FILE_MAP_READ, FALSE, (session + L"_data").c_str());
    if (!g_data) { Log("ERROR cache data unavailable\r\n"); return 3; }
    g_root = Lower(g_header->root);
    auto entries = reinterpret_cast<const CacheEntry*>(g_header + 1);
    for (uint32_t i = 0; i < g_header->count; ++i) {
        if (entries[i].offset > g_header->totalBytes || entries[i].size > g_header->totalBytes - entries[i].offset) return 4;
        g_paths.emplace(g_root + L"\\" + Lower(entries[i].relative), entries + i);
    }
    SYSTEM_INFO info; GetSystemInfo(&info); g_granularity = info.dwAllocationGranularity;
    HMODULE game = GetModuleHandleW(nullptr);
    if (Environment(L"MW_MODERN_PROBE") != L"1") {
        SetDisplayLogCallback(Log);
        if (!InstallDisplayHooks(game)) { Log("ERROR display hooks refused\r\n"); return 8; }
        Log(DisplayHookStatus()); Log("\r\n");
        if (!InstallUiScaleHooks(game)) { Log("ERROR UI scale signature/config refused\r\n"); return 9; }
        Log(UiScaleHookStatus()); Log("\r\n");
        if (!InstallFillbarFontHooks(game)) { Log("ERROR fillbar font hook refused\r\n"); return 11; }
        Log(FillbarFontHookStatus()); Log("\r\n");
        SetMenuStyleLogCallback(Log);
        if (!InstallMenuStyle(game)) { Log("ERROR main menu style refused\r\n"); return 12; }
        Log(MenuStyleStatus()); Log("\r\n");
        if (!InstallOptionsLayoutHooks(game)) { Log("ERROR Options layout refused\r\n"); return 13; }
        Log(OptionsLayoutHookStatus()); Log("\r\n");

        SetMusicVolumeLogCallback(LogLine);
        if (!InstallMusicVolumeHooks(game)) { Log("ERROR music master volume hook refused\r\n"); return 18; }
        Log("Music master volume: "); Log(MusicVolumeHookStatus()); Log("\r\n");
        SetScreenshotLogCallback(LogLine);
        if (!InstallScreenshotHooks(game)) { Log("ERROR physical screenshot hook refused\r\n"); return 14; }
        Log(ScreenshotHookStatus()); Log("\r\n");
    }
    if (Environment(L"MW_MODERN_PROBE") != L"1" && !InstallResolutionHooks(game)) { Log("ERROR unsupported resolution patch signature\r\n"); return 5; }
    if (Environment(L"MW_MODERN_PROBE") != L"1") {
        Log(ResolutionHookStatus()); Log("\r\n");
        if (Environment(L"MW_MODERN_TRACE") == L"1") {
            if (!InstallAudioTrace(game)) { Log("ERROR audio volume trace refused\r\n"); return 17; }
            Log("Audio volume trace: "); Log(AudioTraceStatus()); Log("\r\n");
        }
        Log("Stable profile: RAM cache; original cell loading, map generation and worker waits\r\n");
    }
    if (Environment(L"MW_MODERN_PROBE") == L"1") LoadLibraryW(L"msvcrt.dll");
    HMODULE modules[] = {game, GetModuleHandleW(L"msvcrt.dll"), GetModuleHandleW(L"binkw32.dll")};
    bool readHook = false;
    for (auto module : modules) {
        PatchImport(module, "CreateFileA", reinterpret_cast<void*>(CacheCreateFileA));
        PatchImport(module, "CreateFileW", reinterpret_cast<void*>(CacheCreateFileW));
        readHook |= PatchImport(module, "ReadFile", reinterpret_cast<void*>(CacheReadFile));
        PatchImport(module, "SetFilePointer", reinterpret_cast<void*>(CacheSetFilePointer));
        PatchImport(module, "CloseHandle", reinterpret_cast<void*>(CacheCloseHandle));
    }
    PatchImport(game, "ExitProcess", reinterpret_cast<void*>(CacheExitProcess));
    const auto mmio = MorrowindModernMmio::Install({CacheReadFile, CacheCreateFileW, CacheSetFilePointer, CacheCloseHandle});
    char mmioStatus[192];
    sprintf_s(mmioStatus, "WAV/MMIO cache: modules=%u patched_imports=%u read_adapter=%d\r\n", mmio.modules, mmio.slots, mmio.canRead);
    Log(mmioStatus);
    Log(readHook ? "IAT read hooks installed; four bounded 8 MiB views\r\n" : "ERROR no ReadFile import hooked\r\n");
    if (readHook && Environment(L"MW_MODERN_PROBE") != L"1") {
        HANDLE logger = CreateThread(nullptr, 0, PeriodicStats, nullptr, 0, nullptr);
        if (logger) CloseHandle(logger);
    }
    return readHook ? 0 : 6;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    else if (reason == DLL_PROCESS_DETACH) InterlockedExchange(&g_stopping, 1);
    return TRUE;
}
