#include "screenshot.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void* g_originalTarget = nullptr;
bool g_shadow = false;
unsigned g_calls = 0;
std::array<unsigned char, 0x84> g_received{};
uintptr_t g_beforeSp = 0, g_afterSp = 0;
uint32_t g_ebx = 0, g_esi = 0, g_edi = 0, g_ebp = 0;
constexpr DWORD IncomingError = 0x13572468, OutgoingError = 0x24681357;
std::array<unsigned char, 0x374> g_world{};
std::array<unsigned char, 0x70> g_shadowManager{};
unsigned g_logCalls = 0;
void LogScreenshot(const char*) { ++g_logCalls; SetLastError(0xBADBAD); }
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void WriteMemory(void* target, const void* bytes, size_t size) {
    DWORD old = 0, ignored = 0;
    Require(VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old) != FALSE, "Cannot modify private image");
    std::memcpy(target, bytes, size);
    Require(FlushInstructionCache(GetCurrentProcess(), target, size) != FALSE, "Cannot flush private image");
    Require(VirtualProtect(target, size, old, &ignored) != FALSE, "Cannot restore image protection");
}
void SetWorld(unsigned char* image, bool present, bool renderer, bool shadowsEnabled, bool shadowNode) {
    g_world.fill(0); g_shadowManager.fill(0);
    void* world = present ? g_world.data() : nullptr;
    void* render = renderer ? g_world.data() : nullptr;
    void* shadows = g_shadowManager.data();
    void* node = shadowNode ? g_world.data() : nullptr;
    std::memcpy(g_world.data() + 0x30, &render, 4);
    std::memcpy(g_world.data() + 0x2DC, &shadows, 4);
    g_shadowManager[0x0C] = shadowsEnabled;
    std::memcpy(g_shadowManager.data() + 0x68, &node, 4);
    WriteMemory(image + 0x3C67DC, &world, 4);
}
void __fastcall Capture(void* target, void*) {
    ++g_calls;
    if (GetLastError() != IncomingError) g_calls = 100000;
    g_shadow = target != g_originalTarget;
    std::memcpy(g_received.data(), target, g_received.size());
    SetLastError(OutgoingError);
}
__declspec(naked) void __stdcall Invoke(void*, void*) {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax, dword ptr [esp + 20]
        mov ecx, dword ptr [esp + 24]
        mov ebx, 0x13579BDF
        mov esi, 0x2468ACE0
        mov edi, 0x11223344
        mov ebp, 0x55667788
        mov g_beforeSp, esp
        call eax
        mov g_afterSp, esp
        mov g_ebx, ebx
        mov g_esi, esi
        mov g_edi, edi
        mov g_ebp, ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 8
    }
}
void Check(unsigned char* image, unsigned width, unsigned height, unsigned logicalWidth, unsigned logicalHeight, bool rendererPresent = true) {
    SetWorld(image, true, true, true, true);
    std::array<unsigned char, 0x548> renderer{};
    std::array<unsigned, 2> info{width,height};
    const auto infoPointer = info.data();
    std::memcpy(renderer.data() + 0x544, &infoPointer, sizeof(infoPointer));
    std::array<unsigned char, 0x84> target{};
    for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<unsigned char>(i ^ 0x5A);
    void* rendererPointer = rendererPresent ? renderer.data() : nullptr;
    std::memcpy(target.data() + 4, &rendererPointer, sizeof(rendererPointer));
    std::memcpy(target.data() + 0x38, &logicalWidth, sizeof(logicalWidth));
    std::memcpy(target.data() + 0x3C, &logicalHeight, sizeof(logicalHeight));
    const auto before = target;
    auto expected = target;
    const bool valid = rendererPresent && width >= 640 && height >= 480 && width <= 16384 && height <= 16384;
    if (valid) {
        std::memcpy(expected.data() + 0x38, &width, sizeof(width));
        std::memcpy(expected.data() + 0x3C, &height, sizeof(height));
    }
    g_originalTarget = target.data(); g_calls = 0;
    SetLastError(IncomingError);
    Invoke(image + 0x1B098, target.data());
    Require(g_calls == 1 && g_received == expected && g_shadow == valid, "Native screenshot received wrong borrowed view/dimensions");
    Require(target == before, "Live render target or borrowed refcount pointers were mutated");
    Require(GetLastError() == OutgoingError && g_beforeSp == g_afterSp, "Screenshot wrapper changed LastError/stack");
    Require(g_ebx == 0x13579BDF && g_esi == 0x2468ACE0 && g_edi == 0x11223344 && g_ebp == 0x55667788,
        "Screenshot wrapper changed nonvolatile registers");
}
void CheckEligibility(unsigned char* image, bool world, bool renderer, bool enabled, bool node, bool eligible) {
    SetWorld(image,world,renderer,enabled,node);
    std::array<unsigned char,0x84> target{};
    const auto before=target;
    g_originalTarget=target.data(); g_calls=0;
    SetLastError(IncomingError);
    Invoke(image+0x1B098,target.data());
    Require(g_calls==(eligible?1u:0u),"Screenshot eligibility incorrectly dispatched native scene rendering");
    Require(target==before,"Eligibility mutated native target");
    Require(GetLastError()==(eligible?OutgoingError:IncomingError),"Eligibility changed LastError");
    Require(g_beforeSp==g_afterSp && g_ebx==0x13579BDF && g_esi==0x2468ACE0 && g_edi==0x11223344 && g_ebp==0x55667788,
        "Eligibility changed native stack/nonvolatile registers");
}
}
int wmain(int argc, wchar_t** argv) {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr; unsigned char* image = nullptr;
    try {
        Require(argc == 2, "Pass original Morrowind.exe");
        file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Cannot open original game");
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
        Require(mapping != nullptr, "Cannot map private image");
        image = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        Require(image != nullptr, "Cannot view private image");
        Require(!InstallScreenshotHooks(nullptr) && !InstallScreenshotHooks(reinterpret_cast<HMODULE>(0x30)), "Invalid image accepted");
        unsigned rejected = 0;
        for (uint32_t rva : {0x1B08Cu,0x1B098u,0x2E620u,0x2E655u,0x1C4F5u,0x3527Du,0x2E71Cu,0x2E768u,0x2E82Au,0x2E84Fu,0x2E899u,0x2E945u,0x2AC030u,0x2AC040u}) {
            const unsigned char original = image[rva], bad = static_cast<unsigned char>(original ^ 0x5A);
            WriteMemory(image + rva, &bad, 1);
            std::array<unsigned char, 5> before{}; std::memcpy(before.data(), image + 0x1B098, before.size());
            Require(!InstallScreenshotHooks(reinterpret_cast<HMODULE>(image)), "Corrupt screenshot guard accepted");
            Require(!std::memcmp(before.data(), image + 0x1B098, before.size()), "Rejected image partially modified");
            WriteMemory(image + rva, &original, 1); ++rejected;
        }
        Require(InstallScreenshotHooks(reinterpret_cast<HMODULE>(image)), ScreenshotHookStatus());
        Require(InstallScreenshotHooks(reinterpret_cast<HMODULE>(image)), "Idempotent installation rejected");
        unsigned char jump[5]{0xE9};
        const uint32_t delta = uint32_t(reinterpret_cast<uintptr_t>(&Capture)) - uint32_t(reinterpret_cast<uintptr_t>(image + 0x2E625));
        std::memcpy(jump + 1, &delta, 4); WriteMemory(image + 0x2E620, jump, sizeof(jump));
        const unsigned char ret = 0xC3; WriteMemory(image + 0x1B09D, &ret, 1);
        Check(image,2880,1800,1440,900); Check(image,1920,1080,1280,720);
        Check(image,2560,1600,1706,1066); Check(image,1280,720,854,480);
        Check(image,3840,2160,1920,1080); Check(image,640,480,640,480);
        Check(image,16384,16384,8192,8192); Check(image,0,0,1440,900);
        Check(image,17000,18000,1440,900); Check(image,2880,1800,1440,900,false);
        SetScreenshotLogCallback(LogScreenshot);
        CheckEligibility(image,false,false,false,false,false);
        CheckEligibility(image,true,false,false,false,false);
        CheckEligibility(image,true,true,true,false,false); // Actual v6 dump: enabled manager, null +68.
        CheckEligibility(image,true,true,true,false,false); // Repeated request must not spam diagnostics.
        Require(g_logCalls==2,"Screenshot skip diagnostics missing or repeated");
        CheckEligibility(image,true,true,false,false,true);
        CheckEligibility(image,true,true,true,true,true);
        SetScreenshotLogCallback(nullptr);
        unsigned char originalCall[5]{}; DWORD read = 0;
        Require(SetFilePointer(file,0x1B098,nullptr,FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
            ReadFile(file,originalCall,5,&read,nullptr) && read == 5 && originalCall[0] == 0xE8 && originalCall[1] == 0x83,
            "Original game file changed");
        std::printf("Passed: %u signature rejections; 10 patched-CALL/native-getter view cases; 6 lifecycle cases including actual v6 shadow precondition; original target unchanged.\n", rejected);
        UnmapViewOfFile(image); CloseHandle(mapping); CloseHandle(file); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAILED: %s (Win32 %lu)\n",error.what(),GetLastError());
        if (image) UnmapViewOfFile(image); if (mapping) CloseHandle(mapping); if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return 1;
    }
}
