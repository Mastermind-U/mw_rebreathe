// Native Morrowind 1.6.1820 UI viewport scaling. Independent implementation.
// Engine offsets were verified against the local EXE and primary MGE/MWSE research.
#include "ui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
static_assert(sizeof(void*) == 4, "Morrowind UI hooks require x86");
constexpr DWORD kConfigure = 0xF2A0;
constexpr DWORD kMouseBounds = 0x8740;
constexpr DWORD kMaster = 0x3C67DC;
constexpr DWORD kMinimumImageSize = 0x3C67E0;
constexpr DWORD kUiWidth = 0x78, kUiHeight = 0x7C, kRenderer = 0x30, kMouse = 0x50;
constexpr int kMinimumWidth = 640, kMinimumHeight = 480;
static unsigned char* g_image = nullptr;
static void* g_videoSameMode = nullptr;
static void* g_videoApplyMode = nullptr;
static double g_requestedScale = 1.0;
static char g_status[256] = "UI scale 100% (native).";
using ConfigureFunction = void (__thiscall*)(void*, int);
using BoundsFunction = void (__thiscall*)(void*, int, int, int, int);
using DimensionFunction = int (__thiscall*)(void*);

struct Size { int width, height; double scale; };
Size LogicalSize(int width, int height, double requested) {
    const double limit = (std::min)(static_cast<double>(width) / kMinimumWidth,
                                     static_cast<double>(height) / kMinimumHeight);
    const double effective = (std::max)(1.0, (std::min)(requested, limit));
    return {static_cast<int>(width / effective), static_cast<int>(height / effective), effective};
}

template<typename T> T Read(const void* object, size_t offset) {
    T result = {};
    std::memcpy(&result, static_cast<const unsigned char*>(object) + offset, sizeof(result));
    return result;
}
template<typename T> void Write(void* object, size_t offset, const T& value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}

// The native Video labels contain physical pixels. WorldController +78/+7C
// deliberately contains logical UI pixels after scaling, so these two Video
// consumers must read the renderer rather than those UI fields.
unsigned __int64 __cdecl PhysicalVideoDimensions() {
    void* world = Read<void*>(g_image, kMaster);
    if (!world) return 0;
    void* renderer = Read<void*>(world, kRenderer);
    if (!renderer) return 0;
    const int width = reinterpret_cast<DimensionFunction>(g_image + 0x2AC030)(renderer);
    const int height = reinterpret_cast<DimensionFunction>(g_image + 0x2AC040)(renderer);
    if (width < kMinimumWidth || height < kMinimumHeight || width > 16384 || height > 16384) return 0;
    return (static_cast<unsigned __int64>(static_cast<unsigned>(height)) << 32) |
        static_cast<unsigned>(width);
}

__declspec(naked) void VideoPhysicalPreselection() {
    __asm {
        call PhysicalVideoDimensions
        mov ecx, edx
        mov edx, eax
        mov eax, g_image
        mov eax, [eax + 003C67DCh]
        ret
    }
}

__declspec(naked) void VideoPhysicalComparison() {
    __asm {
        // This is entered by JMP while the four sscanf arguments are still
        // on the native stack: selected W=ESP+18h, selected H=ESP+1Ch.
        call PhysicalVideoDimensions
        cmp edx, [esp + 1Ch]
        jne changed
        cmp eax, [esp + 18h]
        jne changed
        add esp, 10h
        jmp dword ptr [g_videoSameMode]
    changed:
        mov eax, g_image
        mov eax, [eax + 003C67DCh]
        mov ecx, [esp + 1Ch]
        add esp, 10h
        jmp dword ptr [g_videoApplyMode]
    }
}

bool ParseScale(const char* text, double& scale) {
    if (!text || !*text) { scale = 1.0; return true; }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end || !std::isfinite(value) || value < 1.0 || value > 4.0) return false;
    scale = value;
    return true;
}

void SetLogicalMouseBounds(void* mouse, int width, int height) {
    const int halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
    reinterpret_cast<BoundsFunction>(g_image + kMouseBounds)(mouse, -halfWidth, -halfHeight, halfWidth, halfHeight);
}

