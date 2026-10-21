#define NOMINMAX
#include "game_audio_target.h"
#include <cwchar>
#include <cstring>

namespace {
using GameAudioProtocol::Binding;
struct Snapshot {LONG generation=0;DWORD active=0,pid=0,low=0,high=0;};
bool Name(const wchar_t* base,wchar_t (&out)[512]) noexcept {
    if(!base)return false;
    size_t length=0;while(length<480&&base[length])++length;
    if(!length||length>=480)return false;
    return swprintf_s(out,L"%s_audio",base)>0;
}
bool Times(HANDLE process,DWORD* low,DWORD* high) noexcept {
    FILETIME creation{},exit{},kernel{},user{};
    if(!GetProcessTimes(process,&creation,&exit,&kernel,&user))return false;
    *low=creation.dwLowDateTime;*high=creation.dwHighDateTime;return true;
}
bool Header(const Binding* p) noexcept {
    return p&&p->magic==GameAudioProtocol::Magic&&p->version==GameAudioProtocol::Version&&p->size==sizeof(Binding);
}
bool ReadBinding(Binding* p,Snapshot* result) noexcept {
    if(!Header(p))return false;
    for(unsigned i=0;i<3;++i){
        const LONG before=InterlockedCompareExchange(&p->sequence,0,0);
        if(before&1)continue;
        MemoryBarrier();Snapshot value{};value.generation=before;
        value.active=p->active;value.pid=p->pid;value.low=p->creationLow;value.high=p->creationHigh;
        MemoryBarrier();
        if(InterlockedCompareExchange(&p->sequence,0,0)==before){*result=value;return true;}
    }
    return false;
}
bool Lock(Binding* p,LONG* before) noexcept {
    for(unsigned i=0;i<3;++i){
        const LONG value=InterlockedCompareExchange(&p->sequence,0,0);
        if((value&1)||value>0x7FFFFFFA)continue;
        if(InterlockedCompareExchange(&p->sequence,value+1,value)==value){*before=value;return true;}
    }
    return false;
}
bool Pointer(DWORD value,size_t length) noexcept {
    return value>=0x10000&&static_cast<std::uint64_t>(value)+length<=0x80000000ULL;
}
#ifdef MW_AUDIO_TARGET_TESTING
GameAudioReadMemory g_read=ReadProcessMemory;
#else
constexpr auto g_read=ReadProcessMemory;
#endif
template<class T> bool Read(HANDLE process,DWORD address,T* out) noexcept {
    if(!Pointer(address,sizeof(T)))return false;
    SIZE_T read=0;T value{};
    if(!g_read(process,reinterpret_cast<const void*>(static_cast<uintptr_t>(address)),&value,sizeof(value),&read)||read!=sizeof(value))return false;
    *out=value;return true;
}
bool Master(HANDLE process,BYTE* master) noexcept {
    DWORD world=0,manager=0,worldAfter=0,managerAfter=0;BYTE quit=1,quitAfter=1,value=255;
    if(!Read(process,0x7C67DC,&world)||!Pointer(world,0xDC)||
       !Read(process,world+0xDB,&quit)||quit||!Read(process,world+0x34,&manager)||
       !Pointer(manager,0x291)||!Read(process,manager+0x290,&value)||value>250||
       !Read(process,world+0xDB,&quitAfter)||quitAfter||!Read(process,world+0x34,&managerAfter)||
       !Read(process,0x7C67DC,&worldAfter)||worldAfter!=world||managerAfter!=manager)return false;
    *master=value;return true;
}
}
HostGameAudioTarget::~HostGameAudioTarget() noexcept {Close();}
void HostGameAudioTarget::ReleaseProcess() noexcept {
    if(process_)CloseHandle(process_);process_=nullptr;pid_=creationLow_=creationHigh_=0;
}
void HostGameAudioTarget::Close() noexcept {
    ReleaseProcess();if(view_)UnmapViewOfFile(view_);view_=nullptr;
    if(mapping_)CloseHandle(mapping_);mapping_=nullptr;status_="closed";
}
bool HostGameAudioTarget::Create(const wchar_t* baseSession) noexcept {
    if(mapping_){status_="already created";return false;}
    wchar_t name[512]{};if(!Name(baseSession,name)){status_="invalid session name";return false;}
    DWORD low=0,high=0;if(!Times(GetCurrentProcess(),&low,&high)){status_="host identity unavailable";return false;}
    SetLastError(ERROR_SUCCESS);HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Binding),name);
    const DWORD error=GetLastError();
    if(!mapping){status_="cannot create audio binding";return false;}
    if(error==ERROR_ALREADY_EXISTS){CloseHandle(mapping);status_="audio binding already exists";return false;}
    auto* view=static_cast<Binding*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Binding)));
    if(!view){CloseHandle(mapping);status_="cannot map audio binding";return false;}
    std::memset(view,0,sizeof(*view));view->version=GameAudioProtocol::Version;view->size=sizeof(*view);
    view->hostPid=GetCurrentProcessId();view->hostCreationLow=low;view->hostCreationHigh=high;
    MemoryBarrier();view->magic=GameAudioProtocol::Magic;mapping_=mapping;view_=view;status_="waiting for exact child";return true;
}
GameAudioSample HostGameAudioTarget::Sample() noexcept {
    Snapshot binding{};
    if(!ReadBinding(view_,&binding)||binding.active!=1||!binding.pid){status_="inactive or unstable binding";return {};}
    // One host owns one exact game for its whole lifetime. Pin the verified
    // handle through mute restoration, even after exit/inactive/error, so the
    // OS cannot reuse this PID for another process while sessions are owned.
    if(process_&&(pid_!=binding.pid||creationLow_!=binding.low||creationHigh_!=binding.high)){
        status_="different child binding rejected";return {};
    }
    if(!process_){
        HANDLE process=OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,binding.pid);
        DWORD low=0,high=0;
        if(!process||!Times(process,&low,&high)||low!=binding.low||high!=binding.high||WaitForSingleObject(process,0)!=WAIT_TIMEOUT){
            if(process)CloseHandle(process);status_="child identity unavailable or expired";return {};
        }
        process_=process;pid_=binding.pid;creationLow_=low;creationHigh_=high;
    }
    if(WaitForSingleObject(process_,0)!=WAIT_TIMEOUT){status_="child exited";return {};}
    BYTE master=255;const bool known=Master(process_,&master);Snapshot after{};
    if(!ReadBinding(view_,&after)||after.active!=1||after.generation!=binding.generation||after.pid!=binding.pid||after.low!=binding.low||after.high!=binding.high){
        status_="binding changed during sample";return {};
    }
    if(WaitForSingleObject(process_,0)!=WAIT_TIMEOUT){status_="child exited during sample";return {};}
    status_=known?"native Master sampled":"native World/Master unavailable";
    return {pid_,known,known&&master==0};
}
RunnerAudioTarget::~RunnerAudioTarget() noexcept {Close();}
bool RunnerAudioTarget::Publish(const wchar_t* baseSession,HANDLE childProcess) noexcept {
    if(mapping_){status_="already published";return false;}
    wchar_t name[512]{};DWORD low=0,high=0;
    const DWORD pid=GetProcessId(childProcess);
    if(!Name(baseSession,name)||!pid||!Times(childProcess,&low,&high)||WaitForSingleObject(childProcess,0)!=WAIT_TIMEOUT){status_="invalid child identity";return false;}
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name);
    if(!mapping){status_="audio channel absent";return false;}
    auto* view=static_cast<Binding*>(MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Binding)));
    if(!Header(view)){if(view)UnmapViewOfFile(view);CloseHandle(mapping);status_="invalid audio channel";return false;}
    HANDLE host=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,view->hostPid);DWORD hostLow=0,hostHigh=0;
    const bool liveHost=host&&Times(host,&hostLow,&hostHigh)&&hostLow==view->hostCreationLow&&hostHigh==view->hostCreationHigh&&WaitForSingleObject(host,0)==WAIT_TIMEOUT;
    if(host)CloseHandle(host);
    LONG before=0;
    if(!liveHost||!Lock(view,&before)){UnmapViewOfFile(view);CloseHandle(mapping);status_="audio host unavailable or writer busy";return false;}
    if(view->active){InterlockedExchange(&view->sequence,before);UnmapViewOfFile(view);CloseHandle(mapping);status_="audio binding already active";return false;}
    view->pid=pid;view->creationLow=low;view->creationHigh=high;view->active=1;
    MemoryBarrier();generation_=before+2;InterlockedExchange(&view->sequence,generation_);
    mapping_=mapping;view_=view;pid_=pid;creationLow_=low;creationHigh_=high;status_="exact child published";return true;
}
void RunnerAudioTarget::Close() noexcept {
    if(view_&&InterlockedCompareExchange(&view_->sequence,generation_+1,generation_)==generation_){
        if(view_->pid==pid_&&view_->creationLow==creationLow_&&view_->creationHigh==creationHigh_)view_->active=0;
        MemoryBarrier();InterlockedExchange(&view_->sequence,generation_+2);
    }
    if(view_)UnmapViewOfFile(view_);view_=nullptr;if(mapping_)CloseHandle(mapping_);mapping_=nullptr;
    pid_=creationLow_=creationHigh_=0;generation_=0;status_="inactive";
}
#ifdef MW_AUDIO_TARGET_TESTING
void SetGameAudioTargetReadMemoryForTests(GameAudioReadMemory function) noexcept {g_read=function?function:ReadProcessMemory;}
#endif
