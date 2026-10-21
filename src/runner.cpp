#include "shared.h"
#include <cstdio>
#include <cstring>
#include <dbghelp.h>
#include "../audio/game_audio_target.h"
#include "../shutdown/exit_hang_watchdog.h"
#pragma comment(lib, "dbghelp.lib")

// DbgHelp runs in this healthy process, never on the game's damaged heap.
class CrashBroker {
    HANDLE mapping_ = nullptr, ready_ = nullptr, acknowledgement_ = nullptr;
    CrashRequest* request_ = nullptr;
    std::wstring log_;
    bool captured_ = false;
public:
    ~CrashBroker() {
        if (request_) UnmapViewOfFile(request_);
        if (mapping_) CloseHandle(mapping_);
        if (ready_) CloseHandle(ready_);
        if (acknowledgement_) CloseHandle(acknowledgement_);
    }
    bool Initialize() {
        const auto session = Environment(L"MW_MODERN_SESSION");
        log_ = Environment(L"MW_MODERN_LOG");
        if (session.empty() || log_.empty()) return false;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(CrashRequest), (session + L"_crash").c_str());
        if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        request_ = static_cast<CrashRequest*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(CrashRequest)));
        if (!request_) return false;
        ready_ = CreateEventW(nullptr, FALSE, FALSE, (session + L"_crash_ready").c_str());
        if (!ready_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        acknowledgement_ = CreateEventW(nullptr, TRUE, FALSE, (session + L"_crash_ack").c_str());
        if (!acknowledgement_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        *request_ = {};
        request_->magic = kCrashMagic; request_->version = kCrashVersion;
        return true;
    }
    bool Available() const { return request_ && ready_ && acknowledgement_ && request_->magic == kCrashMagic; }
    HANDLE Ready() const { return ready_; }
    void Capture(HANDLE process) {
        if (captured_) { SetEvent(acknowledgement_); return; }
        captured_ = true;
        const DWORD processId = GetProcessId(process);
        DWORD error = ERROR_INVALID_DATA;
        BOOL success = FALSE;
        std::wstring path;
        if (request_->magic == kCrashMagic && request_->version == kCrashVersion &&
            InterlockedCompareExchange(&request_->state, 2, 2) == 2 &&
            request_->processId == processId && request_->threadId && request_->exceptionPointers) {
            // CREATE_NEW preserves previous dumps, including an earlier run with
            // a recycled process ID. No exception can overwrite the first dump.
            HANDLE dump = INVALID_HANDLE_VALUE;
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                path = log_ + L".first-" + std::to_wstring(processId) + L"-" +
                    std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".dmp";
                dump = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (dump != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
            }
            if (dump != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION exception{};
                exception.ThreadId = request_->threadId;
                exception.ExceptionPointers = reinterpret_cast<EXCEPTION_POINTERS*>(static_cast<uintptr_t>(request_->exceptionPointers));
                exception.ClientPointers = TRUE;
                const auto flags = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                    MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                    MiniDumpWithFullMemoryInfo | MiniDumpWithProcessThreadData);
                success = MiniDumpWriteDump(process, processId, dump, flags, &exception, nullptr, nullptr);
                error = success ? ERROR_SUCCESS : GetLastError();
                CloseHandle(dump);
                if (!success) DeleteFileW(path.c_str());
            } else error = GetLastError();
        }
        request_->dumpError = error;
        InterlockedExchange(&request_->dumpSucceeded, success ? 1 : 0);
        SetEvent(acknowledgement_);
        wchar_t description[1024]{};
        _snwprintf_s(description, _TRUNCATE, L"FIRST crash broker: code=%08lX address=%08lX pid=%lu thread=%lu dump=%ls error=%lu\r\n",
            request_->exceptionCode, request_->exceptionAddress, processId, request_->threadId,
            success ? path.c_str() : L"FAILED", error);
        fwprintf(stderr, L"%ls", description);
        char utf8[4096]{};
        const int length = WideCharToMultiByte(CP_UTF8, 0, description, -1, utf8, int(sizeof(utf8)), nullptr, nullptr);
        HANDLE log = CreateFileW(log_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (length > 1) WriteFile(log, utf8, DWORD(length - 1), &written, nullptr);
            CloseHandle(log);
        }
    }
    DWORD Wait(HANDLE object, DWORD timeout, HANDLE process, ExitHangWatchdog* watchdog = nullptr) {
        const bool watchingExit = watchdog && object == process && timeout == INFINITE;
        const bool brokerAvailable = Available();
        if (!brokerAvailable && !watchingExit) return WaitForSingleObject(object, timeout);
        const ULONGLONG start = GetTickCount64();
        HANDLE objects[]{brokerAvailable ? ready_ : object, object};
        const DWORD count = brokerAvailable ? 2u : 1u;
        char lastWatchdogStatus[4096]{};
        for (;;) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            DWORD remaining = timeout == INFINITE ? INFINITE : elapsed >= timeout ? 0 : DWORD(timeout - elapsed);
            if (watchingExit && remaining > 250) remaining = 250;
            const DWORD result = WaitForMultipleObjects(count, objects, FALSE, remaining);
            if (brokerAvailable && result == WAIT_OBJECT_0) { Capture(process); continue; }
            if (result == WAIT_OBJECT_0 + (brokerAvailable ? 1u : 0u)) return WAIT_OBJECT_0;
            if (watchingExit && result == WAIT_TIMEOUT) {
                // DbgHelp remains in this healthy x86 runner. An original AV
                // already captured by the crash broker takes precedence.
                if (!captured_) {
                    watchdog->Poll();
                    const char* status = watchdog->Status();
                    if (std::strcmp(lastWatchdogStatus, status)) {
                        strncpy_s(lastWatchdogStatus, status, _TRUNCATE);
                        fprintf(stderr, "Exit hang watchdog: %s\n", lastWatchdogStatus);
                        fflush(stderr);
                    }
                }
                continue;
            }
            return result;
        }
    }
};

