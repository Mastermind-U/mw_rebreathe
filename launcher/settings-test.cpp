// Exercise the actual launcher LoadSettings/Accept/SaveSettings against hidden
// controls and an isolated INI. Never opens the real launcher or settings.ini.
#include "settings.cpp"
#include <filesystem>
#include <stdexcept>

namespace {
unsigned g_checks=0;
void Check(bool ok,const char* message){++g_checks;if(!ok)throw std::runtime_error(message);}
}
int wmain(){
    MorrowindModernSettings::Context context;
    std::filesystem::path fixture;
    try {
        using namespace MorrowindModernSettings;
        context.desktopWidth=2880;context.desktopHeight=1800;
        Check(context.settings.screenMode==2&&context.settings.width==0&&context.settings.height==0,"Default borderless/native dimensions changed");
        Check(!context.settings.experimental&&context.settings.lockRam&&!context.settings.trace,"Stable defaults must keep RAM locking enabled and experimental preload disabled");
        const auto folder=std::filesystem::absolute(L"settings-fixtures");std::filesystem::create_directories(folder);
        fixture=folder/(std::wstring(L"launcher-")+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".ini");context.settingsPath=fixture.wstring();
        Check(WritePrivateProfileStringW(L"Display",L"Width",L"1280",fixture.c_str())&&WritePrivateProfileStringW(L"Display",L"Height",L"720",fixture.c_str())&&WritePrivateProfileStringW(L"Display",L"UIScalePercent",L"150",fixture.c_str())&&WritePrivateProfileStringW(L"Display",L"ScreenMode",L"2",fixture.c_str()),"Cannot seed own launcher fixture");
        Check(WritePrivateProfileStringW(L"FutureSection",L"UnknownKey",L"preserved",fixture.c_str())!=FALSE,"Cannot seed unknown setting");
        Check(WritePrivateProfileStringW(L"Game",L"PreloadAdjacentCells",L"1",fixture.c_str())!=FALSE,"Cannot seed legacy experimental setting");
        LoadSettings(context);
        Check(context.settings.width==1280&&context.settings.height==720&&context.settings.uiScale==1.5&&context.settings.screenMode==2,"LoadSettings did not preserve selected borderless resolution/scale");
        Check(!context.settings.experimental,"Legacy INI cannot enable adjacent-cell preload in stable profile");
        context.dialog=CreateWindowExW(0,L"STATIC",L"Hidden launcher fixture",WS_OVERLAPPEDWINDOW,0,0,500,400,nullptr,nullptr,context.instance,nullptr);
        Check(context.dialog!=nullptr,"Cannot create hidden settings controls");
        for(int id:{IdMode,IdResolution,IdScale})Check(AddControl(context,L"COMBOBOX",L"",CBS_DROPDOWNLIST,id)!=nullptr,"Cannot create fixture combo");
        for(int id:{IdRam,IdTrace})Check(AddControl(context,L"BUTTON",L"",BS_AUTOCHECKBOX,id)!=nullptr,"Cannot create fixture checkbox");
        for(int id:{IdMode,IdResolution,IdScale})for(unsigned i=0;i<3;++i)SendMessageW(context.Item(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"fixture"));
        context.resolutions={{2880,1800},{1280,720}};
        SendMessageW(context.Item(IdResolution),CB_SETCURSEL,1,0);SendMessageW(context.Item(IdScale),CB_SETCURSEL,0,0);
        for(int mode:{2,0,1}){
            SendMessageW(context.Item(IdMode),CB_SETCURSEL,mode,0);
            Check(IsWindowEnabled(context.Item(IdResolution))!=FALSE,"Resolution selector disabled for a mode");
            Check(Accept(context),"Actual launcher Accept rejected a valid mode/resolution");
            Check(context.settings.width==1280&&context.settings.height==720&&context.settings.screenMode==mode,"Launcher replaced chosen dimensions with desktop dimensions");
            Check(GetPrivateProfileIntW(L"Display",L"Width",0,fixture.c_str())==1280&&GetPrivateProfileIntW(L"Display",L"Height",0,fixture.c_str())==720&&GetPrivateProfileIntW(L"Display",L"ScreenMode",99,fixture.c_str())==static_cast<UINT>(mode),"SaveSettings stored wrong dimensions/mode");
            Check(GetPrivateProfileIntW(L"Game",L"PreloadAdjacentCells",99,fixture.c_str())==0,"Stable SaveSettings must clear legacy preload setting");
            wchar_t text[64]{};GetPrivateProfileStringW(L"FutureSection",L"UnknownKey",L"",text,64,fixture.c_str());Check(!std::wcscmp(text,L"preserved"),"SaveSettings lost unknown settings");
        }
        Check(!IsWindowVisible(context.dialog),"Launcher fixture became visible");
        DestroyWindow(context.dialog);context.dialog=nullptr;WritePrivateProfileStringW(nullptr,nullptr,nullptr,fixture.c_str());Check(DeleteFileW(fixture.c_str())!=FALSE,"Cannot delete own settings fixture");fixture.clear();
        std::printf("Passed %u checks: actual launcher LoadSettings/Accept/SaveSettings, borderless/fullscreen/window preserve selected non-desktop resolution, all selectors enabled, unknown INI keys survive, defaults unchanged; hidden controls and isolated INI only.\n",g_checks);return 0;
    }catch(const std::exception& error){
        std::fprintf(stderr,"FAILED: %s (Win32 %lu)\n",error.what(),GetLastError());if(context.dialog)DestroyWindow(context.dialog);if(!fixture.empty()){WritePrivateProfileStringW(nullptr,nullptr,nullptr,fixture.c_str());DeleteFileW(fixture.c_str());}return 1;
    }
}
