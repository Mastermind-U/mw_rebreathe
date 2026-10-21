#include "screenshot.h"
#include <cstdint>
#include <cstring>

static_assert(sizeof(void*) == 4, "Native screenshots require x86");
namespace {
constexpr uint32_t CallRva = 0x1B098, ScreenshotRva = 0x2E620;
constexpr size_t TargetSize = 0x84;
unsigned char* g_image = nullptr;
const char* g_status = "Screenshot physical-dimension hook not installed";
void (*g_log)(const char*) = nullptr;
unsigned g_lastSkip = 0;
constexpr const char* ReadyStatus = "Native screenshots use physical renderer dimensions; UI render-target unchanged";
using ScreenshotFunction = void(__thiscall*)(void*);
using DimensionFunction = unsigned(__thiscall*)(void*);

template<typename T> T Read(const void* object, size_t offset) {
    T result{}; std::memcpy(&result, static_cast<const unsigned char*>(object) + offset, sizeof(result)); return result;
}
// Screenshot redraws the scene before reading the backbuffer. The native shadow
// pass assumes its scene node exists, including at the initial menu where the
// manager may already be enabled while that node is still null.
unsigned ScreenshotEligibility() {
    __try {
        void* world = Read<void*>(g_image, 0x3C67DC);
        if (!world || !Read<void*>(world, 0x30)) return 1;
        void* shadows = Read<void*>(world, 0x2DC);
        if (shadows && Read<unsigned char>(shadows, 0x0C) && !Read<void*>(shadows, 0x68)) return 2;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
void __fastcall PhysicalScreenshot(void* target, void*) {
    const DWORD incomingError = GetLastError();
    const unsigned skip = ScreenshotEligibility();
    if (skip) {
        g_status = skip == 2 ? "Screenshot skipped: native enabled shadow scene is not ready" :
            "Screenshot skipped: native world/renderer is not live";
        if (g_log && skip != g_lastSkip) g_log(g_status);
        g_lastSkip = skip;
        SetLastError(incomingError);
        return;
    }
    g_lastSkip = 0;
    g_status = ReadyStatus;
    const auto original = reinterpret_cast<ScreenshotFunction>(g_image + ScreenshotRva);
    if (!target) { SetLastError(incomingError); return; }
    void* renderer = Read<void*>(target, 4);
    if (!renderer) { original(target); return; }
    const unsigned width = reinterpret_cast<DimensionFunction>(g_image + 0x2AC030)(renderer);
    const unsigned height = reinterpret_cast<DimensionFunction>(g_image + 0x2AC040)(renderer);
    if (width < 640 || height < 480 || width > 16384 || height > 16384) { original(target); return; }
    // Borrow existing pointers as raw POD, without constructing/destructing
    // the engine's refcounted RenderTarget. The native screenshot only reads
    // this+4, this+38 and this+3C. Real UI/render-target fields stay untouched.
    alignas(4) unsigned char view[TargetSize];
    std::memcpy(view, target, sizeof(view));
    std::memcpy(view + 0x38, &width, sizeof(width));
    std::memcpy(view + 0x3C, &height, sizeof(height));
    original(view);
}

struct Guard { uint32_t rva; const unsigned char* bytes; size_t size; };
constexpr unsigned char Call[]{0xE8,0x83,0x35,0x01,0x00};
constexpr unsigned char Caller[]{0x8B,0x15,0xDC,0x67,0x7C,0x00,0x8D,0x8A,0xA8,0x01,0x00,0x00};
constexpr unsigned char Prefix[]{0x6A,0xFF,0x68,0xA4,0x94,0x72,0x00,0x64,0xA1,0x00,0x00,0x00,0x00,0x50};
constexpr unsigned char Renderer[]{0x8B,0x7E,0x04};
constexpr unsigned char SurfaceSize[]{0x8B,0x56,0x38,0x50,0x8B,0x46,0x3C};
constexpr unsigned char PixelSize[]{0x8B,0x56,0x3C,0x8B,0x4E,0x38};
constexpr unsigned char HeightC[]{0x8B,0x4E,0x3C};
constexpr unsigned char HeightB[]{0x8B,0x5E,0x3C};
constexpr unsigned char HeightA[]{0x8B,0x46,0x3C};
constexpr unsigned char WidthGetter[]{0x8B,0x81,0x44,0x05,0x00,0x00,0x8B,0x00,0xC3};
constexpr unsigned char HeightGetter[]{0x8B,0x81,0x44,0x05,0x00,0x00,0x8B,0x40,0x04,0xC3};
constexpr unsigned char PrepareScene[]{0xE8,0xA6,0xDD,0xFE,0xFF};
constexpr unsigned char ShadowDispatch[]{0x8B,0x15,0xDC,0x67,0x7C,0x00,0x8B,0x8A,0xDC,0x02,0x00,0x00,
    0x85,0xC9,0x74,0x0C,0x8A,0x41,0x0C,0x84,0xC0,0x74,0x05,0xE8,0xFF,0x8B,0x01,0x00};
constexpr unsigned char ShadowDereference[]{0x8B,0x4E,0x68,0x8B,0x01,0xFF,0x90,0x90,0x00,0x00,0x00};
const Guard Guards[]{
    {0x1B08C,Caller,sizeof(Caller)}, {ScreenshotRva,Prefix,sizeof(Prefix)},
    {0x2E655,PrepareScene,sizeof(PrepareScene)}, {0x1C4F5,ShadowDispatch,sizeof(ShadowDispatch)},
    {0x3527D,ShadowDereference,sizeof(ShadowDereference)},
    {0x2E71C,Renderer,sizeof(Renderer)}, {0x2E768,SurfaceSize,sizeof(SurfaceSize)},
    {0x2E82A,PixelSize,sizeof(PixelSize)}, {0x2E84F,HeightC,sizeof(HeightC)},
    {0x2E899,HeightB,sizeof(HeightB)}, {0x2E945,HeightA,sizeof(HeightA)},
    {0x2AC030,WidthGetter,sizeof(WidthGetter)}, {0x2AC040,HeightGetter,sizeof(HeightGetter)}};
void HookBytes(unsigned char* image, unsigned char (&bytes)[5]) {
    bytes[0] = 0xE8;
    const uint32_t displacement = uint32_t(reinterpret_cast<uintptr_t>(&PhysicalScreenshot)) -
        uint32_t(reinterpret_cast<uintptr_t>(image + CallRva + 5));
    std::memcpy(bytes + 1, &displacement, sizeof(displacement));
}
bool Executable(const IMAGE_NT_HEADERS32* nt, uint32_t rva, size_t length) {
    const auto sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint64_t start = sections[i].VirtualAddress;
        if (rva >= start && uint64_t(rva) + length <= start + sections[i].Misc.VirtualSize &&
            (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) return true;
    }
    return false;
}
bool Supported(HMODULE module) {
    __try {
        if (!module) return false;
        const auto image = reinterpret_cast<unsigned char*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x100000) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.TimeDateStamp != 0x72456542 || nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96 ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.ImageBase != 0x400000 ||
            nt->OptionalHeader.SizeOfImage != 0x43A000) return false;
        for (const auto& guard : Guards) {
            if (!Executable(nt, guard.rva, guard.size) || std::memcmp(image + guard.rva, guard.bytes, guard.size)) return false;
        }
        unsigned char hooked[5]; HookBytes(image, hooked);
        return Executable(nt, CallRva, sizeof(Call)) &&
            (!std::memcmp(image + CallRva, Call, sizeof(Call)) || !std::memcmp(image + CallRva, hooked, sizeof(hooked)));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

bool InstallScreenshotHooks(HMODULE module) {
    if (!Supported(module)) { g_status = "Screenshot physical dimensions: unsupported executable/signature"; return false; }
    auto image = reinterpret_cast<unsigned char*>(module);
    unsigned char bytes[5]; HookBytes(image, bytes);
    g_image = image;
    if (std::memcmp(image + CallRva, bytes, sizeof(bytes))) {
        DWORD old = 0, ignored = 0;
        if (!VirtualProtect(image + CallRva, sizeof(bytes), PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(image + CallRva, bytes, sizeof(bytes));
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), image + CallRva, sizeof(bytes)) != FALSE;
        const bool restored = VirtualProtect(image + CallRva, sizeof(bytes), old, &ignored) != FALSE;
        if (!flushed || !restored) {
            if (VirtualProtect(image + CallRva, sizeof(bytes), PAGE_EXECUTE_READWRITE, &ignored)) {
                std::memcpy(image + CallRva, Call, sizeof(Call));
                FlushInstructionCache(GetCurrentProcess(), image + CallRva, sizeof(Call));
                VirtualProtect(image + CallRva, sizeof(Call), old, &ignored);
            }
            g_status = "Screenshot physical dimensions: patch API failure";
            return false;
        }
    }
    g_status = ReadyStatus;
    return true;
}
const char* ScreenshotHookStatus() { return g_status; }
void SetScreenshotLogCallback(void (*callback)(const char*)) { g_log = callback; }
