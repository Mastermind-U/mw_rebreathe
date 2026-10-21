#include "../src/shared.h"
#include "../src/iat.h"
#include "display.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
enum class ScreenMode { Native, Fullscreen, Window, Borderless };
struct PresentParameters8 {
    UINT width, height;
    DWORD format;
    UINT backBuffers;
    DWORD multisample, swapEffect;
    HWND deviceWindow;
    BOOL windowed, autoDepthStencil;
    DWORD depthFormat, flags;
    UINT fullscreenRefreshRate, fullscreenInterval;
};
static_assert(sizeof(PresentParameters8) == 52, "Direct3D 8 x86 presentation ABI");
using CreateD3D8 = void* (WINAPI*)(UINT);
using CreateDevice8 = HRESULT (WINAPI*)(void*, UINT, DWORD, HWND, DWORD,
                                      PresentParameters8*, void**);
struct VtableHook { void** table = nullptr; CreateDevice8 original = nullptr; };
VtableHook g_vtables[8];
SRWLOCK g_vtableLock = SRWLOCK_INIT;
ScreenMode g_mode = ScreenMode::Native;
HMODULE g_game = nullptr;
bool g_installed = false;
HWND g_mainWindow = nullptr;
HWND g_deviceWindow = nullptr;
WNDPROC g_previousWindowProcedure = nullptr;
bool g_sizingWindow = false;
UINT g_width = 0, g_height = 0;
UINT g_backbufferWidth = 0, g_backbufferHeight = 0;
volatile LONG g_startupComplete = 0;
char g_status[512] = "Display hooks have not run.";
void (*g_log)(const char*) = nullptr;
CreateD3D8 g_createD3D = nullptr;
auto g_createWindow = &CreateWindowExA;
auto g_setWindowPos = &SetWindowPos;
auto g_queryRegistry = &RegQueryValueExA;
auto g_getProcAddress = &GetProcAddress;
auto g_keyboardProtect = &VirtualProtect;
auto g_keyboardFlush = &FlushInstructionCache;

bool FillsMonitor(ScreenMode mode) {
    return mode == ScreenMode::Fullscreen || mode == ScreenMode::Borderless;
}

const char* ModeName() {
    switch (g_mode) {
    case ScreenMode::Fullscreen: return "fullscreen";
    case ScreenMode::Window: return "window";
    case ScreenMode::Borderless: return "borderless";
    default: return "native";
    }
}
void Record(const char* text) {
    strcpy_s(g_status, text);
    OutputDebugStringA(text);
    if (g_log) { g_log(text); g_log("\r\n"); }
}
bool Fail(const char* text) { Record(text); return false; }

