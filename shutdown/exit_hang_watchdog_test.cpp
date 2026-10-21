#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#ifdef EXIT_HANG_FIXTURE
volatile unsigned char g_imageSpace[0x400000]{};
unsigned char g_world[0x200]{};
DWORD WINAPI FixtureWorker(void* event){volatile DWORD stackMarker=0x5A17CAFE;WaitForSingleObject(event,INFINITE);return stackMarker==0x5A17CAFE?0:1;}
int wmain(int argc,wchar_t** argv){
    if(argc!=6||std::wcscmp(argv[1],L"--child"))return 2;
    if(GetModuleHandleW(nullptr)!=reinterpret_cast<HMODULE>(0x400000))return 3;
    g_imageSpace[0]=1;g_imageSpace[sizeof(g_imageSpace)-1]=1;
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[2]),stop=OpenEventW(SYNCHRONIZE,FALSE,argv[3]),clear=OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,argv[4]);
    if(!ready||!stop||!clear)return 4;
    const int mode=_wtoi(argv[5]);g_world[0xDB]=mode==0?0:mode==5?2:1;
    auto* slot=reinterpret_cast<unsigned char* volatile*>(0x7C67DC);
    auto* unreadable=mode==7?static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS)):nullptr;if(mode==7&&!unreadable)return 7;
    *slot=mode==2?nullptr:mode==3?reinterpret_cast<unsigned char*>(0x1000):mode==4?reinterpret_cast<unsigned char*>(0x7FFFFFFF):mode==7?unreadable:g_world;
    if(mode==6){auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(0x400000);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(0x400000+dos->e_lfanew);DWORD old=0,ignored=0;VirtualProtect(nt,sizeof(*nt),PAGE_READWRITE,&old);nt->OptionalHeader.ImageBase=0x500000;VirtualProtect(nt,sizeof(*nt),old,&ignored);}
    HMODULE unloaded=LoadLibraryW(L"version.dll");if(unloaded)FreeLibrary(unloaded);
    HANDLE worker=CreateThread(nullptr,0,&FixtureWorker,stop,0,nullptr);if(!worker)return 5;
    SetEvent(ready);HANDLE events[]{stop,clear};
    for(;;){const DWORD result=WaitForMultipleObjects(2,events,FALSE,INFINITE);if(result==WAIT_OBJECT_0)break;if(result==WAIT_OBJECT_0+1){*slot=nullptr;ResetEvent(clear);}else return 6;}
    WaitForSingleObject(worker,5000);CloseHandle(worker);CloseHandle(ready);CloseHandle(stop);CloseHandle(clear);if(unreadable)VirtualFree(unreadable,0,MEM_RELEASE);return 0;
}
#else
#include "exit_hang_watchdog.h"
#include <dbghelp.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
static_assert(sizeof(void*)==4,"x86 fixture driver");
namespace {
unsigned checks=0,childSerial=0;
constexpr DWORD Marker=0x13579BDF;
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::wstring Full(const wchar_t* path){wchar_t out[32768]{};Check(GetFullPathNameW(path,32768,out,nullptr)!=0,"Absolute fixture path failed");return out;}
struct Child {
    PROCESS_INFORMATION process{};HANDLE ready=nullptr,stop=nullptr,clear=nullptr;
    Child(const std::wstring& executable,int mode){
        const auto token=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++childSerial);
        const auto rn=L"Local\\MWExitHang-ready-"+token,sn=L"Local\\MWExitHang-stop-"+token,cn=L"Local\\MWExitHang-clear-"+token;
        ready=CreateEventW(nullptr,TRUE,FALSE,rn.c_str());stop=CreateEventW(nullptr,TRUE,FALSE,sn.c_str());clear=CreateEventW(nullptr,TRUE,FALSE,cn.c_str());Check(ready&&stop&&clear,"Fixture event creation failed");
        std::wstring args=L"\""+executable+L"\" --child \""+rn+L"\" \""+sn+L"\" \""+cn+L"\" "+std::to_wstring(mode);
        STARTUPINFOW startup{};startup.cb=sizeof(startup);Check(CreateProcessW(executable.c_str(),args.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Fixture CreateProcess failed");
        Check(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0,"Fixture did not become ready");
    }
    bool Alive()const{return WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT;}
    void CloseNormally(){SetEvent(stop);Check(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0,"Fixture did not exit after its own stop event");DWORD code=1;Check(GetExitCodeProcess(process.hProcess,&code)&&code==0,"Fixture exit code not zero");}
    ~Child(){if(stop)SetEvent(stop);if(process.hProcess){WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hThread);CloseHandle(process.hProcess);}if(ready)CloseHandle(ready);if(stop)CloseHandle(stop);if(clear)CloseHandle(clear);}
};
struct SavedEnvironment {
    const wchar_t* name;std::wstring value;bool existed=false;
    explicit SavedEnvironment(const wchar_t* key):name(key){wchar_t b[32768]{};const DWORD n=GetEnvironmentVariableW(name,b,32768);existed=n>0&&n<32768;if(existed)value=b;}
    ~SavedEnvironment(){SetEnvironmentVariableW(name,existed?value.c_str():nullptr);}
};
std::vector<std::wstring> Dumps(const std::wstring& log){std::vector<std::wstring> out;WIN32_FIND_DATAW data{};HANDLE search=FindFirstFileW((log+L".hang-*.dmp").c_str(),&data);if(search!=INVALID_HANDLE_VALUE){const auto slash=log.find_last_of(L"\\/");do{out.push_back(log.substr(0,slash+1)+data.cFileName);}while(FindNextFileW(search,&data));FindClose(search);}return out;}
std::vector<unsigned char> File(const std::wstring& path){HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);Check(f!=INVALID_HANDLE_VALUE,"Cannot open captured dump");LARGE_INTEGER size{};Check(GetFileSizeEx(f,&size)&&size.QuadPart>=0&&size.QuadPart<64*1024*1024,"Dump size invalid");std::vector<unsigned char> b(static_cast<size_t>(size.QuadPart));DWORD n=0;Check(b.empty()||ReadFile(f,b.data(),static_cast<DWORD>(b.size()),&n,nullptr)&&n==b.size(),"Dump read failed");CloseHandle(f);return b;}
template<class T>T At(const std::vector<unsigned char>& b,size_t offset){Check(offset<=b.size()&&sizeof(T)<=b.size()-offset,"Dump field outside file");T v{};std::memcpy(&v,b.data()+offset,sizeof(v));return v;}
void VerifyDump(const std::wstring& file,DWORD pid){
    const auto bytes=File(file);const auto header=At<MINIDUMP_HEADER>(bytes,0);Check(header.Signature==MINIDUMP_SIGNATURE,"Not a valid minidump");
    MINIDUMP_DIRECTORY threads{},info{},misc{},unloaded{};bool exception=false;
    for(ULONG i=0;i<header.NumberOfStreams;++i){const auto dir=At<MINIDUMP_DIRECTORY>(bytes,header.StreamDirectoryRva+i*sizeof(MINIDUMP_DIRECTORY));if(dir.StreamType==ThreadListStream)threads=dir;if(dir.StreamType==ThreadInfoListStream)info=dir;if(dir.StreamType==MiscInfoStream)misc=dir;if(dir.StreamType==UnloadedModuleListStream)unloaded=dir;if(dir.StreamType==ExceptionStream)exception=true;}
    Check(threads.Location.Rva&&info.Location.Rva&&misc.Location.Rva,"Required target thread/context streams missing");Check(!exception,"Hang snapshot fabricated an exception context");
    const DWORD count=At<DWORD>(bytes,threads.Location.Rva);Check(count>=2,"Not all synthetic target threads captured");
    bool marker=false;
    for(DWORD i=0;i<count;++i){const auto thread=At<MINIDUMP_THREAD>(bytes,threads.Location.Rva+4+i*sizeof(MINIDUMP_THREAD));Check(thread.ThreadContext.DataSize>=204,"Target x86 context missing");const DWORD eip=At<DWORD>(bytes,thread.ThreadContext.Rva+184),esp=At<DWORD>(bytes,thread.ThreadContext.Rva+196);Check(eip&&thread.Stack.Memory.DataSize&&esp>=thread.Stack.StartOfMemoryRange&&esp<thread.Stack.StartOfMemoryRange+thread.Stack.Memory.DataSize,"Target thread stack/context inconsistent");const size_t begin=thread.Stack.Memory.Rva;const DWORD size=thread.Stack.Memory.DataSize;Check(begin<=bytes.size()&&size<=bytes.size()-begin,"Target stack not present in dump");for(DWORD j=0;j+4<=size;++j){DWORD word=0;std::memcpy(&word,bytes.data()+begin+j,4);if(word==0x5A17CAFE)marker=true;}}
    Check(marker,"Synthetic child stack marker missing");const auto mi=At<MINIDUMP_MISC_INFO>(bytes,misc.Location.Rva);Check((mi.Flags1&MINIDUMP_MISC1_PROCESS_ID)&&mi.ProcessId==pid,"Dump belongs to a different process");
    const auto ti=At<MINIDUMP_THREAD_INFO_LIST>(bytes,info.Location.Rva);Check(ti.NumberOfEntries>=2,"ThreadInfo list missing target threads");Check(unloaded.Location.Rva!=0,"UnloadedModules stream missing");
}
bool Init(ExitHangWatchdog& w,HANDLE child,bool probe=false){SetLastError(Marker);const bool ok=w.Initialize(child,probe);Check(GetLastError()==Marker,"Initialize changed LastError");return ok;}
void Poll(ExitHangWatchdog& w){SetLastError(Marker);w.Poll();Check(GetLastError()==Marker,"Poll changed LastError");}
std::wstring Log(const std::wstring& dir,const wchar_t* name){const auto p=dir+L"\\"+name+L".log";Check(SetEnvironmentVariableW(L"MW_MODERN_LOG",p.c_str())!=FALSE,"Test log environment failed");return p;}
void DisabledAndGuards(const std::wstring& exe,const std::wstring& dir){
    Child child(exe,1);SetEnvironmentVariableW(L"MW_MODERN_TRACE",L"1");SetEnvironmentVariableW(L"MW_MODERN_PROBE",nullptr);const auto log=Log(dir,L"disabled");
    const wchar_t* invalidTraces[]{nullptr,L"0",L"01",L"true"};for(const wchar_t* trace:invalidTraces){SetEnvironmentVariableW(L"MW_MODERN_TRACE",trace);ExitHangWatchdog w;Check(!Init(w,child.process.hProcess),"Non-exact trace flag accepted");Poll(w);}
    SetEnvironmentVariableW(L"MW_MODERN_TRACE",L"1");{ExitHangWatchdog w;Check(!Init(w,child.process.hProcess,true),"Explicit probe accepted");}
    SetEnvironmentVariableW(L"MW_MODERN_PROBE",L"1");{ExitHangWatchdog w;Check(!Init(w,child.process.hProcess),"Probe environment accepted");}SetEnvironmentVariableW(L"MW_MODERN_PROBE",nullptr);
    const wchar_t* invalidPaths[]{nullptr,L"game.log",L"\\game.log",L"C:game.log",L"C:\\"};for(const wchar_t* path:invalidPaths){SetEnvironmentVariableW(L"MW_MODERN_LOG",path);ExitHangWatchdog w;Check(!Init(w,child.process.hProcess),"Invalid/missing log filename accepted");}
    Log(dir,L"disabled");{ExitHangWatchdog w;Check(!Init(w,nullptr)&&!Init(w,INVALID_HANDLE_VALUE),"Invalid child HANDLE accepted");}
    Check(Dumps(log).empty(),"Disabled watchdog wrote a dump");child.CloseNormally();{ExitHangWatchdog w;Check(!Init(w,child.process.hProcess),"Exited child accepted");}
    const auto wrong=dir+L"\\Different.exe";Check(CopyFileW(exe.c_str(),wrong.c_str(),TRUE)!=FALSE,"Wrong-name fixture copy failed");Child other(wrong,1);{ExitHangWatchdog w;Check(!Init(w,other.process.hProcess),"Different executable basename accepted");}other.CloseNormally();
    Child badPe(exe,6);{ExitHangWatchdog w;Check(!Init(w,badPe.process.hProcess),"Wrong runtime PE base accepted");}badPe.CloseNormally();
}
void NoQuitAndWorldGuards(const std::wstring& exe,const std::wstring& dir){
    for(int mode:{0,2,3,4,5,7}){Child child(exe,mode);const auto log=Log(dir,(L"no-quit-"+std::to_wstring(mode)).c_str());if(mode==7){BYTE byte=0;SIZE_T read=0;DWORD world=0;Check(ReadProcessMemory(child.process.hProcess,reinterpret_cast<void*>(0x7C67DC),&world,4,&read)&&read==4&&world!=0,"Unreadable-World fixture pointer missing");Check(!ReadProcessMemory(child.process.hProcess,reinterpret_cast<void*>(world+0xDB),&byte,1,&read)||read!=1,"Unreadable-World fixture address unexpectedly readable");}ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);Check(Init(w,child.process.hProcess),"Normal fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,20000);Poll(w);Check(Dumps(log).empty()&&child.Alive(),"No valid native quit caused a capture or exit");child.CloseNormally();}
    for(bool shortRead:{false,true}){Child child(exe,1);const auto log=Log(dir,shortRead?L"short-read":L"failed-read");ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);SetExitHangWatchdogTestReads(!shortRead,shortRead);Check(Init(w,child.process.hProcess),"Read-failure fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,20000);Poll(w);Check(Dumps(log).empty()&&child.Alive(),"Failed/short RPM started quit timer");SetExitHangWatchdogTestReads(false,false);child.CloseNormally();}
    Child child(exe,1);const auto log=Log(dir,L"stable-world");ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);SetExitHangWatchdogTestFailures(false,true);Check(Init(w,child.process.hProcess),"Stable-world fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,10000);Poll(w);Check(Dumps(log).empty(),"Mismatched double World read started timer");
    SetExitHangWatchdogTestFailures(false,false);Poll(w);SetEvent(child.clear);Sleep(100);DWORD world=1;SIZE_T got=0;Check(ReadProcessMemory(child.process.hProcess,reinterpret_cast<void*>(0x7C67DC),&world,4,&got)&&got==4&&world==0,"Synthetic child did not clear its own World");SetExitHangWatchdogTestClock(true,11999);Poll(w);Check(Dumps(log).empty(),"Grace period captured before 2000ms");SetExitHangWatchdogTestClock(true,12000);Poll(w);const auto dumps=Dumps(log);Check(dumps.size()==1,"Known quit did not remain latched after World removal");VerifyDump(dumps[0],child.process.dwProcessId);Check(child.Alive(),"Watchdog stopped child after dump");child.CloseNormally();
}
void RealCapture(const std::wstring& exe,const std::wstring& dir){
    Child child(exe,1);const auto log=Log(dir,L"real-grace");ExitHangWatchdog w;SetExitHangWatchdogTestClock(false,0);Check(Init(w,child.process.hProcess),"Real grace fixture not armed");const ULONGLONG start=GetTickCount64();Poll(w);
    while(Dumps(log).empty()&&GetTickCount64()-start<6000){Sleep(100);Poll(w);}
    Check(GetTickCount64()-start>=2000,"Actual monotonic grace period was skipped");const auto dumps=Dumps(log);Check(dumps.size()==1,"Real hang capture count not one");VerifyDump(dumps[0],child.process.dwProcessId);const auto bytes=File(dumps[0]);for(int i=0;i<5;++i)Poll(w);Check(Dumps(log).size()==1&&File(dumps[0])==bytes,"Repeated Poll rewrote/repeated captured dump");Check(child.Alive(),"Dump capture altered child lifetime");std::printf("actual_capture_pid=%lu bytes=%zu status=%s\n",child.process.dwProcessId,bytes.size(),w.Status());child.CloseNormally();
}
void CollisionsAndFailures(const std::wstring& exe,const std::wstring& dir){
    {
        Child child(exe,1);const auto log=Log(dir,L"collision");const auto existing=log+L".hang-"+std::to_wstring(child.process.dwProcessId)+L"-3000-0.dmp";HANDLE f=CreateFileW(existing.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);Check(f!=INVALID_HANDLE_VALUE,"Collision sentinel create failed");const DWORD sentinel=0x11223344;DWORD wrote=0;Check(WriteFile(f,&sentinel,4,&wrote,nullptr)&&wrote==4,"Sentinel write failed");CloseHandle(f);
        ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);Check(Init(w,child.process.hProcess),"Collision fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,3000);Poll(w);const auto dumps=Dumps(log);Check(dumps.size()==2&&At<DWORD>(File(existing),0)==sentinel,"Existing dump overwritten on collision");for(const auto& dump:dumps)if(dump!=existing)VerifyDump(dump,child.process.dwProcessId);Check(child.Alive(),"Collision case stopped child");child.CloseNormally();
    }
    {
        Child child(exe,1);const auto log=Log(dir,L"dump-failure");ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);SetExitHangWatchdogTestFailures(true,false);Check(Init(w,child.process.hProcess),"Dump failure fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,3000);Poll(w);Check(std::strstr(w.Status(),"write failed")&&std::strstr(w.Status(),"error=5"),"Dump error not surfaced");for(int i=0;i<3;++i)Poll(w);Check(Dumps(log).size()==1&&child.Alive(),"Failed dump was retried or child altered");SetExitHangWatchdogTestFailures(false,false);child.CloseNormally();
    }
    {
        Child child(exe,1);const auto log=Log(dir,L"missing\\game");ExitHangWatchdog w;SetExitHangWatchdogTestClock(true,1000);Check(Init(w,child.process.hProcess),"Missing-directory fixture not armed");Poll(w);SetExitHangWatchdogTestClock(true,3000);Poll(w);Check(std::strstr(w.Status(),"create failed")!=nullptr,"File failure not surfaced");Poll(w);Check(Dumps(log).empty()&&child.Alive(),"File failure affected child");child.CloseNormally();
    }
}
}
int wmain(int argc,wchar_t** argv){
    try{Check(argc==2,"Pass synthetic fixture Morrowind.exe");const auto exe=Full(argv[1]);const auto dir=exe.substr(0,exe.find_last_of(L"\\/"))+L"\\run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());Check(CreateDirectoryW(dir.c_str(),nullptr)!=FALSE,"Test artifact directory failed");SavedEnvironment trace(L"MW_MODERN_TRACE"),probe(L"MW_MODERN_PROBE"),log(L"MW_MODERN_LOG");DisabledAndGuards(exe,dir);NoQuitAndWorldGuards(exe,dir);RealCapture(exe,dir);CollisionsAndFailures(exe,dir);SetExitHangWatchdogTestClock(false,0);std::printf("PASS exit-hang checks=%u synthetic_child_only no_termination_or_remote_writes=1\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
#endif