void RecordApplication(int physicalWidth, int physicalHeight, const Size& logical) {
    sprintf_s(g_status, "UI scale %.0f%%: render %dx%d, logical UI %dx%d; native mouse/raycast aligned.",
              logical.scale * 100.0, physicalWidth, physicalHeight, logical.width, logical.height);
    wchar_t path[32768] = {};
    DWORD count = GetEnvironmentVariableW(L"MW_MODERN_LOG", path, 32768);
    if (count == 0 || count >= 32768) return;
    wchar_t* name = nullptr;
    for (wchar_t* current = path; *current; ++current) if (*current == L'\\' || *current == L'/') name = current + 1;
    if (!name || static_cast<size_t>(name - path) + 13 >= 32768) return;
    wcscpy_s(name, 32768 - (name - path), L"ui-scale.log");
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, g_status, static_cast<DWORD>(std::strlen(g_status)), &written, nullptr);
    WriteFile(file, "\r\n", 2, &written, nullptr);
    CloseHandle(file);
}

void __fastcall ConfigureScaledUi(void* world, void*, int originalWidth) {
    const auto configure = reinterpret_cast<ConfigureFunction>(g_image + kConfigure);
    if (!world) { configure(world, originalWidth); return; }
    void* renderer = Read<void*>(world, kRenderer);
    void* mouse = Read<void*>(world, kMouse);
    if (!renderer || !mouse) { configure(world, originalWidth); return; }
    const int physicalWidth = reinterpret_cast<DimensionFunction>(g_image + 0x2AC030)(renderer);
    const int physicalHeight = reinterpret_cast<DimensionFunction>(g_image + 0x2AC040)(renderer);
    if (physicalWidth < kMinimumWidth || physicalHeight < kMinimumHeight || physicalWidth > 16384 || physicalHeight > 16384) {
        configure(world, originalWidth); return;
    }
    const Size logical = LogicalSize(physicalWidth, physicalHeight, g_requestedScale);
    Write(world, kUiWidth, logical.width);
    Write(world, kUiHeight, logical.height);
    configure(world, logical.width);
    SetLogicalMouseBounds(mouse, logical.width, logical.height);
    RecordApplication(physicalWidth, physicalHeight, logical);
}

void __fastcall ConfigureScaledMouse(void* mouse, void*, int left, int top, int right, int bottom) {
    void* world = Read<void*>(g_image, kMaster);
    if (world && mouse && Read<void*>(world, kMouse) == mouse) {
        const int width = Read<int>(world, kUiWidth), height = Read<int>(world, kUiHeight);
        if (width >= kMinimumWidth && height >= kMinimumHeight && width <= 16384 && height <= 16384) {
            SetLogicalMouseBounds(mouse, width, height); return;
        }
    }
    reinterpret_cast<BoundsFunction>(g_image + kMouseBounds)(mouse, left, top, right, bottom);
}

struct Signature { DWORD rva; const char* data; size_t size; };
bool MatchesSignatures(const unsigned char* image, size_t size) {
    if (!image || size < kMinimumImageSize) return false;
    const Signature signatures[] = {
        {0xE55E, "\xE8\x3D\x0D\x00\x00", 5},
        {0x29B10, "\xE8\x8B\x57\xFE\xFF", 5},
        {0x29B30, "\xE8\x0B\xEC\xFD\xFF", 5},
        {0x2F5157, "\x8B\x06\xFF\x50\x54\x8B\x16\x8B\xCE\x8B\xF8\xFF\x52\x58", 14},
        {kConfigure, "\x83\xEC\x28\xDB\x44\x24\x2C\x56", 8},
        {0xF4E4, "\xC2\x04\x00", 3},
        {0xF300, "\x8B\x4E\x50", 3},
        {0xF315, "\x8B\x46\x30\x8B\x4E\x7C", 6},
        {kMouseBounds, "\xDB\x44\x24\x04\xD9\x59\x24", 7},
        {0x875C, "\xC2\x10\x00", 3},
        {0x2AC030, "\x8B\x81\x44\x05\x00\x00\x8B\x00\xC3", 9},
        {0x2AC040, "\x8B\x81\x44\x05\x00\x00\x8B\x40\x04\xC3", 10},
        {0x29B0A, "\x8B\x0D\xDC\x67\x7C\x00", 6},
        {0x29AE9, "\x89\x4A\x78", 3},
        {0x29AF5, "\x89\x41\x7C", 3},
        {0x23759D, "\xA1\xDC\x67\x7C\x00\x8B\x48\x7C\x8B\x50\x78", 11},
        {0x238464, "\xA1\xDC\x67\x7C\x00\x8B\x4C\x24\x1C\x8B\x50\x7C\x83\xC4\x10\x3B\xCA\x75\x11\x8B\x54\x24\x08\x3B\x50\x78\x75\x08", 28},
        {0x238480, "\x5F\x32\xC0\x5E\x83\xC4\x08\xC3", 8},
        {0x238488, "\x89\x48\x7C\xA1\xDC\x67\x7C\x00", 8},
    };
    for (const auto& signature : signatures) if (std::memcmp(image + signature.rva, signature.data, signature.size) != 0) return false;
    return true;
}

