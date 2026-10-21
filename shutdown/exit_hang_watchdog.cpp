#define NOMINMAX
#include "exit_hang_watchdog.h"
#include <dbghelp.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#pragma comment(lib,"dbghelp.lib")
static_assert(sizeof(void*)==4,"The watchdog runs in the x86 runner");
namespace {
constexpr uintptr_t ImageBase=0x400000,WorldSlot=0x7C67DC;
constexpr ULONGLONG GraceMs=2000;
#ifdef EXIT_HANG_WATCHDOG_TESTING
bool g_fakeClock=false,g_failDump=false,g_unstableWorld=false;
bool g_failedRead=false,g_shortRead=false;
ULONGLONG g_tick=0;
unsigned g_worldReads=0;
#endif
ULONGLONG Tick()noexcept{
#ifdef EXIT_HANG_WATCHDOG_TESTING
    if(g_fakeClock)return g_tick;
#endif
    return GetTickCount64();
}
BOOL ReadNative(HANDLE process,uintptr_t address,void* output,SIZE_T size,SIZE_T* read)noexcept{
#ifdef EXIT_HANG_WATCHDOG_TESTING
    if(g_failedRead&&address==WorldSlot){*read=0;SetLastError(ERROR_PARTIAL_COPY);return FALSE;}
#endif
    const BOOL result=ReadProcessMemory(process,reinterpret_cast<const void*>(address),output,size,read);
#ifdef EXIT_HANG_WATCHDOG_TESTING
    if(result&&g_shortRead&&address==WorldSlot&&size)*read=size-1;
#endif
    return result;
}
bool Read(HANDLE process,uintptr_t address,void* output,SIZE_T size)noexcept{
    SIZE_T read=0;
    if(!ReadNative(process,address,output,size,&read)||read!=size)return false;
#ifdef EXIT_HANG_WATCHDOG_TESTING
    if(g_unstableWorld&&address==WorldSlot&&size==4&&++g_worldReads%2==0)*static_cast<DWORD*>(output)^=0x1000;
#endif
    return true;
}
bool Flag(const wchar_t* name)noexcept{wchar_t value[4]{};return GetEnvironmentVariableW(name,value,4)==1&&value[0]==L'1';}
bool AbsoluteFile(const std::wstring& path)noexcept{
    const bool drive=path.size()>3&&((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/');
    const bool unc=path.size()>5&&path[0]==L'\\'&&path[1]==L'\\';
    const auto slash=path.find_last_of(L"\\/");return (drive||unc)&&slash!=std::wstring::npos&&slash+1<path.size();
}
bool SupportedImage(HANDLE process)noexcept{
    IMAGE_DOS_HEADER dos{};if(!Read(process,ImageBase,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<static_cast<LONG>(sizeof(dos))||dos.e_lfanew>0x1000)return false;
    IMAGE_NT_HEADERS32 nt{};return Read(process,ImageBase+static_cast<uintptr_t>(dos.e_lfanew),&nt,sizeof(nt))&&nt.Signature==IMAGE_NT_SIGNATURE&&nt.FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC&&nt.OptionalHeader.ImageBase==ImageBase&&nt.OptionalHeader.SizeOfImage>=0x3C6800;
}
bool NativeQuit(HANDLE process)noexcept{
    DWORD first=0,second=0;BYTE quit=0;
    if(!Read(process,WorldSlot,&first,4)||first<0x10000||first>0x7FFEFF24)return false;
    if(!Read(process,static_cast<uintptr_t>(first)+0xDB,&quit,1)||!Read(process,WorldSlot,&second,4))return false;
    return first==second&&quit==1;
}
BOOL WriteDump(HANDLE process,DWORD pid,HANDLE file)noexcept{
#ifdef EXIT_HANG_WATCHDOG_TESTING
    if(g_failDump){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
#endif
    const auto type=static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo|MiniDumpWithProcessThreadData|MiniDumpWithUnloadedModules);
    return MiniDumpWriteDump(process,pid,file,type,nullptr,nullptr,nullptr);
}
}
struct ExitHangWatchdog::State {
    HANDLE child=nullptr;
    DWORD pid=0;
    bool enabled=false,quitObserved=false,attempted=false;
    ULONGLONG quitTick=0;
    std::wstring logPath;
    char status[4096]="not initialized";
    void Say(const char* message,DWORD error=0,const std::wstring* path=nullptr)noexcept{
        char encoded[3072]{};
        if(path&&!WideCharToMultiByte(CP_UTF8,0,path->c_str(),-1,encoded,static_cast<int>(sizeof(encoded)),nullptr,nullptr))strcpy_s(encoded,"<path exceeds status capacity>");
        if(path)sprintf_s(status,"%s pid=%lu error=%lu path=%s",message,pid,error,encoded);
        else sprintf_s(status,"%s pid=%lu error=%lu",message,pid,error);
    }
    void Capture(){
        attempted=true;
        const ULONGLONG tick=Tick();HANDLE file=INVALID_HANDLE_VALUE;std::wstring path;
        for(unsigned collision=0;collision<32;++collision){
            wchar_t suffix[128]{};swprintf_s(suffix,L".hang-%lu-%llu-%u.dmp",pid,tick,collision);path=logPath+suffix;
            file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE)break;
            const DWORD error=GetLastError();if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS){Say("hang dump create failed",error,&path);return;}
        }
        if(file==INVALID_HANDLE_VALUE){Say("hang dump names exhausted",ERROR_FILE_EXISTS,&path);return;}
        const BOOL written=WriteDump(child,pid,file);const DWORD error=written?ERROR_SUCCESS:GetLastError();CloseHandle(file);
        Say(written?"hang dump captured":"hang dump write failed",error,&path);
    }
};
ExitHangWatchdog::ExitHangWatchdog()noexcept{state_=new(std::nothrow) State;}
ExitHangWatchdog::~ExitHangWatchdog()noexcept{delete state_;}
bool ExitHangWatchdog::Initialize(HANDLE exactChild,bool probe)noexcept{
    const DWORD incoming=GetLastError();bool result=false;
    try{
        if(state_){State& s=*state_;
            if(s.enabled){result=s.child==exactChild;if(!result)s.Say("different child rejected",ERROR_INVALID_HANDLE);}
            else if(probe||Flag(L"MW_MODERN_PROBE"))s.Say("disabled for probe");
            else if(!Flag(L"MW_MODERN_TRACE"))s.Say("disabled without trace");
            else if(!exactChild||exactChild==INVALID_HANDLE_VALUE||WaitForSingleObject(exactChild,0)!=WAIT_TIMEOUT)s.Say("child is not alive",ERROR_INVALID_HANDLE);
            else{
                s.pid=GetProcessId(exactChild);wchar_t image[32768]{};DWORD length=static_cast<DWORD>(sizeof(image)/sizeof(image[0]));
                if(!s.pid||!QueryFullProcessImageNameW(exactChild,0,image,&length))s.Say("child identity unavailable",GetLastError());
                else{
                    const wchar_t* name=std::wcsrchr(image,L'\\');name=name?name+1:image;
                    if(_wcsicmp(name,L"Morrowind.exe"))s.Say("child name rejected",ERROR_BAD_EXE_FORMAT);
                    else if(!SupportedImage(exactChild))s.Say("unsupported child PE32 image",ERROR_BAD_EXE_FORMAT);
                    else{
                        wchar_t path[32768]{};const DWORD n=GetEnvironmentVariableW(L"MW_MODERN_LOG",path,32768);
                        if(!n||n>=32768||!AbsoluteFile(path))s.Say("absolute log filename required",ERROR_INVALID_NAME);
                        else{s.child=exactChild;s.logPath=path;s.enabled=true;s.Say("armed; waiting for verified native quit");result=true;}
                    }
                }
            }
        }
    }catch(...){if(state_)state_->Say("initialization failed",ERROR_NOT_ENOUGH_MEMORY);}SetLastError(incoming);return result;
}
void ExitHangWatchdog::Poll()noexcept{
    const DWORD incoming=GetLastError();
    try{
        if(state_){State& s=*state_;
            if(s.enabled&&!s.attempted&&WaitForSingleObject(s.child,0)==WAIT_TIMEOUT){
                if(!s.quitObserved&&NativeQuit(s.child)){s.quitObserved=true;s.quitTick=Tick();s.Say("verified native quit; grace timer started");}
                if(s.quitObserved&&Tick()-s.quitTick>=GraceMs&&WaitForSingleObject(s.child,0)==WAIT_TIMEOUT)s.Capture();
            }
        }
    }catch(...){if(state_){state_->attempted=true;state_->Say("capture failed",ERROR_NOT_ENOUGH_MEMORY);}}SetLastError(incoming);
}
const char* ExitHangWatchdog::Status()const noexcept{return state_?state_->status:"watchdog allocation failed";}
#ifdef EXIT_HANG_WATCHDOG_TESTING
void SetExitHangWatchdogTestClock(bool enabled,ULONGLONG tick)noexcept{g_fakeClock=enabled;g_tick=tick;}
void SetExitHangWatchdogTestFailures(bool dumpFailure,bool unstableWorld)noexcept{g_failDump=dumpFailure;g_unstableWorld=unstableWorld;g_worldReads=0;}
void SetExitHangWatchdogTestReads(bool failedRead,bool shortRead)noexcept{g_failedRead=failedRead;g_shortRead=shortRead;}
#endif
