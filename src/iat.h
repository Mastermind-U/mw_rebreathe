#pragma once
#include "shared.h"
#include <cstring>
inline bool PatchImport(HMODULE module, const char* name, void* replacement) {
    if (!module) return false;
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    bool patched = false;
    for (; imp->Name; ++imp) {
        if (!imp->OriginalFirstThunk) continue;
        auto original = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto target = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; original->u1.AddressOfData; ++original, ++target) {
            if (IMAGE_SNAP_BY_ORDINAL(original->u1.Ordinal)) continue;
            auto symbol = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(symbol->Name), name)) continue;
            DWORD old;
            if (!VirtualProtect(&target->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) continue;
            InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&target->u1.Function), replacement);
            DWORD ignored;
            VirtualProtect(&target->u1.Function, sizeof(void*), old, &ignored);
            patched = true;
        }
    }
    return patched;
}
