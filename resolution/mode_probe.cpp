// Read-only display-mode probe. Uses the system Direct3D 8 runtime and creates
// no rendering device, window or swap chain. No DirectX SDK is required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace {
struct DisplayMode { UINT width, height, refreshRate; DWORD format; };
static_assert(sizeof(DisplayMode) == 16, "Direct3D 8 mode ABI");
using CreateD3D = void* (WINAPI*)(UINT);
using GetAdapterCount = UINT (WINAPI*)(void*);
using GetModeCount = UINT (WINAPI*)(void*, UINT);
using EnumerateMode = HRESULT (WINAPI*)(void*, UINT, UINT, DisplayMode*);
using GetDesktopMode = HRESULT (WINAPI*)(void*, UINT, DisplayMode*);
using CheckDeviceType = HRESULT (WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, BOOL);
using Release = ULONG (WINAPI*)(void*);

template<class Function> Function Method(void* object, size_t index) {
    return reinterpret_cast<Function>((*static_cast<void***>(object))[index]);
}
struct Group {
    std::set<UINT> refreshRates;
    std::set<DWORD> formats;
    bool hardware32 = false;
};
template<class Value> void PrintNumbers(const std::set<Value>& values) {
    bool first = true;
    std::printf("[");
    for (const auto value : values) {
        std::printf("%s%lu", first ? "" : ",", static_cast<unsigned long>(value));
        first = false;
    }
    std::printf("]");
}
}

int main() {
    const auto runtime = LoadLibraryExW(L"d3d8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) {
        std::fprintf(stderr, "System d3d8.dll unavailable: %lu\n", GetLastError());
        return 1;
    }
    const auto create = reinterpret_cast<CreateD3D>(GetProcAddress(runtime, "Direct3DCreate8"));
    if (!create) { FreeLibrary(runtime); return 1; }
    UINT sdk = 220; // DirectX 8.1; retain an 8.0 fallback for older runtimes.
    void* d3d = create(sdk);
    if (!d3d) { sdk = 120; d3d = create(sdk); }
    if (!d3d) {
        std::fprintf(stderr, "Direct3DCreate8 failed for SDK versions 220 and 120.\n");
        FreeLibrary(runtime);
        return 1;
    }

    const auto adapterCount = Method<GetAdapterCount>(d3d, 4)(d3d);
    DEVMODEW desktop{};
    desktop.dmSize = sizeof(desktop);
    const BOOL gotDesktop = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &desktop);
    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::printf("{\n  \"observedUtc\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\",\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::printf("  \"probeArchitecture\":\"x86\",\n  \"systemDirect3D8Only\":true,\n"
                "  \"renderingDeviceCreated\":false,\n  \"sdkVersion\":%u,\n", sdk);
    std::printf("  \"eligibilityScope\":\"observed game size/depth/aspect filter plus HAL fullscreen CheckDeviceType; UI selection and rendering are not exercised\",\n");
    if (gotDesktop) {
        std::printf("  \"desktop\":{\"width\":%lu,\"height\":%lu,\"bitsPerPixel\":%lu,\"refreshRate\":%lu},\n",
                    desktop.dmPelsWidth, desktop.dmPelsHeight, desktop.dmBitsPerPel, desktop.dmDisplayFrequency);
    } else {
        std::printf("  \"desktop\":null,\n");
    }
    std::printf("  \"adapters\":[\n");
    for (UINT adapter = 0; adapter < adapterCount; ++adapter) {
        const UINT count = Method<GetModeCount>(d3d, 6)(d3d, adapter);
        std::map<std::pair<UINT, UINT>, Group> groups;
        UINT enumErrors = 0;
        for (UINT i = 0; i < count; ++i) {
            DisplayMode mode{};
            if (FAILED(Method<EnumerateMode>(d3d, 7)(d3d, adapter, i, &mode))) {
                ++enumErrors;
                continue;
            }
            auto& group = groups[{mode.width, mode.height}];
            group.refreshRates.insert(mode.refreshRate);
            group.formats.insert(mode.format);
            // D3DFMT_A8R8G8B8=21, D3DFMT_X8R8G8B8=22, D3DDEVTYPE_HAL=1.
            if ((mode.format == 21 || mode.format == 22) &&
                SUCCEEDED(Method<CheckDeviceType>(d3d, 9)(d3d, adapter, 1,
                    mode.format, mode.format, FALSE))) {
                group.hardware32 = true;
            }
        }
        DisplayMode current{};
        const HRESULT currentResult = Method<GetDesktopMode>(d3d, 8)(d3d, adapter, &current);
        std::printf("    {\"adapter\":%u,\"rawModeCount\":%u,\"enumerationErrors\":%u,\"currentMode\":",
                    adapter, count, enumErrors);
        if (SUCCEEDED(currentResult)) {
            std::printf("{\"width\":%u,\"height\":%u,\"refreshRate\":%u,\"format\":%lu}",
                        current.width, current.height, current.refreshRate, current.format);
        } else {
            std::printf("null");
        }
        std::printf(",\"modes\":[\n");
        bool first = true;
        for (const auto& item : groups) {
            const auto width = item.first.first, height = item.first.second;
            const auto& group = item.second;
            const bool patched = group.hardware32 && width >= 640 && height >= 480;
            const bool vanilla = patched && UINT64(height) * 4 == UINT64(width) * 3;
            std::printf("%s      {\"width\":%u,\"height\":%u,\"hardware32BitFullscreen\":%s,"
                        "\"vanillaEligible\":%s,\"patchedEligible\":%s,\"refreshRates\":",
                        first ? "" : ",\n", width, height, group.hardware32 ? "true" : "false",
                        vanilla ? "true" : "false", patched ? "true" : "false");
            PrintNumbers(group.refreshRates);
            std::printf(",\"formats\":");
            PrintNumbers(group.formats);
            std::printf("}");
            first = false;
        }
        std::printf("\n    ]}%s\n", adapter + 1 < adapterCount ? "," : "");
    }
    std::printf("  ]\n}\n");
    Method<Release>(d3d, 2)(d3d);
    FreeLibrary(runtime);
    return 0;
}
