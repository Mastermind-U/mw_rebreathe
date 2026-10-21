// Runs display transformation tests in its own process. The two test windows
// stay hidden. The COM device factory is a mock; no renderer is created.
#include "display.cpp"

namespace {
int g_failures = 0;
int g_checks = 0;
int g_registryFallbacks = 0;
HRESULT g_mockResult = S_OK;
PresentParameters8 g_received{};
void* g_mockObject = nullptr;
int g_reloadedDeviceCalls = 0;

void Check(bool value, const char* message) {
    ++g_checks;
    if (!value) { ++g_failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    else std::printf("PASS: %s\n", message);
}
LSTATUS WINAPI RegistryFallback(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) {
    ++g_registryFallbacks;
    return ERROR_FILE_NOT_FOUND;
}
HRESULT WINAPI MockDevice(void*, UINT, DWORD, HWND, DWORD, PresentParameters8* parameters, void** device) {
    g_received = *parameters;
    if (device) *device = SUCCEEDED(g_mockResult) ? reinterpret_cast<void*>(1) : nullptr;
    if (FAILED(g_mockResult)) parameters->width = 1;
    return g_mockResult;
}
HRESULT WINAPI ReloadedDevice(void* object, UINT adapter, DWORD type, HWND focus, DWORD behavior,
    PresentParameters8* parameters, void** device) {
    ++g_reloadedDeviceCalls;
    return MockDevice(object, adapter, type, focus, behavior, parameters, device);
}
unsigned FactorySlotCount() {
    unsigned count = 0;
    for (const auto& slot : g_vtables) if (slot.table) ++count;
    return count;
}
void* WINAPI MockFactory(UINT) { return g_mockObject; }
FARPROC WINAPI MockResolve(HMODULE, LPCSTR) { return reinterpret_cast<FARPROC>(&MockFactory); }
}

int main() {
    EnableDpiAwareness();
    Check(IsProcessDPIAware() != FALSE, "probe is DPI aware before window creation");
    g_game = GetModuleHandleW(nullptr);
    g_mode = ScreenMode::Borderless;
    g_width = 1920;
    g_height = 1080;
    g_queryRegistry = RegistryFallback;

    DWORD size = 4, type = 0, value = 0;
    Check(QueryRegistry(nullptr, "Screen Width", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
          size == 4 && type == REG_DWORD && value == 1920, "startup registry width is selected width");
    BYTE fullscreen = 0xFF;
    size = 1;
    Check(QueryRegistry(nullptr, "Fullscreen", nullptr, &type, &fullscreen, &size) == ERROR_SUCCESS &&
          fullscreen == 0 && size == 1 && type == REG_BINARY, "native one-byte fullscreen flag becomes windowed");
    size = 0;
    Check(QueryRegistry(nullptr, "Fullscreen", nullptr, &type, &fullscreen, &size) == ERROR_MORE_DATA &&
          size == 1, "registry buffer size semantics are preserved");
    size = 0;
    Check(QueryRegistry(nullptr, "Screen Height", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
          size == 4 && type == REG_DWORD, "registry size-only query is supported");
    size = 4;
    Check(QueryRegistry(nullptr, "Unrelated", nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_FILE_NOT_FOUND &&
          g_registryFallbacks == 1, "unrelated registry query remains native");

    WNDCLASSA windowClass{};
    windowClass.hInstance = g_game;
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.lpszClassName = "Morrowind";
    Check(RegisterClassA(&windowClass) != 0, "register isolated hidden test window class");
    HWND window = CreateGameWindow(WS_EX_CLIENTEDGE, "Morrowind", "Display test", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, g_game, nullptr);
    Check(window && window == g_mainWindow, "only matching main window is tracked");
    RECT client{};
    GetClientRect(window, &client);
    MONITORINFO monitor{sizeof(monitor)};
    Check(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE, "query borderless monitor");
    Check(client.right == monitor.rcMonitor.right - monitor.rcMonitor.left &&
          client.bottom == monitor.rcMonitor.bottom - monitor.rcMonitor.top,
          "borderless startup fills monitor independently of selected backbuffer");
    Check((GetWindowLongA(window, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)) == 0 &&
          (GetWindowLongA(window, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == 0, "borderless decorations are removed");
    Check(!IsWindowVisible(window), "test window stays hidden");
    HWND child = CreateGameWindow(0, "Morrowind", "Child", WS_CHILD | WS_CAPTION,
        0, 0, 320, 240, window, nullptr, g_game, nullptr);
    Check(child && g_mainWindow == window && (GetWindowLongA(child, GWL_STYLE) & WS_CHILD) &&
          (GetWindowLongA(child, GWL_STYLE) & WS_CAPTION), "renderer child window is not restyled or tracked as main");

    void* table[16]{};
    table[15] = reinterpret_cast<void*>(&MockDevice);
    struct Object { void** vtable; } object{table};
    Check(HookDeviceFactory(&object), "COM factory slot is patched");
    Check(HookDeviceFactory(&object), "COM factory patch is idempotent");
    g_mockObject = &object;
    g_getProcAddress = &MockResolve;
    const HMODULE d3d = LoadLibraryExW(L"d3d8.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto factory = ResolveDisplayProc(d3d, "Direct3DCreate8");
    Check(d3d && factory == reinterpret_cast<FARPROC>(&CreateDirect3D) &&
          reinterpret_cast<CreateD3D8>(factory)(120) == &object,
          "actual NetImmerse GetProcAddress path returns the hooked factory");
    Check(ResolveDisplayProc(GetModuleHandleW(L"kernel32.dll"), "Direct3DCreate8") == reinterpret_cast<FARPROC>(&MockFactory),
          "same function name in another module is not intercepted");
    Check(ResolveDisplayProc(d3d, reinterpret_cast<LPCSTR>(1)) == reinterpret_cast<FARPROC>(&MockFactory),
          "ordinal lookup is forwarded without dereferencing a string");
    Check(ResolveDisplayProc(d3d, "OtherFunction") == reinterpret_cast<FARPROC>(&MockFactory),
          "unrelated function lookup is forwarded unchanged");
    auto createDevice = reinterpret_cast<CreateDevice8>(table[15]);
    PresentParameters8 parameters{};
    parameters.width = 640;
    parameters.height = 480;
    parameters.fullscreenRefreshRate = 144;
    parameters.fullscreenInterval = 1;
    void* device = nullptr;
    Check(SUCCEEDED(createDevice(&object, 0, 1, window, 0, &parameters, &device)) &&
          g_received.width == 1920 && g_received.height == 1080 && g_received.windowed &&
          g_received.fullscreenRefreshRate == 0 && g_received.fullscreenInterval == 0,
          "borderless startup uses selected resolution and real windowed presentation");

    parameters.width = 2560;
    parameters.height = 1440;
    parameters.windowed = FALSE;
    Check(SUCCEEDED(createDevice(&object, 0, 1, window, 0, &parameters, &device)) &&
          g_received.width == 2560 && g_received.height == 1440 && g_received.windowed,
          "subsequent in-game resolution is honored without startup override");
    GetClientRect(window, &client);
    Check(client.right == monitor.rcMonitor.right - monitor.rcMonitor.left &&
          client.bottom == monitor.rcMonitor.bottom - monitor.rcMonitor.top,
          "borderless keeps monitor size after changing renderer resolution");
    size = 4;
    Check(QueryRegistry(nullptr, "Screen Width", nullptr, nullptr, reinterpret_cast<BYTE*>(&value), &size) == ERROR_FILE_NOT_FOUND &&
          g_registryFallbacks == 2, "startup registry dimension override ends after device creation");

    g_mode = ScreenMode::Window;
    parameters.width = 1280;
    parameters.height = 720;
    parameters.windowed = FALSE;
    Check(SUCCEEDED(createDevice(&object, 0, 1, window, 0, &parameters, &device)) && g_received.windowed &&
          g_received.width == 1280, "window mode uses windowed D3D and requested dimensions");
    g_mode = ScreenMode::Fullscreen;
    parameters.windowed = TRUE;
    parameters.fullscreenRefreshRate = 60;
    parameters.fullscreenInterval = 1;
    Check(SUCCEEDED(createDevice(&object, 0, 1, window, 0, &parameters, &device)) && g_received.windowed &&
          g_received.fullscreenRefreshRate == 0 && g_received.fullscreenInterval == 0,
          "scaled fullscreen uses safe windowed presentation without exclusive refresh settings");

    g_mode = ScreenMode::Borderless;
    InterlockedExchange(&g_startupComplete, 0);
    g_mockResult = E_FAIL;
    parameters.width = 800;
    parameters.height = 600;
    parameters.windowed = FALSE;
    const auto before = parameters;
    Check(FAILED(createDevice(&object, 0, 1, window, 0, &parameters, &device)) &&
          !std::memcmp(&before, &parameters, sizeof(parameters)) && !g_startupComplete,
          "failed creation restores caller parameters and permits a clean startup retry");

    // A DLL can be unloaded and remapped at the same address. Fill all factory
    // records, then replace one tracked table's native slot in place: refreshing
    // that address must reuse its existing record and capture the NEW callee.
    void* otherTables[8][16]{};
    Object otherObjects[8]{};
    Check(FactorySlotCount() == 1, "initial factory occupies exactly one tracking slot");
    for (unsigned i = 0; i < 7; ++i) {
        otherTables[i][15] = reinterpret_cast<void*>(&MockDevice);
        otherObjects[i].vtable = otherTables[i];
        Check(HookDeviceFactory(&otherObjects[i]), "each distinct factory receives one bounded tracking slot");
    }
    Check(FactorySlotCount() == 8, "all eight factory tracking slots are occupied");
    otherTables[7][15] = reinterpret_cast<void*>(&MockDevice);
    otherObjects[7].vtable = otherTables[7];
    Check(!HookDeviceFactory(&otherObjects[7]) && otherTables[7][15] == reinterpret_cast<void*>(&MockDevice) &&
          FactorySlotCount() == 8, "ninth distinct table is refused without changing its native callee");

    table[15] = reinterpret_cast<void*>(&ReloadedDevice);
    Check(HookDeviceFactory(&object) && table[15] == reinterpret_cast<void*>(&CreateDevice) &&
          FactorySlotCount() == 8, "reused table with a fresh native callee is rehooked without another slot");
    CreateDevice8 refreshedOriginal = nullptr;
    for (const auto& slot : g_vtables) if (slot.table == table) refreshedOriginal = slot.original;
    Check(refreshedOriginal == &ReloadedDevice, "reused table records its fresh native callee instead of the stale address");
    Check(HookDeviceFactory(&object) && FactorySlotCount() == 8,
          "refreshed factory remains idempotent even when tracking capacity is full");
    refreshedOriginal = nullptr;
    for (const auto& slot : g_vtables) if (slot.table == table) refreshedOriginal = slot.original;
    Check(refreshedOriginal == &ReloadedDevice && refreshedOriginal != &CreateDevice,
          "idempotent refresh never stores the hook itself as the original callee");

    g_mockResult = S_OK;
    g_mode = ScreenMode::Window;
    InterlockedExchange(&g_startupComplete, 1);
    parameters = {};
    parameters.width = 640;
    parameters.height = 480;
    parameters.deviceWindow = child;
    auto refreshedCreate = reinterpret_cast<CreateDevice8>(table[15]);
    Check(SUCCEEDED(refreshedCreate(&object, 0, 1, window, 0, &parameters, &device)) &&
          g_reloadedDeviceCalls == 1 && g_received.windowed && g_received.width == 640 && g_received.height == 480,
          "actual refreshed vtable call reaches the fresh native factory through the display hook");
    DestroyWindow(window);
    if (d3d) FreeLibrary(d3d);
    UnregisterClassA("Morrowind", g_game);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
