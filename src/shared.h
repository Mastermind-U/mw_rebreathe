#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <iterator>

constexpr uint32_t kCacheMagic = 0x31434D4D;
constexpr uint32_t kCacheVersion = 1;
constexpr size_t kPathChars = 768;
struct CacheHeader {
    uint32_t magic, version, count, reserved;
    uint64_t totalBytes, payloadBytes;
    wchar_t root[kPathChars];
};
struct CacheEntry {
    uint64_t offset, size, writeTime;
    wchar_t relative[kPathChars];
};
static_assert(sizeof(CacheHeader) == 1568, "x86/x64 manifest ABI");
static_assert(sizeof(CacheEntry) == 1560, "x86/x64 entry ABI");
struct CacheCounters { uint64_t reads, bytes, opens, fallbacks; };

// Fixed-width POD shared only by the x86 crash target and its healthy runner.
// Publish state=2 with InterlockedExchange after the remaining fields are set.
constexpr uint32_t kCrashMagic = 0x3152434D;
constexpr uint32_t kCrashVersion = 1;
constexpr DWORD kCrashWaitMs = 15000;
struct CrashRequest {
    uint32_t magic, version;
    volatile LONG state;
    uint32_t processId, threadId, exceptionPointers;
    uint32_t exceptionCode, exceptionAddress, instructionPointer, stackPointer;
    volatile LONG dumpSucceeded;
    uint32_t dumpError;
};
static_assert(sizeof(CrashRequest) == 48, "x86/x64 crash protocol ABI");
inline uint64_t FileTimeValue(FILETIME t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) { if (c == L'/') c = L'\\'; c = towlower(c); }
    return s;
}
inline std::wstring FullPath(const std::wstring& s) {
    std::vector<wchar_t> b(32768);
    DWORD n = GetFullPathNameW(s.c_str(), DWORD(b.size()), b.data(), nullptr);
    return n && n < b.size() ? std::wstring(b.data(), n) : L"";
}
inline std::wstring Environment(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return L"";
    std::vector<wchar_t> b(n);
    GetEnvironmentVariableW(name, b.data(), n);
    return b.data();
}