bool PatchKeyboardCooperativeLevel(HMODULE game) {
    if(!game) return Fail("Keyboard input error: missing executable module.");
    auto* block=reinterpret_cast<unsigned char*>(game)+0x6278;
    const unsigned char original[]={
        0x8B,0x46,0x28,0x8B,0x28,0x6A,0x19,0xFF,0x15,0x74,0x63,0x74,0x00,
        0x8B,0x4E,0x28,0x50,0x51,0xEB,0x12,
        0x8B,0x56,0x28,0x8B,0x2A,0x6A,0x15,0xFF,0x15,0x74,0x63,0x74,0x00,
        0x50,0x8B,0x46,0x28,0x50,0xFF,0x55,0x34
    };
    constexpr size_t background=6,foreground=26;
    for(size_t i=0;i<sizeof(original);++i) {
        if(i!=background && i!=foreground && block[i]!=original[i])
            return Fail("Keyboard input error: native cooperative-level signature differs.");
    }
    if(block[background]==0x0A && block[foreground]==0x06) return true;
    if(block[background]!=0x19 || block[foreground]!=0x15)
        return Fail("Keyboard input error: unexpected cooperative-level flags.");
    DWORD previous=0,ignored=0;
    if(!g_keyboardProtect(block,sizeof(original),PAGE_EXECUTE_READWRITE,&previous))
        return Fail("Keyboard input error: cannot make native flags writable.");
    // Only keyboard flags: keep configured foreground/background access,
    // replace EXCLUSIVE by NONEXCLUSIVE and remove NOWINKEY. Mouse untouched.
    block[background]=0x0A;block[foreground]=0x06;
    const bool flushed=g_keyboardFlush(GetCurrentProcess(),block,sizeof(original))!=FALSE;
    const bool protectedAgain=flushed && g_keyboardProtect(block,sizeof(original),previous,&ignored)!=FALSE;
    if(!flushed || !protectedAgain) {
        block[background]=0x19;block[foreground]=0x15;
        const bool rollbackFlushed=g_keyboardFlush(GetCurrentProcess(),block,sizeof(original))!=FALSE;
        const bool rollbackProtected=g_keyboardProtect(block,sizeof(original),previous,&ignored)!=FALSE;
        if(!rollbackFlushed || !rollbackProtected)
            return Fail("Keyboard input error: flag rollback failed to restore instruction cache or memory protection.");
        return Fail(flushed?"Keyboard input error: protection restore failed; flags rolled back.":
                            "Keyboard input error: instruction-cache flush failed; flags rolled back.");
    }
    Record("Keyboard input: nonexclusive; Windows shortcuts enabled; foreground/background preference preserved.");
    return true;
}

void EnableDpiAwareness() {
    using SetAwareness = BOOL (WINAPI*)(HANDLE);
    const auto user = GetModuleHandleW(L"user32.dll");
    const auto set = reinterpret_cast<SetAwareness>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    if (set) set(reinterpret_cast<HANDLE>(-4)); // Per-monitor awareness v2.
    if (!IsProcessDPIAware()) SetProcessDPIAware();
}

bool ParseDimension(const wchar_t* name, UINT& output) {
    const auto value = Environment(name);
    if (value.empty()) return true;
    wchar_t* end = nullptr;
    const auto number = std::wcstoul(value.c_str(), &end, 10);
    if (*end || number < 1 || number > 16384) return false;
    output = static_cast<UINT>(number);
    return true;
}

void* FindImport(HMODULE module, const char* name) {
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->OriginalFirstThunk) continue;
        auto* original = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk);
        auto* target = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; original->u1.AddressOfData; ++original, ++target) {
            if (IMAGE_SNAP_BY_ORDINAL32(original->u1.Ordinal)) continue;
            const auto* symbol = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + original->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char*>(symbol->Name), name)) {
                return reinterpret_cast<void*>(target->u1.Function);
            }
        }
    }
    return nullptr;
}

LSTATUS SupplyValue(DWORD value, DWORD bytes, DWORD valueType, LPDWORD reserved,
                   LPDWORD type, LPBYTE data, LPDWORD size) {
    if (reserved || !size) return ERROR_INVALID_PARAMETER;
    const DWORD capacity = *size;
    *size = bytes;
    if (type) *type = valueType;
    if (!data) return ERROR_SUCCESS;
    if (capacity < bytes) return ERROR_MORE_DATA;
    std::memcpy(data, &value, bytes);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI QueryRegistry(HKEY key, LPCSTR name, LPDWORD reserved,
                            LPDWORD type, LPBYTE data, LPDWORD size) {
    if (name) {
        if (!_stricmp(name, "Fullscreen") && g_mode != ScreenMode::Native) {
            // Native Morrowind queries a one-byte REG_BINARY flag, not DWORD.
            return SupplyValue(0, 1,
                               REG_BINARY, reserved, type, data, size);
        }
        if (InterlockedCompareExchange(&g_startupComplete, 0, 0) == 0) {
            if (g_width && !_stricmp(name, "Screen Width")) {
                return SupplyValue(g_width, 4, REG_DWORD, reserved, type, data, size);
            }
            if (g_height && !_stricmp(name, "Screen Height")) {
                return SupplyValue(g_height, 4, REG_DWORD, reserved, type, data, size);
            }
        }
    }
    return g_queryRegistry(key, name, reserved, type, data, size);
}

bool MainWindowClass(LPCSTR name, DWORD style, HWND parent, HINSTANCE instance) {
    return name && reinterpret_cast<ULONG_PTR>(name) > 0xFFFF &&
        !_stricmp(name, "Morrowind") && !parent && !(style & WS_CHILD) && instance == g_game;
}

DWORD BorderlessStyle(DWORD style) {
    return (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_MAXIMIZEBOX)) |
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
}
DWORD BorderlessExtendedStyle(DWORD style) {
    return style & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
}

