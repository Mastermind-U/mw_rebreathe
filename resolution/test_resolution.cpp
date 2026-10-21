// Standalone native patch validation against the user's installed executable.
// Pass the path to Morrowind.exe. No game code is executed and no game files
// are changed. The fixture is mapped as data, without resolving imports.
#include "resolution.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", description, ResolutionHookStatus());
        return 1;
    }
    std::printf("PASS: %s\n", description);
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: test_resolution.exe path-to-Morrowind.exe\n");
        return 2;
    }

    // The fixture's code is never executed, so its mapping can be anywhere.
    constexpr DWORD kReservationSize = 0x800000;
    auto* image = static_cast<BYTE*>(VirtualAlloc(nullptr,
        kReservationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) {
        std::fprintf(stderr, "Cannot map test image: error %lu.\n", GetLastError());
        return 2;
    }

    HANDLE input = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return 2;
    }
    DWORD size = GetFileSize(input, nullptr);
    auto* file = static_cast<BYTE*>(std::malloc(size));
    DWORD read = 0;
    const bool readOk = file && ReadFile(input, file, size, &read, nullptr) && read == size;
    CloseHandle(input);
    if (!readOk || size < sizeof(IMAGE_DOS_HEADER)) {
        std::free(file);
        return 2;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > size) {
        std::free(file);
        return 2;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(file + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage < 0x346924 ||
        nt->OptionalHeader.SizeOfImage > kReservationSize ||
        nt->OptionalHeader.SizeOfHeaders > size) {
        std::free(file);
        return 2;
    }
    auto* sections = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<BYTE*>(sections + nt->FileHeader.NumberOfSections) > file + size) {
        std::free(file);
        return 2;
    }

    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    std::memcpy(image, file, nt->OptionalHeader.SizeOfHeaders);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        if (static_cast<unsigned long long>(section.PointerToRawData) + section.SizeOfRawData > size ||
            static_cast<unsigned long long>(section.VirtualAddress) + section.SizeOfRawData > imageSize) {
            VirtualFree(image, 0, MEM_RELEASE);
            std::free(file);
            return 2;
        }
        std::memcpy(image + section.VirtualAddress, file + section.PointerToRawData,
                    section.SizeOfRawData);
    }
    std::free(file);
    auto* before = static_cast<BYTE*>(std::malloc(imageSize));
    if (!before) {
        VirtualFree(image, 0, MEM_RELEASE);
        return 2;
    }
    std::memcpy(before, image, imageSize);

    DWORD previous = 0;
    VirtualProtect(image, imageSize, PAGE_EXECUTE_READ, &previous);
    auto module = reinterpret_cast<HMODULE>(image);
    int failures = Check(InstallResolutionHooks(module), "installed executable matches patch guards");
    const BYTE nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    failures += Check(std::memcmp(image + 0x2A0A4, nops, sizeof(nops)) == 0,
                      "only aspect-ratio exclusion branch is disabled");
    std::memcpy(before + 0x2A0A4, nops, sizeof(nops));
    const auto callSite = image + 0x29DEB;
    LONG relative = 0;
    std::memcpy(&relative, callSite + 1, sizeof(relative));
    const auto sortTarget = reinterpret_cast<ULONG_PTR>(callSite + 5) + relative;
    const auto executable = reinterpret_cast<ULONG_PTR>(GetModuleHandleW(nullptr));
    const auto* testDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(executable);
    const auto* testNt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(executable + testDos->e_lfanew);
    failures += Check(callSite[0] == 0xE8 && sortTarget >= executable &&
                      sortTarget < executable + testNt->OptionalHeader.SizeOfImage,
                      "Video sort CALL targets the injected module's bounded sorter");
    std::memcpy(before + 0x29DEB, callSite, 5);
    failures += Check(std::memcmp(image, before, imageSize) == 0,
                      "only six filter bytes and five sort-call bytes change");
    MEMORY_BASIC_INFORMATION info = {};
    VirtualQuery(image + 0x2A0A4, &info, sizeof(info));
    failures += Check(info.Protect == PAGE_EXECUTE_READ, "page protection is restored");
    failures += Check(InstallResolutionHooks(module), "second installation is idempotent");

    VirtualProtect(image, imageSize, PAGE_READWRITE, &previous);
    image[0x29DEB] = 0xCC;
    failures += Check(!InstallResolutionHooks(module) && image[0x29DEB] == 0xCC,
                      "unexpected sorting call is rejected without writing");
    image[0x29DEB] = 0xE8;
    image[0x2A0A4] = 0xCC;
    failures += Check(!InstallResolutionHooks(module) && image[0x2A0A4] == 0xCC,
                      "unexpected branch is rejected without writing");
    std::memcpy(image + 0x2A0A4, nops, sizeof(nops));
    image[0x2A07C] ^= 1;
    failures += Check(!InstallResolutionHooks(module), "unexpected surrounding code is rejected");
    image[0x2A07C] ^= 1;
    image[0x346920] ^= 1;
    failures += Check(!InstallResolutionHooks(module), "unexpected aspect constant is rejected");
    failures += Check(!InstallResolutionHooks(nullptr), "null module is rejected");

    std::free(before);
    VirtualFree(image, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}

