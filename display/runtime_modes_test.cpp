// Actual hidden Win32 parent/child windows; Direct3D factory is a recording
// fixture. No game process, monitor mode changes or actual settings.ini writes.
#include "display.cpp"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned g_checks=0,g_deviceCalls=0,g_sizes=0;
PresentParameters8 g_seen{};
void Require(bool value,const char* message){++g_checks;if(!value)throw std::runtime_error(message);}
LRESULT CALLBACK TestWindowProcedure(HWND window,UINT message,WPARAM wParam,LPARAM lParam){if(message==WM_SIZE)++g_sizes;return DefWindowProcA(window,message,wParam,lParam);}
HRESULT WINAPI TestCreateDevice(void*,UINT,DWORD,HWND,DWORD,PresentParameters8* pp,void** out){++g_deviceCalls;g_seen=*pp;if(out)*out=reinterpret_cast<void*>(1);return S_OK;}
RECT Client(HWND window){RECT r{};Require(GetClientRect(window,&r)!=FALSE,"GetClientRect failed");return r;}
RECT Frame(HWND window){RECT r{};Require(GetWindowRect(window,&r)!=FALSE,"GetWindowRect failed");return r;}
void EqualSize(RECT a,RECT b,const char* message){Require(a.right-a.left==b.right-b.left&&a.bottom-a.top==b.bottom-b.top,message);}
void CheckChildren(HWND main,HWND child){EqualSize(Client(main),Client(child),"Render child must fill the actual parent client area");Require(!IsWindowVisible(main)&&!IsWindowVisible(child),"Test windows must stay hidden");}
std::vector<unsigned char> Bytes(const std::filesystem::path& path){
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    Require(f!=INVALID_HANDLE_VALUE,"Cannot read isolated settings fixture");
    const DWORD size=GetFileSize(f,nullptr);std::vector<unsigned char> bytes(size);DWORD read=0;
    const BOOL ok=ReadFile(f,bytes.data(),size,&read,nullptr);CloseHandle(f);
    Require(ok&&read==size,"Settings fixture read failed");return bytes;
}
void SeedSettings(const std::filesystem::path& path){
    const std::wstring text=L"\xFEFF[Display]\r\nWidth=3440\r\nHeight=1440\r\nUIScalePercent=175\r\nScreenMode=2\r\nFutureDisplayKey=preserved\r\n[Game]\r\nLockRam=0\r\nTrace=1\r\nPreloadAdjacentCells=0\r\n[ExternalTool]\r\nFutureKey=Не менять\r\n";
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
    Require(f!=INVALID_HANDLE_VALUE,"Cannot create isolated settings fixture");DWORD written=0;
    const DWORD size=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    const BOOL ok=WriteFile(f,text.data(),size,&written,nullptr);CloseHandle(f);Require(ok&&written==size,"Cannot seed isolated settings fixture");
}
void CheckSettings(const std::filesystem::path& path,int mode){
    Require(GetPrivateProfileIntW(L"Display",L"ScreenMode",99,path.c_str())==static_cast<UINT>(mode),"Wrong persisted ScreenMode enumeration");
    Require(GetPrivateProfileIntW(L"Display",L"Width",0,path.c_str())==3440&&GetPrivateProfileIntW(L"Display",L"Height",0,path.c_str())==1440,"Mode change overwrote selected resolution");
    Require(GetPrivateProfileIntW(L"Display",L"UIScalePercent",0,path.c_str())==175,"Mode change overwrote UI scale");
    Require(GetPrivateProfileIntW(L"Game",L"LockRam",99,path.c_str())==0&&GetPrivateProfileIntW(L"Game",L"Trace",99,path.c_str())==1&&GetPrivateProfileIntW(L"Game",L"PreloadAdjacentCells",99,path.c_str())==0,"Mode change overwrote game settings");
    wchar_t text[128]{};GetPrivateProfileStringW(L"Display",L"FutureDisplayKey",L"",text,128,path.c_str());Require(!std::wcscmp(text,L"preserved"),"Mode change lost unknown display key");
    GetPrivateProfileStringW(L"ExternalTool",L"FutureKey",L"",text,128,path.c_str());Require(!std::wcscmp(text,L"Не менять"),"Mode change lost unknown Unicode setting/section");
}
}