void CenterOnMonitor(HWND window, int width, int height, int& x, int& y) {
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        const auto& rect = g_mode == ScreenMode::Borderless ? monitor.rcMonitor : monitor.rcWork;
        x = rect.left + (rect.right - rect.left - width) / 2;
        y = rect.top + (rect.bottom - rect.top - height) / 2;
    }
}

void SizeRendererWindow() {
    if(!g_mainWindow || !g_deviceWindow || g_deviceWindow==g_mainWindow ||
       !IsChild(g_mainWindow,g_deviceWindow) || IsIconic(g_mainWindow)) return;
    RECT client{};
    if(GetClientRect(g_mainWindow,&client) && client.right>0 && client.bottom>0)
        g_setWindowPos(g_deviceWindow,nullptr,0,0,client.right,client.bottom,SWP_NOACTIVATE|SWP_NOZORDER);
}

LRESULT CALLBACK DisplayWindowProcedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    const auto previous=g_previousWindowProcedure;
    const LRESULT result=previous?CallWindowProcA(previous,window,message,wParam,lParam):DefWindowProcA(window,message,wParam,lParam);
    if(message==WM_SIZE && wParam!=SIZE_MINIMIZED && !g_sizingWindow) SizeRendererWindow();
    if(message==WM_NCDESTROY && window==g_mainWindow) {
        g_mainWindow=nullptr;g_deviceWindow=nullptr;g_previousWindowProcedure=nullptr;
    }
    return result;
}

