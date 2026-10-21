#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

struct AudioSessionMuteStats {
    HRESULT lastResult=S_OK;
    DWORD matchedSessions=0,ownedMutes=0,changes=0,errors=0;
    bool initialized=false;
};

// Backend seam used by the isolated ownership/selection regression. Windows
// COM objects and every method below stay on the caller's one host thread.
namespace AudioSessionMuteDetail {
class Session {
public:
    virtual ~Session()=default;
    virtual const std::wstring& Key() const noexcept=0;
    virtual const std::wstring& StableKey() const noexcept=0;
    virtual HRESULT ProcessId(DWORD*) noexcept=0;
    virtual HRESULT IsSystem() noexcept=0;
    virtual HRESULT State(int*) noexcept=0; // inactive=0, active=1, expired=2
    virtual HRESULT GetMute(BOOL*) noexcept=0;
    virtual HRESULT SetMute(BOOL,const GUID*) noexcept=0;
};
class Backend {
public:
    virtual ~Backend()=default;
    virtual HRESULT Initialize() noexcept=0;
    virtual HRESULT Enumerate(std::vector<std::shared_ptr<Session>>&) =0;
    virtual void Shutdown() noexcept=0;
};
}

class AudioSessionMuteGuard {
public:
    AudioSessionMuteGuard() noexcept;
    ~AudioSessionMuteGuard() noexcept;
    AudioSessionMuteGuard(const AudioSessionMuteGuard&)=delete;
    AudioSessionMuteGuard& operator=(const AudioSessionMuteGuard&)=delete;
    void Update(DWORD exactPid,bool masterKnown,bool masterZero) noexcept;
    void Restore() noexcept;
    AudioSessionMuteStats Stats() const noexcept;
#ifdef AUDIO_SESSION_MUTE_TESTING
    explicit AudioSessionMuteGuard(AudioSessionMuteDetail::Backend*) noexcept;
#endif
private:
    struct State;
    State* state_=nullptr;
};
