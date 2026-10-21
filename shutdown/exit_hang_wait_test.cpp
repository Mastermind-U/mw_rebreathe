// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real runner's final wait, not a copied polling implementation.
#define wmain RunnerInternalMain
#include "../src/runner.cpp"
#undef wmain
#include <cstring>
#include <stdexcept>

static_assert(sizeof(void*) == 4, "The wait harness and target fixture are x86");

namespace {
unsigned checks = 0, serial = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::wstring FullPath(const wchar_t* path) {
    wchar_t full[32768]{};
    const DWORD length = GetFullPathNameW(path, 32768, full, nullptr);
    Check(length && length < 32768, "Cannot resolve synthetic fixture path");
    return full;
}
struct SavedEnvironment {
    const wchar_t* name;
    std::wstring value;
    bool existed = false;
    explicit SavedEnvironment(const wchar_t* key) : name(key) {
        wchar_t buffer[32768]{};
        const DWORD size = GetEnvironmentVariableW(name, buffer, 32768);
        existed = size > 0 && size < 32768;
        if (existed) value = buffer;
    }
    ~SavedEnvironment() { SetEnvironmentVariableW(name, existed ? value.c_str() : nullptr); }
};
struct Child {
    PROCESS_INFORMATION process{};
    HANDLE ready = nullptr, stop = nullptr, clear = nullptr;
    explicit Child(const std::wstring& executable) {
        const auto token = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial);
        const auto readyName = L"Local\\MWExitWait-ready-" + token;
        const auto stopName = L"Local\\MWExitWait-stop-" + token;
        const auto clearName = L"Local\\MWExitWait-clear-" + token;
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        stop = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
        clear = CreateEventW(nullptr, TRUE, FALSE, clearName.c_str());
        Check(ready && stop && clear, "Cannot create synthetic child events");
        std::wstring command = L"\"" + executable + L"\" --child \"" + readyName + L"\" \"" +
            stopName + L"\" \"" + clearName + L"\" 1";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "Cannot create synthetic child");
        Check(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "Synthetic child did not initialize");
    }
    ~Child() {
        if (stop) SetEvent(stop);
        if (process.hProcess) {
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
        if (clear) CloseHandle(clear);
    }
};
std::vector<std::wstring> Dumps(const std::wstring& log) {
    std::vector<std::wstring> result;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((log + L".hang-*.dmp").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return result;
    const auto parent = log.substr(0, log.find_last_of(L"\\/") + 1);
    do { result.push_back(parent + data.cFileName); } while (FindNextFileW(search, &data));
    FindClose(search);
    return result;
}
bool CompleteDump(const std::wstring& path) {
    // FILE_SHARE_READ rejects the writer's still-open GENERIC_WRITE handle.
    // The child is released only after MiniDumpWriteDump closed that handle.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_HEADER header{};
    DWORD read = 0;
    const bool ready = ReadFile(file, &header, sizeof(header), &read, nullptr) &&
        read == sizeof(header) && header.Signature == MINIDUMP_SIGNATURE;
    CloseHandle(file);
    return ready;
}
struct ReleaseContext {
    HANDLE process = nullptr, stop = nullptr;
    std::wstring log;
    bool requireDump = true, dumpCompleted = false, childAliveBeforeRelease = false;
    bool stopSignaled = false, timedOut = false, failed = false;
};
DWORD WINAPI ReleaseChild(void* argument) {
    auto& context = *static_cast<ReleaseContext*>(argument);
    try {
        const ULONGLONG start = GetTickCount64();
        if (context.requireDump) {
            for (;;) {
                const auto dumps = Dumps(context.log);
                if (dumps.size() == 1 && CompleteDump(dumps[0])) {
                    context.dumpCompleted = true;
                    break;
                }
                if (GetTickCount64() - start >= 7000) { context.timedOut = true; break; }
                Sleep(25);
            }
        } else Sleep(350);
        context.childAliveBeforeRelease = WaitForSingleObject(context.process, 0) == WAIT_TIMEOUT;
    } catch (...) { context.failed = true; }
    context.stopSignaled = SetEvent(context.stop) != FALSE;
    return 0;
}
std::vector<unsigned char> ReadFileBytes(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "Cannot open completed hang dump");
    LARGE_INTEGER size{};
    const bool validSize = GetFileSizeEx(file, &size) && size.QuadPart >= sizeof(MINIDUMP_HEADER) &&
        size.QuadPart < 64 * 1024 * 1024;
    if (!validSize) { CloseHandle(file); Check(false, "Invalid hang dump size"); }
    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool readAll = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
        read == bytes.size();
    CloseHandle(file);
    Check(readAll, "Cannot read completed hang dump");
    return bytes;
}
template<class T> T At(const std::vector<unsigned char>& bytes, size_t offset) {
    Check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "Minidump field outside file");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