void SizeMainWindow(UINT width, UINT height) {
    if (!g_mainWindow || !width || !height ||
        g_mode == ScreenMode::Native || IsIconic(g_mainWindow)) return;
    const auto style = static_cast<DWORD>(GetWindowLongA(g_mainWindow, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongA(g_mainWindow, GWL_EXSTYLE));
    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, style, GetMenu(g_mainWindow) != nullptr, extended);
    int outerWidth = rect.right - rect.left, outerHeight = rect.bottom - rect.top;
    int x = 0, y = 0;
    MONITORINFO monitor{sizeof(monitor)};
    if(FillsMonitor(g_mode) && GetMonitorInfoW(MonitorFromWindow(g_mainWindow,MONITOR_DEFAULTTOPRIMARY),&monitor)) {
        x=monitor.rcMonitor.left;y=monitor.rcMonitor.top;
        outerWidth=monitor.rcMonitor.right-x;outerHeight=monitor.rcMonitor.bottom-y;
    } else {
        if(g_mode==ScreenMode::Window && GetMonitorInfoW(MonitorFromWindow(g_mainWindow,MONITOR_DEFAULTTOPRIMARY),&monitor)) {
            const int frameWidth=outerWidth-int(width),frameHeight=outerHeight-int(height);
            const int availableW=(std::max)(1,int(monitor.rcWork.right-monitor.rcWork.left)-frameWidth);
            const int availableH=(std::max)(1,int(monitor.rcWork.bottom-monitor.rcWork.top)-frameHeight);
            const double factor=(std::min)(1.0,(std::min)(double(availableW)/width,double(availableH)/height));
            if(factor<1.0) {
                rect={0,0,LONG(width*factor),LONG(height*factor)};
                AdjustWindowRectEx(&rect,style,GetMenu(g_mainWindow)!=nullptr,extended);
                outerWidth=rect.right-rect.left;outerHeight=rect.bottom-rect.top;
            }
        }
        CenterOnMonitor(g_mainWindow, outerWidth, outerHeight, x, y);
    }
    g_sizingWindow=true;
    g_setWindowPos(g_mainWindow, nullptr, x, y, outerWidth, outerHeight,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    g_sizingWindow=false;
    SizeRendererWindow();
}

HWND WINAPI CreateGameWindow(DWORD extended, LPCSTR className, LPCSTR title,
                             DWORD style, int x, int y, int width, int height,
                             HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    const bool main = MainWindowClass(className, style, parent, instance);
    if (main && (g_mode == ScreenMode::Borderless || g_mode == ScreenMode::Fullscreen)) {
        style = BorderlessStyle(style);
        extended = BorderlessExtendedStyle(extended);
    }
    if (main && g_width && g_height && g_mode != ScreenMode::Native) {
        RECT rect{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
        AdjustWindowRectEx(&rect, style, menu != nullptr, extended);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        MONITORINFO monitor{sizeof(monitor)};
        if (FillsMonitor(g_mode) && GetMonitorInfoW(MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
            x = monitor.rcMonitor.left; y = monitor.rcMonitor.top;
            width = monitor.rcMonitor.right - x; height = monitor.rcMonitor.bottom - y;
        } else CenterOnMonitor(nullptr, width, height, x, y);
    }
    const HWND result = g_createWindow(extended, className, title, style, x, y,
                                      width, height, parent, menu, instance, parameter);
    if (main && result) {
        g_mainWindow = result;
        SetLastError(0);
        const LONG old=SetWindowLongA(result,GWL_WNDPROC,LONG(reinterpret_cast<LONG_PTR>(&DisplayWindowProcedure)));
        if(old || GetLastError()==0) g_previousWindowProcedure=reinterpret_cast<WNDPROC>(static_cast<LONG_PTR>(old));
    }
    return result;
}

BOOL WINAPI PositionGameWindow(HWND window, HWND after, int x, int y, int width, int height, UINT flags) {
    if(window==g_deviceWindow && FillsMonitor(g_mode) && g_mainWindow && IsChild(g_mainWindow,window)) {
        RECT client{};
        if(GetClientRect(g_mainWindow,&client) && client.right>0 && client.bottom>0) {
            x=0;y=0;width=client.right;height=client.bottom;flags&=~(SWP_NOMOVE|SWP_NOSIZE);
        }
    }
    if (window == g_mainWindow && (g_mode == ScreenMode::Borderless || g_mode==ScreenMode::Fullscreen) && !IsIconic(window)) {
        const DWORD style = BorderlessStyle(static_cast<DWORD>(GetWindowLongA(window, GWL_STYLE)));
        const DWORD extended = BorderlessExtendedStyle(static_cast<DWORD>(GetWindowLongA(window, GWL_EXSTYLE)));
        SetWindowLongA(window, GWL_STYLE, static_cast<LONG>(style));
        SetWindowLongA(window, GWL_EXSTYLE, static_cast<LONG>(extended));
        if(FillsMonitor(g_mode)) {
            MONITORINFO monitor{sizeof(monitor)};
            if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor)) {
                x=monitor.rcMonitor.left;y=monitor.rcMonitor.top;width=monitor.rcMonitor.right-x;height=monitor.rcMonitor.bottom-y;
                flags&=~(SWP_NOMOVE|SWP_NOSIZE);
            }
        } else if (!(flags & SWP_NOSIZE) && width > 0 && height > 0) {
            CenterOnMonitor(window, width, height, x, y);
            flags &= ~SWP_NOMOVE;
        }
        flags |= SWP_FRAMECHANGED;
    }
    return g_setWindowPos(window, after, x, y, width, height, flags);
}

HRESULT WINAPI CreateDevice(void* object, UINT adapter, DWORD type, HWND focus,
                            DWORD behavior, PresentParameters8* parameters, void** device) {
    CreateDevice8 original = nullptr;
    void** table = *static_cast<void***>(object);
    AcquireSRWLockShared(&g_vtableLock);
    for (const auto& slot : g_vtables) if (slot.table == table) original = slot.original;
    ReleaseSRWLockShared(&g_vtableLock);
    if (!original) return E_FAIL;
    if (!parameters) return original(object, adapter, type, focus, behavior, parameters, device);

    const auto saved = *parameters;
    const bool startup = InterlockedCompareExchange(&g_startupComplete, 0, 0) == 0;
    if (startup && g_width && g_height) {
        parameters->width = g_width;
        parameters->height = g_height;
    }
    if (g_mode != ScreenMode::Native) {
        // All three modern modes use windowed presentation. Fullscreen and
        // borderless fill the monitor; Present stretches the chosen backbuffer.
        parameters->windowed = TRUE;
        if (parameters->windowed) {
            parameters->fullscreenRefreshRate = 0;
            parameters->fullscreenInterval = 0;
        }
    }
    const HRESULT result = original(object, adapter, type, focus, behavior, parameters, device);
    if (SUCCEEDED(result)) {
        g_deviceWindow=parameters->deviceWindow;
        g_backbufferWidth=parameters->width;g_backbufferHeight=parameters->height;
        InterlockedExchange(&g_startupComplete, 1);
        SizeMainWindow(parameters->width, parameters->height);
    }
    RECT client{}, renderClient{};
    MONITORINFO monitor{sizeof(monitor)};
    if (g_mainWindow) GetClientRect(g_mainWindow, &client);
    if (g_deviceWindow) GetClientRect(g_deviceWindow, &renderClient);
    GetMonitorInfoW(MonitorFromWindow(g_mainWindow, MONITOR_DEFAULTTOPRIMARY), &monitor);
    char message[512];
    sprintf_s(message, "Display device: mode=%s backbuffer=%ux%u Windowed=%d result=%08lX DPI-aware=%d client=%ldx%ld render_client=%ldx%ld monitor=%ldx%ld",
              ModeName(), parameters->width, parameters->height, parameters->windowed,
              static_cast<unsigned long>(result), IsProcessDPIAware(), client.right, client.bottom,
              renderClient.right, renderClient.bottom,
              monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    Record(message);
    if (FAILED(result)) *parameters = saved;
    return result;
}

bool HookDeviceFactory(void* object) {
    void** table = *static_cast<void***>(object);
    AcquireSRWLockExclusive(&g_vtableLock);
    VtableHook* available = nullptr;
    for (auto& slot : g_vtables) {
        if (slot.table == table) {
            if (table[15] == reinterpret_cast<void*>(&CreateDevice)) {
                const bool valid = slot.original != nullptr;
                ReleaseSRWLockExclusive(&g_vtableLock);
                return valid;
            }
            // A recreated D3D factory may reuse the same vtable address with
            // fresh native methods. Refresh its existing slot and original.
            available = &slot;
            break;
        }
        if (!slot.table && !available) available = &slot;
    }
    if (!available || !table[15] || table[15] == reinterpret_cast<void*>(&CreateDevice)) {
        ReleaseSRWLockExclusive(&g_vtableLock); return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(table + 15, sizeof(void*), PAGE_READWRITE, &old)) {
        ReleaseSRWLockExclusive(&g_vtableLock);
        return false;
    }
    available->table = table;
    available->original = reinterpret_cast<CreateDevice8>(table[15]);
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(table + 15), reinterpret_cast<void*>(&CreateDevice));
    DWORD ignored = 0;
    const BOOL restored = VirtualProtect(table + 15, sizeof(void*), old, &ignored);
    ReleaseSRWLockExclusive(&g_vtableLock);
    return restored != FALSE;
}

void* WINAPI CreateDirect3D(UINT sdk) {
    void* object = g_createD3D(sdk);
    const bool hooked = object && HookDeviceFactory(object);
    char message[256];
    sprintf_s(message, "Display factory: SDK=%u object=%p CreateDevice-hooked=%d", sdk, object, hooked);
    Record(message);
    return object;
}

FARPROC WINAPI ResolveDisplayProc(HMODULE module, LPCSTR name) {
    const auto original = g_getProcAddress(module, name);
    if (original && name && reinterpret_cast<ULONG_PTR>(name) > 0xFFFF &&
        !std::strcmp(name, "Direct3DCreate8") && module == GetModuleHandleW(L"d3d8.dll")) {
        // NetImmerse uses GetProcAddress, rather than the game's D3D import.
        // Verified native lookups: 0x6A9A07 and 0x6AA433.
        g_createD3D = reinterpret_cast<CreateD3D8>(original);
        return reinterpret_cast<FARPROC>(&CreateDirect3D);
    }
    return original;
}
} // namespace

