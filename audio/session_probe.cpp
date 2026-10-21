// Observe only: this utility contains no SetMute/SetMasterVolume calls.
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#ifndef AUDCLNT_S_NO_SINGLE_PROCESS
#define AUDCLNT_S_NO_SINGLE_PROCESS AUDCLNT_SUCCESS(0x00d)
#endif
template<class T>struct ProbeCom{T* p=nullptr;~ProbeCom(){if(p)p->Release();}T** Out(){return &p;}};
int wmain(int argc,wchar_t** argv){
    if(argc!=3||std::wcscmp(argv[1],L"--observe-only")){std::fwprintf(stderr,L"Usage: session_probe.exe --observe-only exactProcessId\n");return 2;}
    wchar_t* end=nullptr;const unsigned long value=std::wcstoul(argv[2],&end,10);if(!value||!end||*end)return 2;const DWORD target=value;
    const HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(init)){std::printf("CoInitialize=0x%08lX\n",static_cast<unsigned long>(init));return 1;}
    int exitCode=0;
    {
        ProbeCom<IMMDeviceEnumerator> enumerator;HRESULT hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(enumerator.Out()));
        ProbeCom<IMMDeviceCollection> endpoints;if(SUCCEEDED(hr))hr=enumerator.p->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,endpoints.Out());
        UINT endpointCount=0;if(SUCCEEDED(hr))hr=endpoints.p->GetCount(&endpointCount);
        if(FAILED(hr)){std::printf("enumeration=0x%08lX\n",static_cast<unsigned long>(hr));exitCode=1;}
        else{
            unsigned matched=0,rejectedCrossProcess=0,total=0;std::printf("observe_only=1 target_pid=%lu active_render_endpoints=%u\n",target,endpointCount);
            for(UINT e=0;e<endpointCount;++e){
                ProbeCom<IMMDevice> endpoint;if(FAILED(endpoints.p->Item(e,endpoint.Out())))continue;
                ProbeCom<IAudioSessionManager2> manager;if(FAILED(endpoint.p->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.Out()))))continue;
                ProbeCom<IAudioSessionEnumerator> sessions;if(FAILED(manager.p->GetSessionEnumerator(sessions.Out())))continue;
                int count=0;if(FAILED(sessions.p->GetCount(&count)))continue;
                for(int i=0;i<count;++i){
                    ++total;ProbeCom<IAudioSessionControl> base;if(FAILED(sessions.p->GetSession(i,base.Out())))continue;
                    ProbeCom<IAudioSessionControl2> control;if(FAILED(base.p->QueryInterface(__uuidof(IAudioSessionControl2),reinterpret_cast<void**>(control.Out()))))continue;
                    DWORD pid=0;const HRESULT process=control.p->GetProcessId(&pid);if(process==AUDCLNT_S_NO_SINGLE_PROCESS){++rejectedCrossProcess;continue;}if(process!=S_OK||pid!=target)continue;
                    const HRESULT system=control.p->IsSystemSoundsSession();if(system!=S_FALSE)continue;
                    ProbeCom<ISimpleAudioVolume> volume;if(FAILED(base.p->QueryInterface(__uuidof(ISimpleAudioVolume),reinterpret_cast<void**>(volume.Out()))))continue;
                    BOOL mute=FALSE;float gain=0;AudioSessionState state{};const HRESULT muteHr=volume.p->GetMute(&mute),gainHr=volume.p->GetMasterVolume(&gain),stateHr=control.p->GetState(&state);
                    std::printf("match endpoint=%u index=%d pid=%lu process_hr=0x%08lX system_hr=0x%08lX state_hr=0x%08lX state=%d mute_hr=0x%08lX mute=%u volume_hr=0x%08lX volume=%.9f\n",e,i,pid,static_cast<unsigned long>(process),static_cast<unsigned long>(system),static_cast<unsigned long>(stateHr),static_cast<int>(state),static_cast<unsigned long>(muteHr),mute?1u:0u,static_cast<unsigned long>(gainHr),static_cast<double>(gain));++matched;
                }
            }
            std::printf("total_sessions=%u matched=%u rejected_cross_process=%u no_audio_state_writes=1\n",total,matched,rejectedCrossProcess);
        }
    }
    CoUninitialize();return exitCode;
}