struct Patch { DWORD rva; size_t size; unsigned char before[14], after[14]; };
void MakeCallPatch(Patch& patch, DWORD rva, const void* target) {
    patch.rva = rva; patch.size = 5;
    std::memcpy(patch.before, g_image + rva, 5);
    patch.after[0] = 0xE8;
    const DWORD displacement = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(target) - reinterpret_cast<ULONG_PTR>(g_image + rva + 5));
    std::memcpy(patch.after + 1, &displacement, sizeof(displacement));
}

bool WritePatch(const Patch& patch, bool install) {
    DWORD protection = 0, ignored = 0;
    auto* address = g_image + patch.rva;
    if (!VirtualProtect(address, patch.size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(address, install ? patch.after : patch.before, patch.size);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), address, patch.size) != FALSE;
    const bool restored = VirtualProtect(address, patch.size, protection, &ignored) != FALSE;
    return flushed && restored;
}
}

bool InstallUiScaleHooks(HMODULE gameModule) {
    char text[64] = {};
    const DWORD length = GetEnvironmentVariableA("MW_MODERN_UI_SCALE", text, sizeof(text));
    if (length >= sizeof(text) || !ParseScale(text, g_requestedScale)) return false;
    if (g_requestedScale == 1.0) return true;
    auto* image = reinterpret_cast<unsigned char*>(gameModule);
    if (!image) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.ImageBase != 0x400000 ||
        !MatchesSignatures(image, nt->OptionalHeader.SizeOfImage)) return false;
    g_image = image;
    g_videoSameMode = image + 0x238480;
    g_videoApplyMode = image + 0x238488;
    Patch patches[6] = {};
    MakeCallPatch(patches[0], 0xE55E, reinterpret_cast<const void*>(&ConfigureScaledUi));
    MakeCallPatch(patches[1], 0x29B10, reinterpret_cast<const void*>(&ConfigureScaledUi));
    MakeCallPatch(patches[2], 0x29B30, reinterpret_cast<const void*>(&ConfigureScaledMouse));
    Patch& picking = patches[3];
    picking.rva = 0x2F5157; picking.size = 14;
    std::memcpy(picking.before, image + picking.rva, picking.size);
    // Fetch logical viewport W/H, keeping EDI=width and EAX=height as the native
    // renderer getter sequence did. Remaining normalization code is unchanged.
    picking.after[0] = 0xA1;
    const DWORD masterAddress = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(image + kMaster));
    std::memcpy(picking.after + 1, &masterAddress, sizeof(masterAddress));
    const unsigned char loadDimensions[] = {0x8B,0x78,0x78,0x8B,0x40,0x7C,0x90,0x90,0x90};
    std::memcpy(picking.after + 5, loadDimensions, sizeof(loadDimensions));
    Patch& preselection = patches[4];
    MakeCallPatch(preselection, 0x23759D, reinterpret_cast<const void*>(&VideoPhysicalPreselection));
    preselection.size = 11;
    std::memcpy(preselection.before, image + preselection.rva, preselection.size);
    std::memset(preselection.after + 5, 0x90, preselection.size - 5);
    MakeCallPatch(patches[5], 0x238464, reinterpret_cast<const void*>(&VideoPhysicalComparison));
    patches[5].after[0] = 0xE9;
    // Each call restores protection immediately: two mutations share one page.
    for (size_t i = 0; i < 6; ++i) {
        if (!WritePatch(patches[i], true)) {
            for (size_t j = 0; j <= i; ++j) WritePatch(patches[j], false);
            g_image = nullptr; return false;
        }
    }
    sprintf_s(g_status, "UI scale %.0f%% armed; Video highlights and same-mode checks use physical renderer dimensions.", g_requestedScale * 100.0);
    return true;
}

const char* UiScaleHookStatus() { return g_status; }
