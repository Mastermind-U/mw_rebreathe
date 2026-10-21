// Private x64 observer / x86 synthetic target. Does not launch Morrowind or COM.
#define NOMINMAX
#include "game_audio_target.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using GameAudioProtocol::Binding;
unsigned checks=0;
void Require(bool condition,const char* text){++checks;if(!condition)throw std::runtime_error(text);}
std::wstring Suffix(const std::wstring& session,const wchar_t* suffix){return session+suffix;}
struct Control {DWORD command,master,pointerBits;};
// Reserve this address as part of the synthetic PE image, exactly as the real
// game's .data does. VirtualAlloc(0x7C0000) can randomly collide with CRT heap.
unsigned char imageGlobals[0x400000];
constexpr DWORD kWorld=0x13000F80,kManager=0x14000000;
HANDLE controlMap=nullptr,request=nullptr,ack=nullptr,stop=nullptr;
Control* control=nullptr;
void ApplyCommand(unsigned char* worldRegion,unsigned char* manager){
    DWORD old=0;VirtualProtect(worldRegion,0x2000,PAGE_READWRITE,&old);
    std::memset(worldRegion,0,0x2000);std::memset(manager,0,0x1000);
    auto* world=worldRegion+0xF80;DWORD pointer=kWorld,managerPointer=kManager;
    if(control->command==1)pointer=0;
    if(control->command==2)managerPointer=0;
    if(control->command==3)world[0xDB]=1;
    if(control->command==4)pointer=1;
    if(control->command==5)managerPointer=1;
    std::memcpy(world+0x34,&managerPointer,4);manager[0x290]=static_cast<unsigned char>(control->master);
    std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(0x7C67DC)),&pointer,4);
    if(control->command==6)VirtualProtect(worldRegion+0x1000,0x1000,PAGE_NOACCESS,&old);
}
int Child(const std::wstring& session){
    const auto storage=reinterpret_cast<uintptr_t>(imageGlobals);
    volatile SIZE_T pointerBytes=sizeof(void*);
    if(pointerBytes!=4||storage>0x7C67DC||storage+sizeof(imageGlobals)<0x7C67E0)return 20;
    auto* world=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(0x13000000),0x2000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* manager=static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(0x14000000),0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!world||!manager)return 20;
    controlMap=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,Suffix(session,L"_control").c_str());
    if(!controlMap)return 21;control=static_cast<Control*>(MapViewOfFile(controlMap,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Control)));
    request=OpenEventW(SYNCHRONIZE,FALSE,Suffix(session,L"_request").c_str());
    ack=OpenEventW(EVENT_MODIFY_STATE,FALSE,Suffix(session,L"_ack").c_str());
    stop=OpenEventW(SYNCHRONIZE,FALSE,Suffix(session,L"_stop").c_str());
    if(!control||!request||!ack||!stop)return 22;
    RunnerAudioTarget publisher;if(!publisher.Publish(session.c_str(),GetCurrentProcess()))return 23;
    control->pointerBits=unsigned(sizeof(void*)*8);ApplyCommand(world,manager);SetEvent(ack);HANDLE waits[]={request,stop};
    for(;;){const DWORD wait=WaitForMultipleObjects(2,waits,FALSE,30000);if(wait==WAIT_OBJECT_0+1)break;if(wait!=WAIT_OBJECT_0)return 24;ApplyCommand(world,manager);SetEvent(ack);}
    publisher.Close();UnmapViewOfFile(control);CloseHandle(controlMap);CloseHandle(request);CloseHandle(ack);CloseHandle(stop);
    VirtualFree(world,0,MEM_RELEASE);VirtualFree(manager,0,MEM_RELEASE);return 0;
}
Binding* testView=nullptr;
unsigned readCalls=0,failCall=0,globalReads=0,managerReads=0,quitReads=0;
enum class Fault {None,Fail,Short,WorldChanged,ManagerChanged,QuitChanged,BindingChanged};
Fault readFault=Fault::None;
BOOL WINAPI ReadFixture(HANDLE process,LPCVOID address,LPVOID buffer,SIZE_T length,SIZE_T* read){
    ++readCalls;
    if(readFault==Fault::Fail&&readCalls==failCall){if(read)*read=0;SetLastError(ERROR_PARTIAL_COPY);return FALSE;}
    const BOOL result=ReadProcessMemory(process,address,buffer,length,read);
    if(readFault==Fault::Short&&readCalls==failCall&&result){if(read)*read=length-1;return TRUE;}
    const auto value=reinterpret_cast<uintptr_t>(address);
    if(value==0x7C67DC&&++globalReads==2&&readFault==Fault::WorldChanged&&result){DWORD other=kWorld+0x1000;std::memcpy(buffer,&other,4);}
    if(value==kWorld+0x34&&++managerReads==2&&readFault==Fault::ManagerChanged&&result){DWORD other=kManager+0x1000;std::memcpy(buffer,&other,4);}
    if(value==kWorld+0xDB&&++quitReads==2&&readFault==Fault::QuitChanged&&result)*static_cast<BYTE*>(buffer)=1;
    if(readFault==Fault::BindingChanged&&readCalls==1){InterlockedIncrement(&testView->sequence);testView->active=0;InterlockedIncrement(&testView->sequence);}
    return result;
}
void FaultMode(Fault mode,unsigned at=1){readFault=mode;readCalls=globalReads=managerReads=quitReads=0;failCall=at;SetGameAudioTargetReadMemoryForTests(ReadFixture);}
void Command(DWORD command,DWORD master){control->command=command;control->master=master;Require(SetEvent(request)!=FALSE,"command signal");Require(WaitForSingleObject(ack,5000)==WAIT_OBJECT_0,"child command timeout");}
void BindingWrite(DWORD active,DWORD pid,DWORD low,DWORD high){InterlockedIncrement(&testView->sequence);testView->active=active;testView->pid=pid;testView->creationLow=low;testView->creationHigh=high;InterlockedIncrement(&testView->sequence);}
}
int wmain(int argc,wchar_t** argv){
    if(argc==3&&std::wcscmp(argv[1],L"--target-child")==0)return Child(argv[2]);
    PROCESS_INFORMATION child{};HANDLE inspector=nullptr;HostGameAudioTarget host;
    try{
        Require(argc==2,"Pass x86 child fixture EXE");wchar_t base[160]{};swprintf_s(base,L"Local\\MWAudioTargetTest_%lu_%llu",GetCurrentProcessId(),GetTickCount64());const std::wstring session=base;
        Require(!host.Create(nullptr)&&!host.Create(L""),"invalid channel names accepted");Require(host.Create(session.c_str()),"host channel create");Require(!host.Create(session.c_str()),"duplicate local create");
        HostGameAudioTarget duplicate;Require(!duplicate.Create(session.c_str()),"existing channel overwritten");
        RunnerAudioTarget absent;Require(!absent.Publish(L"Local\\MWAudioMissingFixture",GetCurrentProcess()),"absent channel accepted");Require(!absent.Publish(session.c_str(),nullptr),"null child accepted");Require(!host.Sample().pid,"unpublished channel sampled");
        controlMap=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Control),Suffix(session,L"_control").c_str());Require(controlMap!=nullptr,"control mapping");control=static_cast<Control*>(MapViewOfFile(controlMap,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Control)));Require(control!=nullptr,"control view");control->master=250;
        request=CreateEventW(nullptr,FALSE,FALSE,Suffix(session,L"_request").c_str());ack=CreateEventW(nullptr,FALSE,FALSE,Suffix(session,L"_ack").c_str());stop=CreateEventW(nullptr,TRUE,FALSE,Suffix(session,L"_stop").c_str());Require(request&&ack&&stop,"fixture events");
        std::wstring command=L"\""+std::wstring(argv[1])+L"\" --target-child \""+session+L"\"";STARTUPINFOW startup{};startup.cb=sizeof(startup);Require(CreateProcessW(argv[1],command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,"synthetic x86 child");CloseHandle(child.hThread);child.hThread=nullptr;
        const DWORD ready=WaitForSingleObject(ack,5000);
        if(ready!=WAIT_OBJECT_0){DWORD code=999;GetExitCodeProcess(child.hProcess,&code);std::fprintf(stderr,"Synthetic child exit=%lu\n",code);}
        Require(ready==WAIT_OBJECT_0,"child did not publish");Require(control->pointerBits==32,"child is not x86");
        inspector=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,Suffix(session,L"_audio").c_str());Require(inspector!=nullptr,"inspect channel");testView=static_cast<Binding*>(MapViewOfFile(inspector,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Binding)));Require(testView!=nullptr,"inspect channel view");
        const DWORD low=testView->creationLow,high=testView->creationHigh;Require(testView->pid==child.dwProcessId,"published wrong exact child PID");
        auto sample=host.Sample();Require(sample.pid==child.dwProcessId&&sample.masterKnown&&!sample.masterZero,"valid full Master");
        const LONG originalGeneration=testView->sequence;
        RunnerAudioTarget second;Require(!second.Publish(session.c_str(),GetCurrentProcess()),"second publisher replaced live child");
        Require(testView->sequence==originalGeneration,"rejected publisher invalidated original lifetime token");
        for(unsigned master:{0u,1u,125u,250u}){Command(0,master);sample=host.Sample();Require(sample.pid==child.dwProcessId&&sample.masterKnown&&sample.masterZero==(master==0),"Master sample mismatch");}
        for(unsigned bad:{251u,255u}){Command(0,bad);sample=host.Sample();Require(sample.pid==child.dwProcessId&&!sample.masterKnown&&!sample.masterZero,"invalid Master accepted");}
        for(DWORD mode:{1u,2u,3u,4u,5u,6u}){Command(mode,0);sample=host.Sample();Require(sample.pid==child.dwProcessId&&!sample.masterKnown&&!sample.masterZero,"null/quit/invalid/unreadable native pointer accepted");}
        Command(0,0);FaultMode(Fault::None);Require(host.Sample().masterZero,"baseline sampled zero");Require(readCalls==7,"unexpected native snapshot read count");
        for(Fault fault:{Fault::Fail,Fault::Short})for(unsigned at=1;at<=7;++at){FaultMode(fault,at);sample=host.Sample();Require(sample.pid==child.dwProcessId&&!sample.masterKnown&&!sample.masterZero,"failed/short RPM produced known Master");}
        for(Fault fault:{Fault::WorldChanged,Fault::ManagerChanged,Fault::QuitChanged}){FaultMode(fault);sample=host.Sample();Require(sample.pid==child.dwProcessId&&!sample.masterKnown&&!sample.masterZero,"replaced World/manager/quit snapshot accepted");}
        FaultMode(Fault::BindingChanged);sample=host.Sample();Require(!sample.pid&&!sample.masterKnown,"changed publication accepted");FaultMode(Fault::None);BindingWrite(1,child.dwProcessId,low,high);
        BindingWrite(1,child.dwProcessId,low^1,high);Require(!host.Sample().pid,"reused PID/creation mismatch accepted");BindingWrite(1,0x7FFFFFFF,low,high);Require(!host.Sample().pid,"missing PID accepted");BindingWrite(1,child.dwProcessId,low,high);Require(host.Sample().masterZero,"valid identity did not recover");
        InterlockedIncrement(&testView->sequence);Require(!host.Sample().pid,"odd/torn publication accepted");InterlockedIncrement(&testView->sequence);Require(host.Sample().masterZero,"stable publication did not recover");
        // An old publisher must not clear a later binding. This fixture has
        // deliberately advanced generation above; its destructor sees mismatch.
        const LONG beforeClose=testView->sequence;SetEvent(stop);Require(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0,"child exit timeout");DWORD exitCode=999;Require(GetExitCodeProcess(child.hProcess,&exitCode)&&exitCode==0,"child clean fixture exit");Require(testView->active==1&&testView->sequence==beforeClose,"old publisher cleared newer generation");Require(!host.Sample().pid,"exited held process reused");
        Require(host.PinnedProcessIdForTests()==child.dwProcessId,"exited process identity was unpinned before session restore");
        // A publisher using the current generation retires only its own entry.
        BindingWrite(0,0,0,0);{RunnerAudioTarget own;Require(own.Publish(session.c_str(),GetCurrentProcess()),"self identity publication");Require(testView->active==1,"self publication inactive");Require(!host.Sample().pid&&host.PinnedProcessIdForTests()==child.dwProcessId,"host rebound to another process");}Require(testView->active==0,"publisher RAII did not retire own binding");Require(!host.Sample().pid,"retired channel sampled");Require(host.PinnedProcessIdForTests()==child.dwProcessId,"inactive channel unpinned old identity");
        const DWORD savedMagic=testView->magic;testView->magic^=1;Require(!host.Sample().pid,"bad protocol header accepted");testView->magic=savedMagic;
        UnmapViewOfFile(testView);testView=nullptr;CloseHandle(inspector);inspector=nullptr;host.Close();Require(host.PinnedProcessIdForTests()==0,"host Close retained process handle");CloseHandle(child.hProcess);child.hProcess=nullptr;UnmapViewOfFile(control);control=nullptr;CloseHandle(controlMap);CloseHandle(request);CloseHandle(ack);CloseHandle(stop);
        std::printf("PASS game audio target checks=%u observerBits=%u x86Child=true\n",checks,unsigned(sizeof(void*)*8));return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s (Win32=%lu)\n",error.what(),GetLastError());if(child.hProcess){TerminateProcess(child.hProcess,90);WaitForSingleObject(child.hProcess,5000);CloseHandle(child.hProcess);}if(testView)UnmapViewOfFile(testView);if(inspector)CloseHandle(inspector);return 1;}
}