int wmain(){
    HWND window=nullptr;std::filesystem::path fixture;
    const auto originalSettings=Environment(L"MW_MODERN_SETTINGS");
    try {
        EnableDpiAwareness();g_game=GetModuleHandleW(nullptr);g_mode=ScreenMode::Borderless;g_width=640;g_height=480;
        DEVMODEW before{};before.dmSize=sizeof(before);Require(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&before)!=FALSE,"Cannot capture current desktop mode");
        WNDCLASSA cls{};cls.hInstance=g_game;cls.lpfnWndProc=&TestWindowProcedure;cls.lpszClassName="Morrowind";
        Require(RegisterClassA(&cls)!=0,"Cannot register isolated window class");
        window=CreateGameWindow(WS_EX_CLIENTEDGE,"Morrowind","Runtime display fixture",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,g_game,nullptr);
        Require(window&&window==g_mainWindow,"Cannot create hidden main window");
        MONITORINFO monitor{sizeof(monitor)};Require(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTOPRIMARY),&monitor)!=FALSE,"Cannot query fixture monitor");
        RECT initialFrame=Frame(window);Require(EqualRect(&initialFrame,&monitor.rcMonitor)!=FALSE,"Initial borderless frame must fill monitor even before device creation at lower selected resolution");
        EqualSize(Client(window),monitor.rcMonitor,"Initial borderless client must fill monitor");
        HWND child=CreateGameWindow(0,"Morrowind","Render child",WS_CHILD|WS_CLIPCHILDREN,0,0,320,240,window,nullptr,g_game,nullptr);
        Require(child!=nullptr,"Cannot create hidden render child");
        void* table[16]{};table[15]=reinterpret_cast<void*>(&TestCreateDevice);struct Object{void** table;} object{table};
        Require(HookDeviceFactory(&object),"Cannot hook isolated Direct3D factory");
        PresentParameters8 pp{};pp.width=640;pp.height=480;pp.deviceWindow=child;pp.windowed=FALSE;pp.fullscreenRefreshRate=144;pp.fullscreenInterval=1;
        void* device=nullptr;
        Require(SUCCEEDED(reinterpret_cast<CreateDevice8>(table[15])(&object,0,1,window,0,&pp,&device)),"Isolated CreateDevice failed");
        Require(g_seen.windowed&&g_seen.width==640&&g_seen.height==480&&g_seen.fullscreenRefreshRate==0&&g_seen.fullscreenInterval==0,"Modern presentation must be windowed with unchanged selected backbuffer");
        Require(g_deviceWindow==child,"Actual presentation child window was not tracked");CheckChildren(window,child);
        const auto directory=std::filesystem::absolute(std::filesystem::path(L"runtime-fixtures"));std::filesystem::create_directories(directory);
        fixture=directory/(std::wstring(L"settings-")+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".ini");
        SeedSettings(fixture);Require(SetEnvironmentVariableW(L"MW_MODERN_SETTINGS",fixture.c_str())!=FALSE,"Cannot set isolated settings path");
        Require(SetDisplayMode(DisplayMode::Fullscreen),"Cannot switch runtime fullscreen mode");
        Require(GetDisplayMode()==DisplayMode::Fullscreen,"Fullscreen API state mismatch");EqualSize(Client(window),monitor.rcMonitor,"Fullscreen must fill the physical monitor");
        RECT frame=Frame(window);Require(EqualRect(&frame,&monitor.rcMonitor)!=FALSE,"Fullscreen frame must coincide with monitor bounds");CheckChildren(window,child);CheckSettings(fixture,0);
        Require((GetWindowLongA(window,GWL_STYLE)&(WS_CAPTION|WS_THICKFRAME))==0,"Fullscreen must be undecorated");
        Require(PositionGameWindow(child,nullptr,0,0,640,480,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Native child-size interception failed");CheckChildren(window,child);
        Require(SetDisplayMode(DisplayMode::Borderless),"Cannot switch runtime borderless mode");
        Require(GetDisplayMode()==DisplayMode::Borderless,"Borderless API state mismatch");RECT client=Client(window);EqualSize(client,monitor.rcMonitor,"Borderless must fill monitor with lower selected render resolution");
        frame=Frame(window);Require(EqualRect(&frame,&monitor.rcMonitor)!=FALSE,"Borderless frame must coincide with monitor bounds");
        Require((GetWindowLongA(window,GWL_STYLE)&(WS_CAPTION|WS_THICKFRAME))==0,"Borderless must stay undecorated");
        Require(g_backbufferWidth==640&&g_backbufferHeight==480&&g_seen.width==640&&g_seen.height==480,"Monitor-fill borderless must preserve selected backbuffer");
        Require(PositionGameWindow(child,nullptr,40,50,640,480,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Borderless native child resize interception failed");CheckChildren(window,child);
        Require(PositionGameWindow(window,nullptr,40,50,640,480,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Borderless native parent resize interception failed");
        frame=Frame(window);Require(EqualRect(&frame,&monitor.rcMonitor)!=FALSE,"Native selected-resolution resize cannot shrink monitor-fill borderless");CheckChildren(window,child);
        Require(PositionGameWindow(child,nullptr,40,50,640,480,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE|SWP_NOSIZE)!=FALSE,"Borderless native child flags interception failed");CheckChildren(window,child);CheckSettings(fixture,2);
        Require(SetDisplayMode(DisplayMode::Window),"Cannot switch runtime framed mode");
        Require(GetDisplayMode()==DisplayMode::Window,"Window API state mismatch");
        Require((GetWindowLongA(window,GWL_STYLE)&WS_CAPTION)==WS_CAPTION&&!(GetWindowLongA(window,GWL_STYLE)&WS_POPUP),"Framed mode must restore caption and remove popup style");
        client=Client(window);Require(client.right==640&&client.bottom==480,"Framed mode must preserve a fitting selected client size");CheckChildren(window,child);CheckSettings(fixture,1);
        const unsigned sizes=g_sizes;
        Require(SetWindowPos(window,nullptr,0,0,900,650,SWP_NOACTIVATE|SWP_NOZORDER)!=FALSE,"Cannot resize fixture parent");
        Require(g_sizes>sizes,"Actual WM_SIZE was not dispatched");CheckChildren(window,child);
        for(unsigned pass=0;pass<20;++pass)for(DisplayMode mode:{DisplayMode::Fullscreen,DisplayMode::Borderless,DisplayMode::Window}){
            Require(SetDisplayMode(mode),"Repeated runtime mode switch failed");CheckChildren(window,child);Require(g_backbufferWidth==640&&g_backbufferHeight==480,"Mode switch changed backbuffer dimensions");
            if(mode==DisplayMode::Window) {const RECT selected=Client(window);Require(selected.right==640&&selected.bottom==480,"Repeated framed mode restores selected client size");}
            else {const RECT filled=Frame(window);Require(EqualRect(&filled,&monitor.rcMonitor)!=FALSE,"Every fullscreen/borderless mode switch must fill monitor");EqualSize(Client(window),monitor.rcMonitor,"Every monitor-fill mode keeps client monitor-sized");}
        }
        Require(g_deviceCalls==1,"Style-only runtime switch unnecessarily recreated the device");CheckSettings(fixture,1);
        const auto saved=Bytes(fixture);const auto mode=GetDisplayMode();frame=Frame(window);const LONG style=GetWindowLongA(window,GWL_STYLE);
        Require(!SetDisplayMode(static_cast<DisplayMode>(99)),"Invalid mode must be rejected");
        RECT unchanged=Frame(window);Require(GetDisplayMode()==mode&&GetWindowLongA(window,GWL_STYLE)==style&&EqualRect(&frame,&unchanged),"Invalid mode changed live window state");Require(Bytes(fixture)==saved,"Invalid mode changed settings file");
        g_backbufferWidth=0;Require(!SetDisplayMode(DisplayMode::Fullscreen),"Unready renderer must reject mode changes");Require(Bytes(fixture)==saved,"Rejected unready mode changed settings");g_backbufferWidth=640;
        DEVMODEW after{};after.dmSize=sizeof(after);Require(EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&after)!=FALSE,"Cannot recapture desktop mode");
        Require(before.dmPelsWidth==after.dmPelsWidth&&before.dmPelsHeight==after.dmPelsHeight&&before.dmBitsPerPel==after.dmBitsPerPel&&before.dmDisplayFrequency==after.dmDisplayFrequency,"Runtime window modes changed desktop display mode");
        DestroyWindow(window);window=nullptr;Require(!g_mainWindow&&!g_deviceWindow&&!g_previousWindowProcedure,"Window destruction did not clear display lifecycle pointers");
        Require(!SetDisplayMode(DisplayMode::Window),"Destroyed window must reject mode changes");
        UnregisterClassA("Morrowind",g_game);WritePrivateProfileStringW(nullptr,nullptr,nullptr,fixture.c_str());Require(DeleteFileW(fixture.c_str())!=FALSE,"Cannot delete own isolated settings fixture");fixture.clear();
        SetEnvironmentVariableW(L"MW_MODERN_SETTINGS",originalSettings.empty()?nullptr:originalSettings.c_str());
        std::printf("Passed %u checks: actual hidden Win32 parent/child styles, sizes, WM_SIZE, 60 mode changes, scaled fullscreen, monitor-fill borderless at lower render resolution, framed window; one windowed mock device; numeric mode persistence preserves resolution/UI scale/Game/unknown Unicode keys; invalid/lifecycle rejection; desktop mode unchanged.\n",g_checks);return 0;
    }catch(const std::exception& error){
        std::fprintf(stderr,"FAILED: %s (Win32 %lu)\n",error.what(),GetLastError());
        if(window)DestroyWindow(window);if(!fixture.empty()){WritePrivateProfileStringW(nullptr,nullptr,nullptr,fixture.c_str());DeleteFileW(fixture.c_str());}
        SetEnvironmentVariableW(L"MW_MODERN_SETTINGS",originalSettings.empty()?nullptr:originalSettings.c_str());return 1;
    }
}
