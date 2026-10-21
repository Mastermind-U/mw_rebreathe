#define NOMINMAX
#include "fillbar_fonts.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
unsigned char g_label[0x184]{};
void* g_parent = reinterpret_cast<void*>(0x30);
const char* g_text = reinterpret_cast<const char*>(0x7C6180);
bool g_null = false, g_failure = false;
unsigned g_createCalls = 0, g_propertyCalls = 0;
uintptr_t g_before = 0, g_after = 0;
uint32_t g_bx = 0, g_si = 0, g_di = 0, g_bp = 0;
constexpr DWORD kIncoming = 0x12345678, kOutgoing = 0x87654321;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

void* __fastcall LabelStub(void* parent, void*, int id, const char* text, bool black, bool replace) {
    ++g_createCalls;
    if (parent != g_parent || id != 123 || text != g_text || black || replace || GetLastError() != kIncoming) g_failure = true;
    SetLastError(kOutgoing);
    return g_null ? nullptr : g_label;
}
void __fastcall PropertyStub(void* label, void*, short property, int value, short type) {
    ++g_propertyCalls;
    if (label != g_label || property != -0x7FEA || value != 1 || type != 1) g_failure = true;
    std::memcpy(g_label + 0x15C, &value, sizeof(value));
    SetLastError(0xBADBEEF);
}
__declspec(naked) void* __stdcall Invoke(void*) {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebx, 13579BDFh
        mov esi, g_parent
        mov edi, 2468ACE0h
        mov ebp, 31415926h
        mov g_before, esp
        mov eax, [esp + 20]
        mov ecx, 123
        call eax
        mov g_after, esp
        mov g_bx, ebx
        mov g_si, esi
        mov g_di, edi
        mov g_bp, ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 4
    }
}
void Write(void* dest, const void* src, size_t size) {
    DWORD old = 0, ignored = 0;
    Require(VirtualProtect(dest, size, PAGE_EXECUTE_READWRITE, &old) != FALSE, "Private image protection failed");
    std::memcpy(dest, src, size);
    Require(FlushInstructionCache(GetCurrentProcess(), dest, size) != FALSE, "Private image flush failed");
    Require(VirtualProtect(dest, size, old, &ignored) != FALSE, "Private image protection restore failed");
}
void Jump(unsigned char* dest, const void* function) {
    unsigned char bytes[5]{0xE9};
    uint32_t offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(dest + 5));
    std::memcpy(bytes + 1, &offset, 4); Write(dest, bytes, 5);
}
}
int wmain(int argc, wchar_t** argv) {
    HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr; void* view = nullptr;
    try {
        Require(argc == 2, "Pass original Morrowind.exe path");
        file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "Cannot open original EXE");
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
        Require(mapping != nullptr, "Cannot create private image");
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0); Require(view != nullptr, "Cannot map private image");
        auto* image = static_cast<unsigned char*>(view);
        Require(!InstallFillbarFontHooks(nullptr), "Null image accepted");
        Require(!InstallFillbarFontHooks(reinterpret_cast<HMODULE>(0x30)), "Unreadable image accepted");
        for (DWORD rva : {0x23E831u, 0x23E83Du, 0x188BE0u, 0x188D0Bu, 0x181F30u}) {
            unsigned char before = image[rva], bad = static_cast<unsigned char>(before ^ 0x55), call[5]{};
            std::memcpy(call, image + 0x23E83D, 5); Write(image + rva, &bad, 1);
            unsigned char rejectedCall[5]{}; std::memcpy(rejectedCall, image + 0x23E83D, 5);
            Require(!InstallFillbarFontHooks(reinterpret_cast<HMODULE>(image)), "Corrupt signature accepted");
            Require(std::memcmp(rejectedCall, image + 0x23E83D, 5) == 0, "Rejected image was mutated");
            Write(image + rva, &before, 1);
            Require(std::memcmp(call, image + 0x23E83D, 5) == 0, "CALL restoration failure");
        }
        Require(InstallFillbarFontHooks(reinterpret_cast<HMODULE>(image)), FillbarFontHookStatus());
        Require(InstallFillbarFontHooks(reinterpret_cast<HMODULE>(image)), "Idempotent installation failed");
        Jump(image + 0x188BE0, reinterpret_cast<const void*>(&LabelStub));
        Jump(image + 0x181F30, reinterpret_cast<const void*>(&PropertyStub));
        const unsigned char ret = 0xC3; Write(image + 0x23E842, &ret, 1);
        for (unsigned i = 0; i < 10001; ++i) {
            g_null = (i % 7 == 0); const unsigned prior = g_propertyCalls;
            int sentinel = 7; std::memcpy(g_label + 0x15C, &sentinel, 4);
            SetLastError(kIncoming);
            void* result = Invoke(image + 0x23E831);
            Require(result == (g_null ? nullptr : g_label), "Return value changed");
            Require(GetLastError() == kOutgoing, "LastError changed");
            Require(g_before == g_after && g_bx == 0x13579BDF && g_si == reinterpret_cast<uintptr_t>(g_parent) && g_di == 0x2468ACE0 && g_bp == 0x31415926, "Stack/nonvolatile registers changed");
            int font = -1; std::memcpy(&font, g_label + 0x15C, 4);
            Require(font == (g_null ? 7 : 1), "Null result accessed or caption role not changed");
            Require(g_propertyCalls == prior + (g_null ? 0 : 1), "Wrong property invocation count");
            Require(!g_failure, "Native arguments/LastError/property signature changed");
        }
        Require(g_createCalls == 10001, "Original constructor call count changed");
        std::printf("PASS: 10001 patched caption CALLs; null results, arguments, return, stack, nonvolatile registers and LastError preserved; 5 signature guards rejected before mutation.\n");
        UnmapViewOfFile(view); CloseHandle(mapping); CloseHandle(file); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return 1;
    }
}