// This helper has the same x86 architecture as the game and injected DLL.
// The cache owner remains a separate x64 process.
static bool Inject(PROCESS_INFORMATION& game, const std::wstring& dll, CrashBroker& broker) {
    auto address = VirtualAllocEx(game.hProcess, nullptr, (dll.size() + 1) * sizeof(wchar_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!address) return false;
    SIZE_T written;
    if (!WriteProcessMemory(game.hProcess, address, dll.c_str(), (dll.size() + 1) * sizeof(wchar_t), &written)) return false;
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    auto thread = CreateRemoteThread(game.hProcess, nullptr, 0, load, address, 0, nullptr);
    if (!thread) return false;
    // Do not free a remote argument while its thread could still be using it.
    if (broker.Wait(thread, 30000, game.hProcess) != WAIT_OBJECT_0) { CloseHandle(thread); return false; }
    DWORD remoteBase = 0; GetExitCodeThread(thread, &remoteBase); CloseHandle(thread);
    VirtualFreeEx(game.hProcess, address, 0, MEM_RELEASE);
    if (!remoteBase) return false;
    auto local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return false;
    auto init = GetProcAddress(local, "ModernizeStart");
    if (!init) { FreeLibrary(local); return false; }
    uintptr_t offset = reinterpret_cast<uintptr_t>(init) - reinterpret_cast<uintptr_t>(local);
    FreeLibrary(local);
    auto start = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase + offset);
    thread = CreateRemoteThread(game.hProcess, nullptr, 0, start, nullptr, 0, nullptr);
    if (!thread) return false;
    if (broker.Wait(thread, 30000, game.hProcess) != WAIT_OBJECT_0) { CloseHandle(thread); return false; }
    DWORD status = 1; GetExitCodeThread(thread, &status); CloseHandle(thread);
    if (status) fwprintf(stderr, L"DLL initialization failed: %lu\n", status);
    return status == 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) { fwprintf(stderr, L"Internal helper: runner.exe <target.exe> <game-directory> <DLL>\n"); return 2; }
    CrashBroker broker;
    if (!broker.Initialize()) {
        fwprintf(stderr, L"Crash broker unavailable: %lu; target will retain a raw first-exception fallback.\n", GetLastError());
    }
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION game{};
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, argv[2], &startup, &game)) {
        fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError()); return 3;
    }
    if (!Inject(game, argv[3], broker)) {
        fwprintf(stderr, L"Injection failed: %lu; game was not resumed.\n", GetLastError());
        TerminateProcess(game.hProcess, 4); CloseHandle(game.hThread); CloseHandle(game.hProcess); return 4;
    }
    RunnerAudioTarget audioTarget;
    const auto session = Environment(L"MW_MODERN_SESSION");
    // Optional host control. A missing channel must not change probe/crash
    // broker behavior or prevent an otherwise valid game from starting.
    audioTarget.Publish(session.c_str(), game.hProcess);
    ExitHangWatchdog exitWatchdog;
    const bool probe = Environment(L"MW_MODERN_PROBE") == L"1";
    const bool watchExit = exitWatchdog.Initialize(game.hProcess, probe);
    if (!watchExit && !probe && Environment(L"MW_MODERN_TRACE") == L"1") {
        fprintf(stderr, "Exit hang watchdog unavailable: %s\n", exitWatchdog.Status());
    }
    ResumeThread(game.hThread); CloseHandle(game.hThread);
    broker.Wait(game.hProcess, INFINITE, game.hProcess, watchExit ? &exitWatchdog : nullptr);
    DWORD exitCode; GetExitCodeProcess(game.hProcess, &exitCode);
    audioTarget.Close(); CloseHandle(game.hProcess);
    return int(exitCode);
}
