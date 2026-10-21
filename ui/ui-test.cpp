#include "ui.cpp"

static bool PrivateWrite(unsigned char* address, const void* bytes, size_t size) {
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return VirtualProtect(address, size, protection, &ignored) != FALSE;
}
static void* TestThunk(unsigned char* image, DWORD call) {
    const unsigned char pushArg[] = {0xFF,0x74,0x24,0x04};
    const unsigned char ret[] = {0xC2,4,0};
    if (!PrivateWrite(image + call - 4, pushArg, sizeof(pushArg)) || !PrivateWrite(image + call + 5, ret, sizeof(ret))) return nullptr;
    return image + call - 4;
}
static bool Equal(float actual, float expected) { return std::fabs(actual - expected) < 0.0001f; }
struct ComparisonResult { int changed; void* world; int selectedHeight; };
__declspec(naked) static void InvokeVideoComparison(void*, int, int, ComparisonResult*) {
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, [esp + 10h]
        mov eax, [esp + 14h]
        mov edx, [esp + 18h]
        mov esi, [esp + 1Ch]
        sub esp, 20h
        mov [esp + 18h], eax
        mov [esp + 1Ch], edx
        lea eax, resumed
        mov [esp + 10h], eax
        jmp ebx
    resumed:
        add esp, 0Ch
        pop edi
        pop esi
        pop ebx
        ret
    }
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    bool ok = true;
    double value = 0;
    ok &= ParseScale("1.25", value) && value == 1.25;
    ok &= !ParseScale("nan", value) && !ParseScale("1.5x", value) && !ParseScale("0.5", value) && !ParseScale("5", value);
    SetEnvironmentVariableA("MW_MODERN_LOG", "");
    const char* scales[] = {"1.25", "1.5", "2.0"};
    for (const char* scale : scales) {
        HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE mapping = file == INVALID_HANDLE_VALUE ? nullptr : CreateFileMappingA(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
        auto* image = mapping == nullptr ? nullptr : static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!image) return 3;
        ok &= MatchesSignatures(image, kMinimumImageSize);
        const unsigned char corrupt = 0xE9, correct = 0xE8;
        ok &= PrivateWrite(image + 0xE55E, &corrupt, 1) && !MatchesSignatures(image, kMinimumImageSize);
        ok &= PrivateWrite(image + 0xE55E, &correct, 1) && MatchesSignatures(image, kMinimumImageSize);
        ok &= !MatchesSignatures(image, kMinimumImageSize - 1);
        SetEnvironmentVariableA("MW_MODERN_UI_SCALE", scale);
        if (!InstallUiScaleHooks(reinterpret_cast<HMODULE>(image))) return 4;
        // Replace complex UI creation only in this private test view. Capture the
        // exact argument sent to original ConfigureUI; native getters, mouse
        // bounds and remaining raycast conversion execute unchanged engine code.
        const unsigned char captureConfigure[] = {0x8B,0x44,0x24,4, 0x89,0x81,0xA4,0,0,0, 0xC2,4,0};
        ok &= PrivateWrite(image + kConfigure, captureConfigure, sizeof(captureConfigure));
        auto startup = reinterpret_cast<ConfigureFunction>(TestThunk(image, 0xE55E));
        auto resize = reinterpret_cast<ConfigureFunction>(TestThunk(image, 0x29B10));
        if (!startup || !resize) return 5;
        // Execute the patched preselection block, capturing the exact native
        // ECX=height / EDX=width / EAX=World register contract at its return.
        const unsigned char returnOnly = 0xC3;
        ok &= PrivateWrite(image + 0x23759D + 11, &returnOnly, 1);
        void* preselect = image + 0x23759D;
        // Capture which original Video branch the real JMP wrapper takes.
        // The harness supplies the verified native sscanf argument frame.
        const unsigned char captureSame[] = {0x31,0xC0,0x89,0x06,0xC3};
        const unsigned char captureChange[] = {0xC7,0x06,1,0,0,0,0x89,0x46,4,0x89,0x4E,8,0xC3};
        ok &= PrivateWrite(image + 0x238480, captureSame, sizeof(captureSame));
        ok &= PrivateWrite(image + 0x238488, captureChange, sizeof(captureChange));
        // SEC_IMAGE is normally relocated in the test process; fix the single
        // original absolute float address used by the unchanged raycast tail.
        DWORD oneAddress = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(image + 0x34BFDC));
        ok &= PrivateWrite(image + 0x2F51B5, &oneAddress, sizeof(oneAddress));
        unsigned char world[0x374] = {}, renderer[0x548] = {}, mouse[0x40] = {}, backbuffer[0x20] = {};
        Write<void*>(image, kMaster, world);
        Write<void*>(world, kRenderer, renderer);
        Write<void*>(world, kMouse, mouse);
        Write<void*>(renderer, 0x544, backbuffer);
        const int dimensions[][2] = {{1920,1080}, {2880,1800}, {1280,720}, {2560,1440}};
        const auto normalize = reinterpret_cast<bool (__thiscall*)(void*, unsigned int, unsigned int, float*, float*)>(image + 0x2F5150);
        for (size_t mode = 0; mode < 4; ++mode) {
            const int width = dimensions[mode][0], height = dimensions[mode][1];
            Write(backbuffer, 0, width); Write(backbuffer, 4, height);
            Write(world, kUiWidth, width); Write(world, kUiHeight, height);
            (mode == 0 ? startup : resize)(world, width);
            const Size logical = LogicalSize(width, height, std::strtod(scale, nullptr));
            ok &= Read<int>(world, kUiWidth) == logical.width && Read<int>(world, kUiHeight) == logical.height;
            ok &= Read<int>(world, 0xA4) == logical.width;
            ok &= Equal(Read<float>(mouse, 0x24), -static_cast<float>((logical.width+1)/2));
            ok &= Equal(Read<float>(mouse, 0x2C), -static_cast<float>((logical.height+1)/2));
            ok &= Equal(Read<float>(mouse, 0x30), static_cast<float>((logical.width+1)/2));
            ok &= Equal(Read<float>(mouse, 0x38), static_cast<float>((logical.height+1)/2));
            float x = -1, y = -1;
            ok &= normalize(renderer, 0, 0, &x, &y) && Equal(x, 0) && Equal(y, 1);
            ok &= normalize(renderer, logical.width, logical.height, &x, &y) && Equal(x, 1) && Equal(y, 0);
            ok &= normalize(renderer, logical.width/2, logical.height/2, &x, &y) &&
                   Equal(x, static_cast<float>(logical.width/2) / logical.width) &&
                   Equal(y, 1.0f - static_cast<float>(logical.height/2) / logical.height);
            // The post-resize caller supplies physical bounds. Our real patched
            // CALL must replace them with current logical bounds.
            // Execute the exact fastcall hook target; native bounds setter remains unchanged.
            ConfigureScaledMouse(mouse, nullptr, -width/2, -height/2, width/2, height/2);
            ok &= Equal(Read<float>(mouse, 0x30), static_cast<float>((logical.width+1)/2));
            int selectedWidth = 0, selectedHeight = 0;
            void* selectedWorld = nullptr;
            __asm {
                call dword ptr [preselect]
                mov selectedWidth, edx
                mov selectedHeight, ecx
                mov selectedWorld, eax
            }
            ok &= selectedWidth == width && selectedHeight == height && selectedWorld == world;
            ComparisonResult comparison{-1, nullptr, 0};
            InvokeVideoComparison(image + 0x238464, width, height, &comparison);
            ok &= comparison.changed == 0;
            comparison = {-1, nullptr, 0};
            InvokeVideoComparison(image + 0x238464, logical.width, logical.height, &comparison);
            ok &= comparison.changed == 1 && comparison.world == world && comparison.selectedHeight == logical.height;
            comparison = {-1, nullptr, 0};
            InvokeVideoComparison(image + 0x238464, width + 16, height, &comparison);
            ok &= comparison.changed == 1 && comparison.world == world && comparison.selectedHeight == height;
            comparison = {-1, nullptr, 0};
            InvokeVideoComparison(image + 0x238464, width, height + 16, &comparison);
            ok &= comparison.changed == 1 && comparison.world == world && comparison.selectedHeight == height + 16;
            ok &= Read<int>(world, kUiWidth) == logical.width && Read<int>(world, kUiHeight) == logical.height;
            std::printf("scale=%s render=%dx%d logical=%dx%d native bounds/raycast/physical Video comparison=%s\n", scale, width,height,logical.width,logical.height,ok ? "passed" : "FAILED");
        }
        UnmapViewOfFile(image); CloseHandle(mapping); CloseHandle(file); g_image = nullptr;
    }
    std::puts(ok ? "UI scale: private engine-call execution and signature guards passed" : "UI scale FAILED");
    return ok ? 0 : 1;
}
