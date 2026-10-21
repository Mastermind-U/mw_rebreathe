#define NOMINMAX
#include "session_mute.h"
#include <audioclient.h>
#include <cstdio>
#include <stdexcept>
#include <utility>
namespace {
unsigned checks=0;
void Check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
constexpr DWORD Target=23056,Other=23111,Marker=0x13579BDF;
constexpr HRESULT CrossProcess=static_cast<HRESULT>(0x0889000D);
struct FakeSession final:AudioSessionMuteDetail::Session {
    std::wstring key,stable;
    DWORD pid=Target,thread=GetCurrentThreadId();
    int state=1;
    BOOL mute=FALSE;
    HRESULT pidHr=S_OK,systemHr=S_FALSE,stateHr=S_OK,getHr=S_OK,setHr=S_OK;
    unsigned writes=0,mutes=0,unmutes=0;
    bool wrongThread=false;
    FakeSession(std::wstring k,std::wstring s):key(std::move(k)),stable(std::move(s)){}
    const std::wstring& Key()const noexcept override{return key;}
    const std::wstring& StableKey()const noexcept override{return stable;}
    void Called()noexcept{wrongThread=wrongThread||thread!=GetCurrentThreadId();SetLastError(0xBAD0BAD0);}
    HRESULT ProcessId(DWORD* out)noexcept override{Called();if(pidHr==S_OK)*out=pid;return pidHr;}
    HRESULT IsSystem()noexcept override{Called();return systemHr;}
    HRESULT State(int* out)noexcept override{Called();if(stateHr==S_OK)*out=state;return stateHr;}
    HRESULT GetMute(BOOL* out)noexcept override{Called();if(getHr==S_OK)*out=mute;return getHr;}
    HRESULT SetMute(BOOL value,const GUID* context)noexcept override{Called();if(!context)return E_POINTER;++writes;if(setHr==S_OK){mute=value;if(value)++mutes;else ++unmutes;}return setHr;}
};
struct FakeBackend final:AudioSessionMuteDetail::Backend {
    std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>> sessions;
    HRESULT initHr=S_OK,enumHr=S_OK;
    unsigned initializes=0,enumerations=0,shutdowns=0;
    bool throwEnumeration=false;
    HRESULT Initialize()noexcept override{++initializes;SetLastError(0xBAD);return initHr;}
    HRESULT Enumerate(std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>>& out)override{++enumerations;if(throwEnumeration)throw std::bad_alloc();out=sessions;SetLastError(0xBAD);return enumHr;}
    void Shutdown()noexcept override{++shutdowns;}
    std::shared_ptr<FakeSession> Add(const wchar_t* key,const wchar_t* stable){auto p=std::make_shared<FakeSession>(key,stable);sessions.push_back(p);return p;}
};
void Update(AudioSessionMuteGuard& guard,DWORD pid,bool known,bool zero){SetLastError(Marker);guard.Update(pid,known,zero);Check(GetLastError()==Marker,"Update changed caller LastError");}
void Restore(AudioSessionMuteGuard& guard){SetLastError(Marker);guard.Restore();Check(GetLastError()==Marker,"Restore changed caller LastError");}
void Selection(){
    FakeBackend backend;auto own=backend.Add(L"one\ninstance-a",L"one\napp"),second=backend.Add(L"two\ninstance-a",L"two\napp");
    auto prior=backend.Add(L"prior",L"prior");prior->mute=TRUE;
    auto other=backend.Add(L"other",L"other");other->pid=Other;
    auto cross=backend.Add(L"cross",L"cross");cross->pidHr=CrossProcess;
    auto noPid=backend.Add(L"noPid",L"noPid");noPid->pidHr=S_FALSE;
    auto system=backend.Add(L"system",L"system");system->systemHr=S_OK;
    auto badSystem=backend.Add(L"badSystem",L"badSystem");badSystem->systemHr=E_FAIL;
    auto expired=backend.Add(L"expired",L"expired");expired->state=2;
    auto badState=backend.Add(L"badState",L"badState");badState->stateHr=S_FALSE;
    auto badGet=backend.Add(L"badGet",L"badGet");badGet->getHr=S_FALSE;
    auto failedGet=backend.Add(L"failedGet",L"failedGet");failedGet->getHr=E_FAIL;
    {
        AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        Check(own->mute&&second->mute&&guard.Stats().ownedMutes==2,"Eligible endpoints did not mute");
        for(const auto& item:{prior,other,cross,noPid,system,badSystem,expired,badState,badGet,failedGet})Check(item->writes==0,"Rejected or pre-muted session was written");
        Update(guard,Target,true,true);Check(own->writes==1&&second->writes==1,"Repeated poll rewrote existing mute");
        Update(guard,Target,false,true);Check(!own->mute&&!second->mute&&prior->mute&&guard.Stats().ownedMutes==0,"Unknown Master did not restore only owned mute");
        Check(backend.initializes==1,"COM backend initialized more than once");
    }
    Check(backend.shutdowns==1,"Backend shutdown not called once");
}
void NormalLifecycle(){
    for(unsigned mode=0;mode<5;++mode){
        FakeBackend backend;auto item=backend.Add(L"instance",L"stable");
        {
            AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);Check(item->mute&&item->writes==1,"Initial mute failed");
            if(mode==0)Update(guard,Target,true,false);
            if(mode==1)Update(guard,Target,false,false);
            if(mode==2)Update(guard,0,false,false);
            if(mode==3)Restore(guard);
            if(mode<4)Check(!item->mute&&guard.Stats().ownedMutes==0,"Normal explicit lifecycle restore failed");
        }
        Check(item->writes==2&&!item->mute&&item->unmutes==1,"Destructor/lifecycle changed original mute");
    }
}
void FailureAndRetry(){
    for(HRESULT failure:{E_FAIL,S_FALSE}){
        FakeBackend backend;auto item=backend.Add(L"a",L"a");AudioSessionMuteGuard guard(&backend);
        item->setHr=failure;Update(guard,Target,true,true);Check(!item->mute&&guard.Stats().ownedMutes==0,"Rejected mute claimed ownership");
        item->setHr=S_OK;Update(guard,Target,true,true);Check(item->mute&&guard.Stats().ownedMutes==1,"Retry initial mute failed");
        item->getHr=failure;Restore(guard);Check(item->mute&&guard.Stats().ownedMutes==1,"Failed GetMute discarded ownership");
        item->getHr=S_OK;item->setHr=failure;Restore(guard);Check(item->mute&&guard.Stats().ownedMutes==1,"Failed unmute discarded ownership");
        item->setHr=S_OK;Restore(guard);Check(!item->mute&&guard.Stats().ownedMutes==0,"Retry restore failed");
    }
    {
        FakeBackend backend;backend.initHr=RPC_E_CHANGED_MODE;auto item=backend.Add(L"a",L"a");AudioSessionMuteGuard guard(&backend);
        Update(guard,Target,true,true);Check(!guard.Stats().initialized&&item->writes==0,"Failed COM initialization changed audio");
        backend.initHr=S_OK;backend.enumHr=E_FAIL;Update(guard,Target,true,true);Check(item->mute&&guard.Stats().errors>=2,"Partial enumeration lost valid matching session/error");
        backend.throwEnumeration=true;Update(guard,Target,true,true);Check(guard.Stats().ownedMutes==1,"Enumeration exception lost ownership");
        backend.throwEnumeration=false;backend.enumHr=S_OK;Restore(guard);Check(!item->mute,"Exception recovery did not restore");
    }
}
void ExpirationAndReconnect(){
    {
        FakeBackend backend;auto item=backend.Add(L"old",L"stable");AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        item->state=2;Update(guard,Target,true,true);Check(!item->mute&&guard.Stats().ownedMutes==0,"Expired owned mute was dropped without restore");
    }
    {
        FakeBackend backend;auto item=backend.Add(L"old",L"stable");AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        item->state=2;item->setHr=E_FAIL;Update(guard,Target,true,true);Check(item->mute&&guard.Stats().ownedMutes==1,"Expired failed restore lost obligation");
        item->setHr=S_OK;Update(guard,Target,true,true);Check(!item->mute&&guard.Stats().ownedMutes==0,"Expired restore retry failed");
    }
    for(bool immediateNonzero:{false,true}){
        FakeBackend backend;auto old=backend.Add(L"endpoint\nold",L"endpoint\nstable");AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        old->pidHr=AUDCLNT_E_DEVICE_INVALIDATED;old->stateHr=AUDCLNT_E_DEVICE_INVALIDATED;
        backend.sessions.clear();auto fresh=backend.Add(L"endpoint\nnew",L"endpoint\nstable");fresh->mute=TRUE;
        Update(guard,Target,true,!immediateNonzero);
        if(!immediateNonzero){Check(fresh->mute&&guard.Stats().ownedMutes==1,"Reconnect inherited mute lost prior obligation");Update(guard,Target,true,false);}
        Check(!fresh->mute&&fresh->unmutes==1&&guard.Stats().ownedMutes==0,"Reconnect did not restore prior across instance IDs");
        Check(old->unmutes==0,"Invalid old interface was written");
    }
    {
        FakeBackend backend;auto old=backend.Add(L"old",L"stable");AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        old->pidHr=AUDCLNT_E_DEVICE_INVALIDATED;old->stateHr=AUDCLNT_E_DEVICE_INVALIDATED;backend.sessions.clear();Update(guard,Target,true,true);Restore(guard);
        Check(guard.Stats().ownedMutes==1&&FAILED(guard.Stats().lastResult),"Unavailable endpoint silently dropped restoration obligation");
        auto other=backend.Add(L"new",L"stable");other->pid=Other;other->mute=TRUE;Update(guard,Target,true,false);Check(other->writes==0&&guard.Stats().ownedMutes==1,"Reconnect restored another PID");
        other->pid=Target;Restore(guard);Check(!other->mute&&guard.Stats().ownedMutes==0,"Late reconnect could not satisfy pending restore");
    }
    {
        FakeBackend backend;auto old=backend.Add(L"old",L"stable");AudioSessionMuteGuard guard(&backend);Update(guard,Target,true,true);
        old->pidHr=AUDCLNT_E_DEVICE_INVALIDATED;old->stateHr=AUDCLNT_E_DEVICE_INVALIDATED;backend.sessions.clear();auto reused=backend.Add(L"new",L"stable");reused->mute=TRUE;
        Update(guard,0,false,false);Restore(guard);Check(reused->writes==0&&guard.Stats().ownedMutes==1,"Lost process authorization rebound a future session");
        Update(guard,Target,true,false);Check(!reused->mute,"Restored exact live PID did not rebind");
    }
}
DWORD WINAPI ForeignThread(void* p){auto* guard=static_cast<AudioSessionMuteGuard*>(p);guard->Update(Target,true,true);guard->Restore();return 0;}
void ThreadAndOwnership(){
    FakeBackend backend;auto item=backend.Add(L"a",L"a");AudioSessionMuteGuard guard(&backend);
    HANDLE thread=CreateThread(nullptr,0,&ForeignThread,&guard,0,nullptr);Check(thread!=nullptr,"Could not create isolated test thread");Check(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0,"Test thread wait failed");CloseHandle(thread);
    Check(item->writes==0&&backend.initializes==0&&guard.Stats().errors==2,"Foreign thread touched COM/audio state");
    Update(guard,Target,true,true);Check(item->mute&&!item->wrongThread,"Main thread mute did not recover");
    // An external unmute while guarded is re-muted at the next poll, then the
    // original FALSE state is restored when the in-game Master becomes nonzero.
    item->mute=FALSE;Update(guard,Target,true,true);Check(item->mutes==2,"External unmute bypassed Master0 guard");Update(guard,Target,true,false);Check(!item->mute,"External-unmute recovery failed");
}
}
int main(){try{Selection();NormalLifecycle();FailureAndRetry();ExpirationAndReconnect();ThreadAndOwnership();std::printf("PASS session-mute checks=%u fake_backend_only no_real_audio_state_writes=1\n",checks);return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