void SetDisplayLogCallback(void (*callback)(const char*)) { g_log = callback; }
const char* DisplayHookStatus() { return g_status; }

DisplayMode GetDisplayMode() {
    return g_mode==ScreenMode::Fullscreen?DisplayMode::Fullscreen:
        g_mode==ScreenMode::Window?DisplayMode::Window:DisplayMode::Borderless;
}

bool SetDisplayMode(DisplayMode mode) {
    if(mode!=DisplayMode::Fullscreen && mode!=DisplayMode::Window && mode!=DisplayMode::Borderless)
        return Fail("Display mode change rejected: invalid mode.");
    if(!g_mainWindow || !IsWindow(g_mainWindow) || !g_backbufferWidth || !g_backbufferHeight)
        return Fail("Display mode change rejected: renderer window is not ready.");
    const ScreenMode previous=g_mode;
    const LONG style=GetWindowLongA(g_mainWindow,GWL_STYLE),extended=GetWindowLongA(g_mainWindow,GWL_EXSTYLE);
    g_mode=mode==DisplayMode::Fullscreen?ScreenMode::Fullscreen:mode==DisplayMode::Window?ScreenMode::Window:ScreenMode::Borderless;
    DWORD requestedStyle=BorderlessStyle(static_cast<DWORD>(style));
    if(g_mode==ScreenMode::Window)
        requestedStyle=(static_cast<DWORD>(style)&~WS_POPUP)|WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    SetLastError(0);
    if(!SetWindowLongA(g_mainWindow,GWL_STYLE,LONG(requestedStyle)) && GetLastError()!=0) {
        g_mode=previous;return Fail("Display mode change failed: window style.");
    }
    SetLastError(0);
    if(!SetWindowLongA(g_mainWindow,GWL_EXSTYLE,LONG(BorderlessExtendedStyle(DWORD(extended)))) && GetLastError()!=0) {
        SetWindowLongA(g_mainWindow,GWL_STYLE,style);g_mode=previous;
        return Fail("Display mode change failed: extended window style.");
    }
    SizeMainWindow(g_backbufferWidth,g_backbufferHeight);
    RECT client{};GetClientRect(g_mainWindow,&client);
    auto settings=Environment(L"MW_MODERN_SETTINGS");
    if(settings.empty()) {
        HMODULE own=nullptr;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
           reinterpret_cast<LPCWSTR>(&SetDisplayMode),&own)) {
            wchar_t path[32768]{};const DWORD length=GetModuleFileNameW(own,path,32768);
            if(length && length<32768 && length>=4 && !_wcsicmp(path+length-4,L".dll")) {
                std::wstring directory(path,length);const size_t slash=directory.find_last_of(L"\\/");
                if(slash!=std::wstring::npos) {
                    directory.resize(slash);const size_t parent=directory.find_last_of(L"\\/");
                    if(parent!=std::wstring::npos && !_wcsicmp(directory.c_str()+parent+1,L"bin")) directory.resize(parent);
                    settings=directory+L"\\settings.ini";
                }
            }
        }
    }
    bool saved=true;
    if(!settings.empty()) {
        const auto value=std::to_wstring(static_cast<int>(mode));
        saved=WritePrivateProfileStringW(L"Display",L"ScreenMode",value.c_str(),settings.c_str())!=FALSE;
    }
    char message[512];
    sprintf_s(message,"Display mode changed: mode=%s backbuffer=%ux%u client=%ldx%ld Windowed=1 settings=%s",
        ModeName(),g_backbufferWidth,g_backbufferHeight,client.right,client.bottom,settings.empty()?"not-configured":saved?"saved":"failed");
    Record(message);
    return true;
}

