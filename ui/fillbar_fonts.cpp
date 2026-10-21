// Independent native Morrowind 1.6.1820 fillbar-caption font-role hook.
#include "fillbar_fonts.h"
#include <cstdint>
#include <cstring>

namespace {
static_assert(sizeof(void*) == 4, "Native Morrowind hooks require x86");
constexpr DWORD kCall = 0x23E83D, kCreateLabel = 0x188BE0, kSetProperty = 0x181F30;
unsigned char* g_image = nullptr;
const char* g_status = "Fillbar caption role is native Font0.";
using Label = void* (__thiscall*)(void*, int, const char*, bool, bool);
using SetProperty = void (__thiscall*)(void*, short, int, short);

bool Readable(const void* pointer, size_t length) {
    if (!pointer || !length) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start + length < start) return false;
    uintptr_t cursor = start;
    while (cursor < start + length) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= cursor) return false;
        cursor = end;
    }
    return true;
}

void* __fastcall Caption(void* parent, void*, int id, const char* text, bool black, bool replace) {
    void* result = reinterpret_cast<Label>(g_image + kCreateLabel)(parent, id, text, black, replace);
    const DWORD error = GetLastError();
    if (result) reinterpret_cast<SetProperty>(g_image + kSetProperty)(result, static_cast<short>(-0x7FEA), 1, 1);
    SetLastError(error);
    return result;
}

bool Guard(unsigned char* image) {
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x100000 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32))) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.ImageBase != 0x400000 ||
        nt->OptionalHeader.SizeOfImage < 0x3D0000) return false;
    struct Signature { DWORD rva; const char* bytes; size_t size; };
    const Signature signatures[] = {
        {0x23E831, "\x6A\x00\x6A\x00\x68\x80\x61\x7C\x00\x51\x8B\xCE", 12},
        {kCall, "\xE8\x9E\xA3\xF4\xFF", 5},
        {kCreateLabel, "\x83\xEC\x0C\x56\x8B\xF1\x80\x3E\x58", 9},
        {0x188D0B, "\xC7\x87\x5C\x01\x00\x00\x00\x00\x00\x00", 10},
        {kSetProperty, "\x83\xEC\x18\x53\x55\x8B\x6C\x24\x28\x56\x57", 11},
    };
    for (const auto& s : signatures) if (!Readable(image + s.rva, s.size) || std::memcmp(image + s.rva, s.bytes, s.size)) return false;
    return true;
}
}

bool InstallFillbarFontHooks(HMODULE module) {
    auto* image = reinterpret_cast<unsigned char*>(module);
    if (g_image) return g_image == image;
    if (!Guard(image)) { g_status = "Fillbar caption hook rejected unsupported native signatures."; return false; }
    unsigned char original[5]{}, patch[5]{0xE8};
    std::memcpy(original, image + kCall, sizeof(original));
    const auto delta = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&Caption) - reinterpret_cast<uintptr_t>(image + kCall + 5));
    std::memcpy(patch + 1, &delta, sizeof(delta));
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(image + kCall, sizeof(patch), PAGE_EXECUTE_READWRITE, &protection)) return false;
    g_image = image;
    std::memcpy(image + kCall, patch, sizeof(patch));
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), image + kCall, sizeof(patch)) != FALSE;
    const bool restored = VirtualProtect(image + kCall, sizeof(patch), protection, &ignored) != FALSE;
    if (!flushed || !restored) {
        DWORD temporary = 0;
        const bool writable = VirtualProtect(image + kCall, sizeof(original), PAGE_EXECUTE_READWRITE, &temporary) != FALSE;
        if (writable) std::memcpy(image + kCall, original, sizeof(original));
        const bool rolledFlush = writable && FlushInstructionCache(GetCurrentProcess(), image + kCall, sizeof(original)) != FALSE;
        const bool rolledProtection = VirtualProtect(image + kCall, sizeof(original), protection, &ignored) != FALSE;
        g_image = nullptr;
        g_status = writable && rolledFlush && rolledProtection ? "Fillbar hook API failure; original CALL restored." :
            "Fillbar hook rollback failed; suspended game must not resume.";
        return false;
    }
    g_status = "Native fillbar captions use Font1 before first layout; other text remains Font0.";
    return true;
}
const char* FillbarFontHookStatus() { return g_status; }
