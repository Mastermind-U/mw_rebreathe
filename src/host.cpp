#include "shared.h"
#include <filesystem>
#include <cstdio>
#include <bcrypt.h>
#include <stdexcept>
#include <tlhelp32.h>
#include <cmath>
#include "../launcher/settings.h"
#include "../audio/game_audio_target.h"
#include "../audio/session_mute.h"
#pragma comment(lib, "bcrypt.lib")
namespace fs = std::filesystem;
static bool g_interactive = false;
struct Source { fs::path path; CacheEntry entry{}; };
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void WaitForRunner(HANDLE runner, HostGameAudioTarget* target) {
    if (!target) {
        Require(WaitForSingleObject(runner, INFINITE) == WAIT_OBJECT_0, "Cannot wait for x86 runner");
        return;
    }
    // Windows audio COM objects stay on this healthy host thread. The game is
    // sampled read-only through the exact process published by its own runner.
    AudioSessionMuteGuard mute;
    DWORD verifiedAudioPid = 0;
    DWORD previousPid = 0, previousChanges = 0, previousErrors = 0, previousSessions = 0;
    bool previousKnown = false, previousZero = false;
    ULONGLONG nextErrorLog = 0;
    for (;;) {
        const DWORD result = WaitForSingleObject(runner, 100);
        if (result == WAIT_OBJECT_0) break;
        Require(result == WAIT_TIMEOUT, "Cannot wait for x86 runner");
        const GameAudioSample sample = target->Sample();
        // The target keeps this process handle pinned through mute cleanup, so
        // its PID remains reserved even after the child exits. Keep authority
        // to restore our sessions when native World data becomes unavailable.
        if (sample.pid) verifiedAudioPid = sample.pid;
        mute.Update(verifiedAudioPid, sample.masterKnown, sample.masterZero);
        const auto stats = mute.Stats();
        const ULONGLONG now = GetTickCount64();
        const bool reportError = stats.errors != previousErrors && now >= nextErrorLog;
        if (sample.pid != previousPid || sample.masterKnown != previousKnown || sample.masterZero != previousZero ||
            stats.changes != previousChanges || reportError ||
            stats.matchedSessions != previousSessions) {
            wprintf(L"Audio session guard: pid=%lu known=%u master_zero=%u sessions=%lu owned=%lu changes=%lu errors=%lu result=%08lX\n",
                verifiedAudioPid, sample.masterKnown ? 1u : 0u, sample.masterZero ? 1u : 0u,
                stats.matchedSessions, stats.ownedMutes, stats.changes, stats.errors,
                static_cast<unsigned long>(stats.lastResult));
            fflush(stdout);
            previousKnown = sample.masterKnown; previousZero = sample.masterZero;
            previousPid = sample.pid;
            previousChanges = stats.changes; previousErrors = stats.errors;
            previousSessions = stats.matchedSessions;
            nextErrorLog = now + 5000;
        }
    }
    mute.Update(verifiedAudioPid, false, false);
    mute.Restore();
    const auto finalStats = mute.Stats();
    wprintf(L"Audio session guard stopped: owned_remaining=%lu changes=%lu errors=%lu result=%08lX\n",
        finalStats.ownedMutes, finalStats.changes, finalStats.errors,
        static_cast<unsigned long>(finalStats.lastResult));
    fflush(stdout);
}
static bool GameAlreadyRunning(const fs::path& target) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot check for an existing game process");
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.h, &entry)) return false;
    do {
        if (_wcsicmp(entry.szExeFile, L"Morrowind.exe")) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        wchar_t image[32768]; DWORD length = DWORD(std::size(image));
        if (!process.h || !QueryFullProcessImageNameW(process.h, 0, image, &length)) return true;
        if (Lower(FullPath(image)) == Lower(FullPath(target.wstring()))) return true;
    } while (Process32NextW(snapshot.h, &entry));
    return false;
}
static std::string HashFile(const fs::path& p) {
    Handle file(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    Require(file.h != INVALID_HANDLE_VALUE, "Cannot read executable");
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash;
    Require(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "SHA256 unavailable");
    DWORD objectSize, out; BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &out, 0);
    std::vector<uint8_t> object(objectSize), block(1024 * 1024);
    Require(BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) == 0, "SHA256 init failed");
    DWORD n;
    while (ReadFile(file.h, block.data(), DWORD(block.size()), &n, nullptr) && n) BCryptHashData(hash, block.data(), n, 0);
    uint8_t digest[32]; BCryptFinishHash(hash, digest, sizeof(digest), 0); BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    std::string result; const char* hex = "0123456789abcdef";
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
int wmain(int argc, wchar_t** argv) {
    try {
        wchar_t self[32768]; GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
        fs::path bin = fs::path(self).parent_path();
        fs::path gameRoot = bin.parent_path().parent_path();
        bool probe = false, lock = true, warmOnly = false, trace = false;
        bool showSettings = argc == 1;
        DEVMODEW desktop{}; desktop.dmSize = sizeof(desktop);
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop);
        int width = int(desktop.dmPelsWidth), height = int(desktop.dmPelsHeight), screenMode = 2;
        double uiScale = height >= 1800 ? 2.0 : height >= 1350 ? 1.5 : height >= 1080 ? 1.25 : 1.0;
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--game-dir" && i + 1 < argc) gameRoot = FullPath(argv[++i]);

            else if (a == L"--probe") probe = true;
            else if (a == L"--allow-pageable") lock = false;
            else if (a == L"--warm-only") warmOnly = true;
            else if (a == L"--trace") trace = true;
            else if (a == L"--settings") showSettings = true;
            else if (a == L"--screen-mode" && i + 1 < argc) {
                std::wstring mode = argv[++i];
                Require(mode == L"fullscreen" || mode == L"window" || mode == L"borderless", "Invalid --screen-mode");
                screenMode = mode == L"fullscreen" ? 0 : mode == L"window" ? 1 : 2;
            }
            else if (a == L"--resolution" && i + 1 < argc) {
                wchar_t extra; Require(swscanf_s(argv[++i], L"%dx%d%c", &width, &height, &extra, 1u) == 2, "Expected --resolution WIDTHxHEIGHT");
            }
            else if (a == L"--ui-scale" && i + 1 < argc) {
                wchar_t* end; uiScale = wcstod(argv[++i], &end); Require(!*end && std::isfinite(uiScale), "Invalid UI scale");
            }
            else { fwprintf(stderr, L"Usage: MorrowindModern.exe [--settings] [--game-dir PATH] [--screen-mode fullscreen|window|borderless] [--resolution WIDTHxHEIGHT] [--ui-scale 1.5] [--trace] [--allow-pageable] [--probe] [--warm-only]\n"); return 2; }
        }
        g_interactive = showSettings;
        if (showSettings && !probe && !warmOnly) {
            MorrowindModernSettings::LaunchSettings settings;
            if (!MorrowindModernSettings::ConfigureLaunch(settings)) return 0;
            width = settings.width; height = settings.height; screenMode = settings.screenMode; uiScale = settings.uiScale;
            trace = settings.trace; lock = settings.lockRam;
        }
        Require(width >= 640 && height >= 480 && width <= 16384 && height <= 16384, "Display dimensions out of range");
        Require(std::isfinite(uiScale) && uiScale >= 1.0 && uiScale <= 3.0 && width / uiScale >= 640 && height / uiScale >= 480, "UI scale leaves too little space; need at least 640x480 logical pixels");
        gameRoot = FullPath(gameRoot.wstring());
        Require(fs::exists(gameRoot / L"Morrowind.ini"), "Choose a Morrowind installation with --game-dir");
        auto target = probe ? bin / L"cache_probe.exe" : gameRoot / L"Morrowind.exe";
        if (!probe) Require(!GameAlreadyRunning(target), "This Morrowind installation is already running; close the game first");
        if (!probe) Require(HashFile(target) == "eac72e1b3524ee0012d080d3c2e2e00e401464e41ee54f9afd6aaaadf2c221f5", "Unsupported Morrowind.exe. Only original Steam 1.6.1820 English build is currently verified.");
        Require(fs::exists(bin / L"modernizer.dll") && fs::exists(bin / L"runner.exe"), "Missing x86 components");
        auto dataRoot = gameRoot / L"Data Files";
        std::vector<Source> files; uint64_t total = 0, payload = 0;
        for (const auto& file : fs::recursive_directory_iterator(dataRoot)) {
            if (file.is_symlink()) throw std::runtime_error("Data Files symlinks/reparse points are unsupported");
            if (!file.is_regular_file() || !file.file_size()) continue;
            auto relative = fs::relative(file.path(), dataRoot).wstring();
            Require(relative.size() < kPathChars, "Resource path too long");
            WIN32_FILE_ATTRIBUTE_DATA info;
            Require(GetFileAttributesExW(file.path().c_str(), GetFileExInfoStandard, &info), "Cannot stat a resource");
            Source s; s.path = file.path(); s.entry.offset = total; s.entry.size = file.file_size(); s.entry.writeTime = FileTimeValue(info.ftLastWriteTime);
            wcscpy_s(s.entry.relative, relative.c_str()); files.push_back(s);
            payload += s.entry.size; total = (total + s.entry.size + 63) & ~uint64_t(63);
        }
        Require(total > 0 && files.size() <= 1000000, "Invalid resource set");
        MEMORYSTATUSEX memory{sizeof(memory)}; GlobalMemoryStatusEx(&memory);
        Require(total + 2ull * 1024 * 1024 * 1024 < memory.ullAvailPhys, "Not enough available RAM; need cache plus 2 GiB spare");
        wchar_t session[128]; swprintf_s(session, L"Local\\MorrowindModern_%lu_%llu", GetCurrentProcessId(), GetTickCount64());
        std::wstring name = session;
        Handle data(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(total >> 32), DWORD(total), (name + L"_data").c_str()));
        Require(data.h != nullptr, "Cannot allocate resource cache");
        auto view = static_cast<uint8_t*>(MapViewOfFile(data.h, FILE_MAP_WRITE, 0, 0, 0));
        Require(view != nullptr, "Cannot map cache in x64 host");
        bool locked = false;
        if (lock) {
            SIZE_T minimum, maximum; GetProcessWorkingSetSize(GetCurrentProcess(), &minimum, &maximum);
            SIZE_T wanted = SIZE_T(total) + 64 * 1024 * 1024;
            Require(SetProcessWorkingSetSize(GetCurrentProcess(), wanted, wanted + 64 * 1024 * 1024), "Cannot enlarge working set for pinned RAM; use --allow-pageable to permit eviction");
            locked = VirtualLock(view, SIZE_T(total)) != FALSE;
            Require(locked, "Cannot pin full cache in physical RAM; use --allow-pageable to permit eviction");
        }
        wprintf(L"Loading %zu files, %.1f MiB into %ls RAM...\n", files.size(), payload / 1048576.0, locked ? L"locked physical" : L"pageable");
        unsigned lastPercent = 101;
        for (auto& s : files) {
            Handle file(CreateFileW(s.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            Require(file.h != INVALID_HANDLE_VALUE, "Cannot open a resource; cache canceled");
            BY_HANDLE_FILE_INFORMATION before; Require(GetFileInformationByHandle(file.h, &before), "Cannot validate resource");
            Require(FileTimeValue(before.ftLastWriteTime) == s.entry.writeTime && ((uint64_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow) == s.entry.size, "Resource changed during preload");
            uint64_t done = 0;
            while (done < s.entry.size) {
                DWORD n = 0; DWORD part = DWORD(std::min<uint64_t>(8 * 1024 * 1024, s.entry.size - done));
                Require(ReadFile(file.h, view + s.entry.offset + done, part, &n, nullptr) && n == part, "Resource read failed"); done += n;
            }
            unsigned percent = unsigned((s.entry.offset + s.entry.size) * 100 / total);
            if (percent / 10 != lastPercent / 10) { wprintf(L"%u%%\n", percent); lastPercent = percent; }
        }
        uint64_t indexSize = sizeof(CacheHeader) + files.size() * sizeof(CacheEntry);
        Handle index(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(indexSize), (name + L"_index").c_str()));
        Require(index.h != nullptr, "Cannot allocate cache index");
        auto header = static_cast<CacheHeader*>(MapViewOfFile(index.h, FILE_MAP_WRITE, 0, 0, 0));
        Require(header != nullptr, "Cannot map cache index");
        header->magic = kCacheMagic; header->version = kCacheVersion; header->count = uint32_t(files.size()); header->totalBytes = total; header->payloadBytes = payload;
        Require(dataRoot.wstring().size() < kPathChars, "Game path too long"); wcscpy_s(header->root, dataRoot.c_str());
        auto entries = reinterpret_cast<CacheEntry*>(header + 1);
        for (size_t i = 0; i < files.size(); ++i) entries[i] = files[i].entry;
        fs::create_directories(bin.parent_path() / L"logs");
        auto log = bin.parent_path() / L"logs" / (probe ? L"probe.log" : L"game.log");
        SetEnvironmentVariableW(L"MW_MODERN_SESSION", name.c_str()); SetEnvironmentVariableW(L"MW_MODERN_LOG", log.c_str());
        SetEnvironmentVariableW(L"MW_MODERN_EXPERIMENTAL", L"0"); SetEnvironmentVariableW(L"MW_MODERN_PROBE", probe ? L"1" : L"0");
        SetEnvironmentVariableW(L"MW_MODERN_TRACE", trace ? L"1" : L"0");
        const auto settingsPath = bin.parent_path() / L"settings.ini";
        Require(SetEnvironmentVariableW(L"MW_MODERN_SETTINGS", settingsPath.c_str()) != FALSE,
                "Cannot configure in-game display settings path");
        const wchar_t* modeName = screenMode == 0 ? L"fullscreen" : screenMode == 1 ? L"window" : L"borderless";
        SetEnvironmentVariableW(L"MW_MODERN_SCREEN_MODE", modeName);
        SetEnvironmentVariableW(L"MW_MODERN_WIDTH", std::to_wstring(width).c_str());
        SetEnvironmentVariableW(L"MW_MODERN_HEIGHT", std::to_wstring(height).c_str());
        SetEnvironmentVariableW(L"MW_MODERN_UI_SCALE", std::to_wstring(uiScale).c_str());
        // Steam's normal startup stub otherwise hands off to the client and
        // exits before the instrumented process reaches the game entry point.
        if (!probe) {
            SetEnvironmentVariableW(L"SteamAppId", L"22320");
            SetEnvironmentVariableW(L"SteamGameId", L"22320");
        }
        if (warmOnly) { wprintf(L"Cache built successfully.\n"); return 0; }
        HostGameAudioTarget audioTarget;
        const bool audioAvailable = !probe && audioTarget.Create(name.c_str());
        if (!probe && !audioAvailable) {
            fprintf(stderr, "Audio session guard unavailable: %s; native audio remains active.\n", audioTarget.Status());
        }
        auto runner = bin / L"runner.exe"; auto dll = bin / L"modernizer.dll";
        std::wstring command = L"\"" + runner.wstring() + L"\" \"" + target.wstring() + L"\" \"" + gameRoot.wstring() + L"\" \"" + dll.wstring() + L"\"";
        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Require(job.h && SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Cannot configure cache/game lifetime job");
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        Require(CreateProcessW(runner.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, gameRoot.c_str(), &startup, &child), "Cannot launch x86 runner");
        if (!AssignProcessToJobObject(job.h, child.hProcess)) {
            TerminateProcess(child.hProcess, 1); CloseHandle(child.hThread); CloseHandle(child.hProcess);
            throw std::runtime_error("Cannot keep game lifetime tied to RAM cache owner");
        }
        ResumeThread(child.hThread); CloseHandle(child.hThread); wprintf(L"Cache ready. %ls\n", L"Original cell loading enabled.");
        // Keep mappings and locked pages alive until the game exits.
        WaitForRunner(child.hProcess, audioAvailable ? &audioTarget : nullptr);
        DWORD status; GetExitCodeProcess(child.hProcess, &status); CloseHandle(child.hProcess);
        UnmapViewOfFile(header); if (locked) VirtualUnlock(view, SIZE_T(total)); UnmapViewOfFile(view);
        wprintf(L"Game/helper exit code: %lu\n", status); return int(status);
    } catch (const std::exception& error) {
        fprintf(stderr, "ERROR: %s (Win32 %lu)\n", error.what(), GetLastError());
        if (g_interactive) MessageBoxA(nullptr, error.what(), "Morrowind Modernizer", MB_OK | MB_ICONERROR);
        return 1;
    }
}