bool InstallDisplayHooks(HMODULE game) {
    if (!game) return Fail("Display error: missing executable module.");
    if (g_installed) return game == g_game;
    const auto mode = Lower(Environment(L"MW_MODERN_SCREEN_MODE"));
    if (mode.empty()) g_mode = ScreenMode::Borderless;
    else if (mode == L"fullscreen") g_mode = ScreenMode::Fullscreen;
    else if (mode == L"window" || mode == L"windowed") g_mode = ScreenMode::Window;
    else if (mode == L"borderless") g_mode = ScreenMode::Borderless;
    else return Fail("Display error: screen mode must be fullscreen, window or borderless.");
    if (!ParseDimension(L"MW_MODERN_WIDTH", g_width) || !ParseDimension(L"MW_MODERN_HEIGHT", g_height) ||
        (!!g_width != !!g_height) || (g_width && (g_width < 640 || g_height < 480))) {
        return Fail("Display error: dimensions must be paired, width640..16384 and height480..16384.");
    }
    EnableDpiAwareness();
    auto* query = FindImport(game, "RegQueryValueExA");
    auto* create = FindImport(game, "CreateWindowExA");
    auto* position = FindImport(game, "SetWindowPos");
    auto* d3d = FindImport(game, "Direct3DCreate8");
    auto* resolve = FindImport(game, "GetProcAddress");
    if (!query || !create || !position || !d3d || !resolve) return Fail("Display error: required native display imports not found.");
    if(!PatchKeyboardCooperativeLevel(game)) return false;
    g_game = game;
    g_queryRegistry = reinterpret_cast<decltype(g_queryRegistry)>(query);
    g_createWindow = reinterpret_cast<decltype(g_createWindow)>(create);
    g_setWindowPos = reinterpret_cast<decltype(g_setWindowPos)>(position);
    g_createD3D = reinterpret_cast<CreateD3D8>(d3d);
    g_getProcAddress = reinterpret_cast<decltype(g_getProcAddress)>(resolve);
    const bool hooked = PatchImport(game, "RegQueryValueExA", reinterpret_cast<void*>(&QueryRegistry)) &&
        PatchImport(game, "CreateWindowExA", reinterpret_cast<void*>(&CreateGameWindow)) &&
        PatchImport(game, "SetWindowPos", reinterpret_cast<void*>(&PositionGameWindow)) &&
        PatchImport(game, "Direct3DCreate8", reinterpret_cast<void*>(&CreateDirect3D)) &&
        PatchImport(game, "GetProcAddress", reinterpret_cast<void*>(&ResolveDisplayProc));
    if (!hooked) return Fail("Display error: could not install all display imports.");
    g_installed = true;
    char message[512];
    sprintf_s(message, "Display hooks: mode=%s startup=%ux%u DPI-aware=%d; subsequent Video resolution changes use native dimensions",
              ModeName(), g_width, g_height, IsProcessDPIAware());
    Record(message);
    return true;
}
