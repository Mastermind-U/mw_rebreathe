#pragma once
#include <windows.h>
#include <cstdint>

// Cross-bitness channel. All addresses published/read by this module are native
// PE32 game addresses; no host pointer is placed in shared memory.
namespace GameAudioProtocol {
constexpr DWORD Magic=0x41574D4D,Version=1;
struct Binding {
    DWORD magic,version,size;
    volatile LONG sequence;
    DWORD active,pid,creationLow,creationHigh;
    DWORD hostPid,hostCreationLow,hostCreationHigh,reserved;
};
static_assert(sizeof(Binding)==48,"Stable x86/x64 audio binding protocol");
}
struct GameAudioSample {
    // pid is nonzero only while a held process handle matches the published
    // creation time and is unsignaled. Unknown is never interpreted as zero.
    DWORD pid=0;
    bool masterKnown=false;
    bool masterZero=false;
};
class HostGameAudioTarget {
public:
    // Single caller thread. Close only after sampling and owned-session mute
    // restoration/destruction have completed; the held handle pins the PID.
    HostGameAudioTarget() noexcept=default;
    ~HostGameAudioTarget() noexcept;
    HostGameAudioTarget(const HostGameAudioTarget&)=delete;
    HostGameAudioTarget& operator=(const HostGameAudioTarget&)=delete;
    // baseSession is MW_MODERN_SESSION. The module appends L"_audio".
    bool Create(const wchar_t* baseSession) noexcept;
    // Seven exact RPM reads, including repeated World/manager/quit checks.
    // A consistent snapshot is best effort; it is not a remote lifetime lock.
    GameAudioSample Sample() noexcept;
    void Close() noexcept;
    const char* Status() const noexcept {return status_;}
#ifdef MW_AUDIO_TARGET_TESTING
    DWORD PinnedProcessIdForTests() const noexcept {return process_?GetProcessId(process_):0;}
#endif
private:
    HANDLE mapping_=nullptr,process_=nullptr;
    GameAudioProtocol::Binding* view_=nullptr;
    DWORD pid_=0,creationLow_=0,creationHigh_=0;
    const char* status_="not created";
    void ReleaseProcess() noexcept;
};
class RunnerAudioTarget {
public:
    RunnerAudioTarget() noexcept=default;
    ~RunnerAudioTarget() noexcept;
    RunnerAudioTarget(const RunnerAudioTarget&)=delete;
    RunnerAudioTarget& operator=(const RunnerAudioTarget&)=delete;
    // Only publishes the exact child HANDLE returned by CreateProcess. Failure
    // is nonfatal (including an absent channel during probes).
    bool Publish(const wchar_t* baseSession,HANDLE childProcess) noexcept;
    void Close() noexcept;
    const char* Status() const noexcept {return status_;}
private:
    HANDLE mapping_=nullptr;
    GameAudioProtocol::Binding* view_=nullptr;
    DWORD pid_=0,creationLow_=0,creationHigh_=0;
    LONG generation_=0;
    const char* status_="not published";
};
#ifdef MW_AUDIO_TARGET_TESTING
using GameAudioReadMemory=BOOL(WINAPI*)(HANDLE,LPCVOID,LPVOID,SIZE_T,SIZE_T*);
void SetGameAudioTargetReadMemoryForTests(GameAudioReadMemory function) noexcept;
#endif