void VerifyDump(const std::wstring& path, const PROCESS_INFORMATION& process) {
    const auto bytes = ReadFileBytes(path);
    const auto header = At<MINIDUMP_HEADER>(bytes, 0);
    Check(header.Signature == MINIDUMP_SIGNATURE, "Invalid minidump signature");
    MINIDUMP_DIRECTORY threads{}, info{}, misc{}, system{}, unloaded{};
    bool exception = false;
    for (ULONG i = 0; i < header.NumberOfStreams; ++i) {
        const auto entry = At<MINIDUMP_DIRECTORY>(bytes, header.StreamDirectoryRva + i * sizeof(MINIDUMP_DIRECTORY));
        if (entry.StreamType == ThreadListStream) threads = entry;
        if (entry.StreamType == ThreadInfoListStream) info = entry;
        if (entry.StreamType == MiscInfoStream) misc = entry;
        if (entry.StreamType == SystemInfoStream) system = entry;
        if (entry.StreamType == UnloadedModuleListStream) unloaded = entry;
        if (entry.StreamType == ExceptionStream) exception = true;
    }
    Check(threads.Location.Rva && info.Location.Rva && misc.Location.Rva && system.Location.Rva,
        "Target identity/thread/context streams missing");
    Check(!exception, "Hang snapshot invented an exception");
    const auto machine = At<MINIDUMP_SYSTEM_INFO>(bytes, system.Location.Rva);
    Check(machine.ProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL, "Dump is not x86");
    const auto identity = At<MINIDUMP_MISC_INFO>(bytes, misc.Location.Rva);
    Check((identity.Flags1 & MINIDUMP_MISC1_PROCESS_ID) && identity.ProcessId == process.dwProcessId,
        "Hang dump belongs to another process");
    const DWORD count = At<DWORD>(bytes, threads.Location.Rva);
    Check(count >= 2, "Target's main and worker threads were not both captured");
    bool initialThread = false, marker = false;
    for (DWORD i = 0; i < count; ++i) {
        const auto thread = At<MINIDUMP_THREAD>(bytes, threads.Location.Rva + 4 + i * sizeof(MINIDUMP_THREAD));
        initialThread |= thread.ThreadId == process.dwThreadId;
        Check(thread.ThreadContext.DataSize >= 204, "Target x86 thread context missing");
        const DWORD flags = At<DWORD>(bytes, thread.ThreadContext.Rva);
        const DWORD eip = At<DWORD>(bytes, thread.ThreadContext.Rva + 184);
        const DWORD esp = At<DWORD>(bytes, thread.ThreadContext.Rva + 196);
        Check((flags & CONTEXT_i386) == CONTEXT_i386 && eip && thread.Stack.Memory.DataSize &&
            esp >= thread.Stack.StartOfMemoryRange &&
            esp < thread.Stack.StartOfMemoryRange + thread.Stack.Memory.DataSize,
            "Target x86 instruction/stack context invalid");
        const size_t begin = thread.Stack.Memory.Rva;
        const DWORD size = thread.Stack.Memory.DataSize;
        Check(begin <= bytes.size() && size <= bytes.size() - begin, "Captured target stack missing");
        for (DWORD offset = 0; offset + 4 <= size; ++offset) {
            DWORD word = 0;
            std::memcpy(&word, bytes.data() + begin + offset, 4);
            if (word == 0x5A17CAFE) marker = true;
        }
    }
    Check(initialThread && marker, "Captured stacks do not prove the exact live fixture threads");
    const auto threadInfo = At<MINIDUMP_THREAD_INFO_LIST>(bytes, info.Location.Rva);
    Check(threadInfo.NumberOfEntries >= 2 && unloaded.Location.Rva,
        "ThreadInfo/unloaded-module capture flags were not exercised");
}
void Scenario(const std::wstring& executable, const std::wstring& directory,
    bool availableBroker, bool watcher) {
    Child child(executable);
    const auto tag = std::wstring(availableBroker ? L"broker" : L"no-broker") +
        (watcher ? L"-watch" : L"-probe");
    const auto log = directory + L"\\" + tag + L".log";
    const auto session = L"Local\\MWExitWaitBroker-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(serial);
    Check(SetEnvironmentVariableW(L"MW_MODERN_SESSION", session.c_str()) &&
        SetEnvironmentVariableW(L"MW_MODERN_LOG", log.c_str()), "Cannot set isolated runner environment");
    CrashBroker broker;
    if (availableBroker) Check(broker.Initialize() && broker.Available(), "Actual CrashBroker did not initialize");
    else Check(!broker.Available(), "Uninitialized CrashBroker unexpectedly available");
    ExitHangWatchdog watchdog;
    Check(watchdog.Initialize(child.process.hProcess, !watcher) == watcher,
        "Watchdog/probe gating disagrees with final wait integration");
    ReleaseContext release{};
    release.process = child.process.hProcess;
    release.stop = child.stop;
    release.log = log;
    release.requireDump = watcher;
    HANDLE worker = CreateThread(nullptr, 0, &ReleaseChild, &release, 0, nullptr);
    Check(worker != nullptr, "Cannot start private child release worker");
    const ULONGLONG start = GetTickCount64();
    const DWORD wait = broker.Wait(child.process.hProcess, INFINITE, child.process.hProcess,
        watcher ? &watchdog : nullptr);
    const ULONGLONG elapsed = GetTickCount64() - start;
    const DWORD workerWait = WaitForSingleObject(worker, 10000);
    CloseHandle(worker);
    Check(workerWait == WAIT_OBJECT_0, "Private release worker did not finish");
    Check(wait == WAIT_OBJECT_0, "Real CrashBroker final Wait did not return process signal");
    Check(release.stopSignaled && release.childAliveBeforeRelease && !release.failed && !release.timedOut,
        "Child did not survive capture until its own private stop event");
    DWORD exitCode = STILL_ACTIVE;
    Check(GetExitCodeProcess(child.process.hProcess, &exitCode) && exitCode == 0,
        "Synthetic child did not return normally with exit code zero");
    const auto dumps = Dumps(log);
    if (watcher) {
        Check(release.dumpCompleted && dumps.size() == 1 && elapsed >= 2000 && elapsed < 7000,
            "Real 250ms wait loop failed to produce exactly one timed hang snapshot");
        VerifyDump(dumps[0], child.process);
        Check(std::strstr(watchdog.Status(), "hang dump captured") != nullptr, "Watchdog did not report successful capture");
    } else Check(dumps.empty() && elapsed < 2000, "Probe/no-watch final wait produced a hang dump or delayed exit");
    std::printf("scenario=%ls broker=%d watcher=%d elapsed_ms=%llu pid=%lu exit=%lu dumps=%zu\n",
        tag.c_str(), availableBroker ? 1 : 0, watcher ? 1 : 0, elapsed, child.process.dwProcessId, exitCode, dumps.size());
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "Pass only the synthetic Morrowind.exe fixture path");
        const auto executable = FullPath(argv[1]);
        const auto directory = executable.substr(0, executable.find_last_of(L"\\/")) + L"\\wait-run-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        Check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "Cannot create private test artifact directory");
        SavedEnvironment trace(L"MW_MODERN_TRACE"), probe(L"MW_MODERN_PROBE"), log(L"MW_MODERN_LOG"), session(L"MW_MODERN_SESSION");
        Check(SetEnvironmentVariableW(L"MW_MODERN_TRACE", L"1") && SetEnvironmentVariableW(L"MW_MODERN_PROBE", nullptr),
            "Cannot set isolated trace flags");
        Scenario(executable, directory, true, true);
        Scenario(executable, directory, false, true);
        Scenario(executable, directory, true, false);
        Scenario(executable, directory, false, false);
        std::printf("PASS real-runner-final-wait checks=%u positive=2 probe=2 synthetic_only=1 no_termination=1\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
