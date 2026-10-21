#define NOMINMAX
#include "session_mute.h"
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <new>
#include <utility>
#ifndef AUDCLNT_S_NO_SINGLE_PROCESS
#define AUDCLNT_S_NO_SINGLE_PROCESS AUDCLNT_SUCCESS(0x00d)
#endif
namespace {
const GUID MuteContext={0xa51e7a62,0x2eb4,0x4a10,{0xb2,0xcd,0x95,0x7c,0x85,0x1f,0x1e,0x91}};
template<class T>class Com {
public:
    T* p=nullptr;
    ~Com(){if(p)p->Release();}
    T** Out(){return &p;}
    T* Take(){T* value=p;p=nullptr;return value;}
};
class WinSession final:public AudioSessionMuteDetail::Session {
    IAudioSessionControl2* control_;
    ISimpleAudioVolume* volume_;
    std::wstring key_;
    std::wstring stableKey_;
public:
    WinSession(IAudioSessionControl2* control,ISimpleAudioVolume* volume,std::wstring key,std::wstring stable):control_(control),volume_(volume),key_(std::move(key)),stableKey_(std::move(stable)){}
    ~WinSession() override{volume_->Release();control_->Release();}
    const std::wstring& Key()const noexcept override{return key_;}
    const std::wstring& StableKey()const noexcept override{return stableKey_;}
    HRESULT ProcessId(DWORD* pid)noexcept override{return control_->GetProcessId(pid);}
    HRESULT IsSystem()noexcept override{return control_->IsSystemSoundsSession();}
    HRESULT State(int* state)noexcept override{AudioSessionState value{};const HRESULT hr=control_->GetState(&value);if(SUCCEEDED(hr))*state=static_cast<int>(value);return hr;}
    HRESULT GetMute(BOOL* value)noexcept override{return volume_->GetMute(value);}
    HRESULT SetMute(BOOL value,const GUID* context)noexcept override{return volume_->SetMute(value,context);}
};
class WinBackend final:public AudioSessionMuteDetail::Backend {
    IMMDeviceEnumerator* enumerator_=nullptr;
    bool com_=false;
public:
    HRESULT Initialize()noexcept override{
        if(enumerator_)return S_OK;
        HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(hr))return hr;
        com_=true;
        hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator_));
        if(FAILED(hr)){CoUninitialize();com_=false;}return hr;
    }
    HRESULT Enumerate(std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>>& output)override{
        Com<IMMDeviceCollection> endpoints;HRESULT hr=enumerator_->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,endpoints.Out());if(FAILED(hr))return hr;
        UINT count=0;hr=endpoints.p->GetCount(&count);if(FAILED(hr))return hr;
        HRESULT result=S_OK;
        for(UINT e=0;e<count;++e){
            Com<IMMDevice> endpoint;hr=endpoints.p->Item(e,endpoint.Out());if(FAILED(hr)){result=hr;continue;}
            LPWSTR endpointId=nullptr;hr=endpoint.p->GetId(&endpointId);if(FAILED(hr)||!endpointId){if(endpointId)CoTaskMemFree(endpointId);result=FAILED(hr)?hr:E_UNEXPECTED;continue;}
            std::wstring endpointKey;
            try{endpointKey=endpointId;}catch(...){CoTaskMemFree(endpointId);throw;}CoTaskMemFree(endpointId);
            Com<IAudioSessionManager2> manager;hr=endpoint.p->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.Out()));if(FAILED(hr)){result=hr;continue;}
            Com<IAudioSessionEnumerator> sessions;hr=manager.p->GetSessionEnumerator(sessions.Out());if(FAILED(hr)){result=hr;continue;}
            int sessionCount=0;hr=sessions.p->GetCount(&sessionCount);if(FAILED(hr)){result=hr;continue;}
            for(int i=0;i<sessionCount;++i){
                Com<IAudioSessionControl> base;hr=sessions.p->GetSession(i,base.Out());if(FAILED(hr)){result=hr;continue;}
                Com<IAudioSessionControl2> control;hr=base.p->QueryInterface(__uuidof(IAudioSessionControl2),reinterpret_cast<void**>(control.Out()));if(FAILED(hr)){result=hr;continue;}
                Com<ISimpleAudioVolume> volume;hr=base.p->QueryInterface(__uuidof(ISimpleAudioVolume),reinterpret_cast<void**>(volume.Out()));if(FAILED(hr)){result=hr;continue;}
                LPWSTR instance=nullptr;hr=control.p->GetSessionInstanceIdentifier(&instance);if(FAILED(hr)||!instance){if(instance)CoTaskMemFree(instance);result=FAILED(hr)?hr:E_UNEXPECTED;continue;}
                std::wstring key;
                try{key=endpointKey;key.push_back(L'\n');key.append(instance);}catch(...){CoTaskMemFree(instance);throw;}CoTaskMemFree(instance);
                LPWSTR identifier=nullptr;hr=control.p->GetSessionIdentifier(&identifier);if(FAILED(hr)||!identifier){if(identifier)CoTaskMemFree(identifier);result=FAILED(hr)?hr:E_UNEXPECTED;continue;}
                std::wstring stable;
                try{stable=endpointKey;stable.push_back(L'\n');stable.append(identifier);}catch(...){CoTaskMemFree(identifier);throw;}CoTaskMemFree(identifier);
                // Construct before relinquishing the two COM references.
                auto item=std::make_shared<WinSession>(control.p,volume.p,std::move(key),std::move(stable));control.Take();volume.Take();output.push_back(std::move(item));
            }
        }
        return result;
    }
    void Shutdown()noexcept override{if(enumerator_){enumerator_->Release();enumerator_=nullptr;}if(com_){CoUninitialize();com_=false;}}
};
bool Gone(HRESULT hr){return hr==AUDCLNT_E_DEVICE_INVALIDATED||hr==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)||hr==HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);}
}
struct AudioSessionMuteGuard::State {
    struct Owned{std::shared_ptr<AudioSessionMuteDetail::Session> session;DWORD pid;BOOL prior;};
    AudioSessionMuteDetail::Backend* backend=nullptr;
    bool ownsBackend=false;
    DWORD thread=GetCurrentThreadId(),pid=0,authorizedPid=0;
    std::vector<Owned> owned;
    AudioSessionMuteStats stats{};
    void Error(HRESULT hr){stats.lastResult=hr;++stats.errors;}
    bool SameThread(){if(thread==GetCurrentThreadId())return true;Error(HRESULT_FROM_WIN32(ERROR_INVALID_THREAD_ID));return false;}
    bool Eligible(const std::shared_ptr<AudioSessionMuteDetail::Session>& session,DWORD target){
        DWORD actual=0;const HRESULT process=session->ProcessId(&actual);if(process!=S_OK||actual!=target)return false;
        if(session->IsSystem()!=S_FALSE)return false;
        int status=2;const HRESULT hr=session->State(&status);return hr==S_OK&&(status==0||status==1);
    }
    bool TryRestore(Owned& entry){
            DWORD actual=0;HRESULT hr=entry.session->ProcessId(&actual);bool done=false;
            if(Gone(hr)||(hr==S_OK&&actual!=entry.pid)||hr==AUDCLNT_S_NO_SINGLE_PROCESS)Error(hr==S_OK?E_ACCESSDENIED:hr);
            else if(hr!=S_OK)Error(hr);
            else {
                const HRESULT system=entry.session->IsSystem();
                if(system==S_OK)Error(E_ACCESSDENIED);
                else if(system!=S_FALSE)Error(system);
                else{
                    int status=2;hr=entry.session->State(&status);
                    if(Gone(hr))Error(hr);
                    else if(hr!=S_OK)Error(hr);
                    else{
                        BOOL current=FALSE;hr=entry.session->GetMute(&current);
                        if(Gone(hr))Error(hr);
                        else if(hr!=S_OK)Error(hr);
                        else if(current==entry.prior)done=true;
                        else{hr=entry.session->SetMute(entry.prior,&MuteContext);if(hr==S_OK){++stats.changes;done=true;}else Error(hr);}
                    }
                }
            }
            return done;
    }
    void RestoreOwned(){
        for(size_t i=0;i<owned.size();){
            if(TryRestore(owned[i]))owned.erase(owned.begin()+static_cast<ptrdiff_t>(i));else ++i;
        }
        stats.ownedMutes=static_cast<DWORD>(owned.size());
    }
    void Rebind(const std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>>& sessions){
        for(auto& entry:owned)for(const auto& session:sessions){
            if(entry.pid!=authorizedPid||!session||session->StableKey()!=entry.session->StableKey()||!Eligible(session,entry.pid))continue;
            // Never replace a still-valid instance merely because another
            // simultaneously active stream has the same persistent identity.
            int status=0;DWORD process=0;const HRESULT oldPid=entry.session->ProcessId(&process),oldState=entry.session->State(&status);
            if(session->Key()==entry.session->Key()||Gone(oldPid)||Gone(oldState)||(oldState==S_OK&&status==2))entry.session=session;
            break;
        }
    }
    void RefreshBindings(){
        if(owned.empty()||!stats.initialized||!backend)return;
        std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>> sessions;
        const HRESULT hr=backend->Enumerate(sessions);if(FAILED(hr))Error(hr);Rebind(sessions);
    }
};
AudioSessionMuteGuard::AudioSessionMuteGuard()noexcept{
    state_=new(std::nothrow) State;
    if(state_){state_->backend=new(std::nothrow) WinBackend;state_->ownsBackend=true;if(!state_->backend)state_->Error(E_OUTOFMEMORY);}
}
#ifdef AUDIO_SESSION_MUTE_TESTING
AudioSessionMuteGuard::AudioSessionMuteGuard(AudioSessionMuteDetail::Backend* backend)noexcept{state_=new(std::nothrow) State;if(state_)state_->backend=backend;}
#endif
void AudioSessionMuteGuard::Update(DWORD exactPid,bool masterKnown,bool masterZero)noexcept{
    const DWORD incoming=GetLastError();
    try{
        State* s=state_;if(s&&s->SameThread()){
            s->stats.lastResult=S_OK;s->stats.matchedSessions=0;s->authorizedPid=exactPid;
            if(!exactPid||!masterKnown||!masterZero||s->pid!=exactPid){s->RefreshBindings();s->RestoreOwned();if(s->owned.empty())s->pid=exactPid;}
            if(exactPid&&masterKnown&&masterZero&&s->pid==exactPid&&s->backend){
                if(!s->stats.initialized){const HRESULT hr=s->backend->Initialize();if(SUCCEEDED(hr))s->stats.initialized=true;else s->Error(hr);}
                if(s->stats.initialized){
                    std::vector<std::shared_ptr<AudioSessionMuteDetail::Session>> sessions;const HRESULT enumResult=s->backend->Enumerate(sessions);if(FAILED(enumResult))s->Error(enumResult);
                    s->Rebind(sessions);
                    for(const auto& session:sessions){
                        if(!session||!s->Eligible(session,exactPid))continue;++s->stats.matchedSessions;
                        const auto found=std::find_if(s->owned.begin(),s->owned.end(),[&](const State::Owned& entry){return entry.session->Key()==session->Key();});
                        BOOL muted=FALSE;HRESULT hr=session->GetMute(&muted);if(hr!=S_OK){s->Error(hr);continue;}
                        if(found!=s->owned.end()){
                            found->session=session;
                            if(!muted){hr=session->SetMute(TRUE,&MuteContext);if(hr==S_OK)++s->stats.changes;else s->Error(hr);}continue;
                        }
                        if(muted)continue; // pre-existing mute is never owned
                        // Reserve ownership before writing so allocation failure
                        // cannot leave an unrecorded mute behind.
                        s->owned.push_back({session,exactPid,FALSE});
                        hr=session->SetMute(TRUE,&MuteContext);if(hr==S_OK)++s->stats.changes;else{s->owned.pop_back();s->Error(hr);}
                    }
                    // Expiration does not restore persisted per-application
                    // mute. Attempt our own prior-state restore before dropping
                    // an expired session; keep transient failures for retry.
                    for(size_t i=0;i<s->owned.size();){int status=0;const HRESULT hr=s->owned[i].session->State(&status);if((Gone(hr)||(hr==S_OK&&status==2))&&s->TryRestore(s->owned[i]))s->owned.erase(s->owned.begin()+static_cast<ptrdiff_t>(i));else ++i;}
                }
                s->stats.ownedMutes=static_cast<DWORD>(s->owned.size());
            }
        }
    }catch(...){if(state_)state_->Error(E_OUTOFMEMORY);}SetLastError(incoming);
}
void AudioSessionMuteGuard::Restore()noexcept{const DWORD incoming=GetLastError();try{if(state_&&state_->SameThread()){state_->stats.lastResult=S_OK;state_->RefreshBindings();state_->RestoreOwned();}}catch(...){if(state_)state_->Error(E_UNEXPECTED);}SetLastError(incoming);}
AudioSessionMuteStats AudioSessionMuteGuard::Stats()const noexcept{if(state_)return state_->stats;AudioSessionMuteStats s{};s.lastResult=E_OUTOFMEMORY;s.errors=1;return s;}
AudioSessionMuteGuard::~AudioSessionMuteGuard()noexcept{
    if(!state_)return;
    if(state_->thread!=GetCurrentThreadId())return; // never release apartment objects on a foreign thread
    Restore();state_->owned.clear();if(state_->backend){state_->backend->Shutdown();if(state_->ownsBackend)delete state_->backend;}delete state_;
}
