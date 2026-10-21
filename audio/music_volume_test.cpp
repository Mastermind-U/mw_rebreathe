// Isolated x86 native music graph fixture. No game/window/audio device is run.
#define NOMINMAX
#include "music_volume.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
static_assert(sizeof(void*)==4,"x86 only");
namespace {
unsigned char* t_image=nullptr;
std::array<unsigned char,0x400> t_world{};
std::array<unsigned char,0x300> t_manager{};
std::array<void*,20> t_audioTable{},t_graphTable{},t_controlTable{},t_positionTable{};
struct Interface {void** table=nullptr;};
Interface t_audio,t_graph,t_control,t_position;
struct Volume {LONG db;DWORD incoming;bool playing;};
std::vector<Volume> t_volumes;
std::vector<char> t_events;
bool t_audioAlive=true,t_graphAlive=true,t_playing=true,t_qiFails=false,t_argFailure=false;
HRESULT t_putResult=S_OK,t_stopResult=S_OK;
LONG t_initialDb=0,t_backendDb=0,t_runDb=0;
unsigned t_runCount=0,t_nonfade=0,t_rollback=0;
DWORD t_entryEax=0,t_entryEcx=0,t_entryEdx=0,t_entryFlags=0,t_beforeFlags=0,t_entryError=0;
DWORD t_expectedError=0,t_clock=0,t_clockStep=60;
unsigned t_masterCalls=0,t_guards=0,t_abi=0,t_native=0,t_lifetime=0,t_logCalls=0;
std::string t_lastLog;
uintptr_t t_before=0,t_after=0;
DWORD t_result=0,t_bx=0,t_si=0,t_di=0,t_bp=0;
unsigned short t_cwBefore=0,t_cwAfter=0,t_swBefore=0,t_swAfter=0;
constexpr DWORD Incoming=0x12345678,Outgoing=0x87654321,MasterOutgoing=0x579ACE01,MasterResult=0xA13579BD;
struct Site {DWORD rva;size_t length;unsigned type;bool release;};
const Site Sites[]={{0x3715,5,0,false},{0x37bd,5,0,false},{0x3aaf,5,1,false},{0x3bc3,5,2,false},{0x3707,9,3,false},{0x37a7,9,3,false},{0x3a95,9,4,false},{0x1d58,5,2,true},{0x332e,5,2,true},{0x348e,5,2,true},{0x3c37,5,2,true},{0x1a1c9e,5,5,false},{0x10fe8,5,6,false}};
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T>T Get(const void* p,size_t o){T v{};std::memcpy(&v,static_cast<const unsigned char*>(p)+o,sizeof(v));return v;}
template<class T>void Put(void* p,size_t o,T v){std::memcpy(static_cast<unsigned char*>(p)+o,&v,sizeof(v));}
void Write(void* dest,const void* src,size_t n){DWORD old=0,ignored=0;Require(VirtualProtect(dest,n,PAGE_EXECUTE_READWRITE,&old)!=FALSE,"Private page write failed");std::memcpy(dest,src,n);Require(FlushInstructionCache(GetCurrentProcess(),dest,n)!=FALSE,"Private flush failed");Require(VirtualProtect(dest,n,old,&ignored)!=FALSE,"Private page restore failed");}
void Byte(DWORD rva,unsigned char v){Write(t_image+rva,&v,1);}
void Jump(DWORD rva,const void* target){unsigned char b[5]{0xE9};const DWORD delta=static_cast<DWORD>(reinterpret_cast<uintptr_t>(target))-static_cast<DWORD>(reinterpret_cast<uintptr_t>(t_image+rva+5));std::memcpy(b+1,&delta,4);Write(t_image+rva,b,5);}
void RelativeJump(DWORD from,DWORD to){unsigned char b[5]{0xE9};const DWORD delta=to-from-5;std::memcpy(b+1,&delta,4);Write(t_image+from,b,5);}
void RebaseRange(DWORD rva,size_t length){
    const DWORD values[]={0x7C67DC,0x746468,0x746470,0x7464C8,0x7464CC,0x7464D0,0x7464D4,0x7464D8,0x74B920,0x7463E8,0x7462F0,0x74625C,0x746440,0x746124,0x74B8E0,0x74B930,0x74B8F0,0x74B900};
    for(size_t i=0;i+4<=length;++i){const DWORD value=Get<DWORD>(t_image,rva+i);for(DWORD original:values)if(value==original){const DWORD relocated=static_cast<DWORD>(reinterpret_cast<uintptr_t>(t_image))+original-0x400000;Write(t_image+rva+i,&relocated,4);i+=3;break;}}
}
DWORD __stdcall FakePutVolume(void* receiver,LONG db){
    const DWORD error=GetLastError();if(receiver!=&t_audio||!t_audioAlive||(t_expectedError&&error!=t_expectedError))t_argFailure=true;
    t_volumes.push_back({db,error,t_playing});t_events.push_back('V');if(SUCCEEDED(t_putResult))t_backendDb=db;SetLastError(Outgoing);return static_cast<DWORD>(t_putResult);
}
DWORD __stdcall FakeGetVolume(void* receiver,LONG* out){if(receiver!=&t_audio||!out)t_argFailure=true;if(out)*out=t_backendDb;return S_OK;}
DWORD __stdcall FakeAudioRelease(void* receiver){if(receiver!=&t_audio||!t_audioAlive)t_argFailure=true;t_events.push_back('A');t_audioAlive=false;return 0;}
DWORD __stdcall FakeGraphRelease(void* receiver){if(receiver!=&t_graph||!t_graphAlive)t_argFailure=true;t_events.push_back('G');t_graphAlive=false;return 0;}
DWORD __stdcall FakeControlRelease(void* receiver){if(receiver!=&t_control)t_argFailure=true;t_events.push_back('C');return 1;}
DWORD __stdcall FakeStop(void* receiver){if(receiver!=&t_control)t_argFailure=true;t_events.push_back('S');if(SUCCEEDED(t_stopResult))t_playing=false;return static_cast<DWORD>(t_stopResult);}
DWORD __stdcall FakeQuery(void* receiver,const void* iid,void** out){
    if(receiver!=&t_graph||!iid||!out)t_argFailure=true;t_events.push_back('Q');
    if(t_qiFails)return static_cast<DWORD>(E_NOINTERFACE);
    if(iid==t_image+0x34B8F0)*out=&t_audio;
    else if(iid==t_image+0x34B900)*out=&t_position;
    else if(iid==t_image+0x34B920)*out=&t_control;
    else {t_argFailure=true;return static_cast<DWORD>(E_NOINTERFACE);}return S_OK;
}
DWORD __stdcall FakeRun(void* receiver){if(receiver!=&t_control)t_argFailure=true;t_events.push_back('R');++t_runCount;t_runDb=t_backendDb;t_playing=true;return S_OK;}
DWORD __stdcall FakePause(void* receiver){if(receiver!=&t_control)t_argFailure=true;t_events.push_back('p');return S_OK;}
DWORD __stdcall FakePositionRelease(void* receiver){if(receiver!=&t_position)t_argFailure=true;return 1;}
DWORD __stdcall FakeSeek(void* receiver,DWORD low,DWORD high){if(receiver!=&t_position||low||high)t_argFailure=true;return S_OK;}
DWORD __stdcall FakeRender(void* receiver,const wchar_t* name,const wchar_t* list){if(receiver!=&t_graph||!name||!name[0]||list)t_argFailure=true;t_events.push_back('L');return S_OK;}
int __cdecl FakeAccess(const char* file,int mode){if(!file||!file[0]||mode)t_argFailure=true;return 0;}
int __cdecl FakeCompare(const char* first,const char* second){return std::strcmp(first,second);}
DWORD __stdcall FakeCreate(const void* cls,void* outer,DWORD context,const void* iid,void** out){
    if(cls!=t_image+0x34B930||outer||context!=1||iid!=t_image+0x34B8E0||!out)t_argFailure=true;
    *out=&t_graph;t_audioAlive=t_graphAlive=true;t_playing=false;t_backendDb=t_initialDb;t_events.push_back('N');return S_OK;
}
DWORD __fastcall ObservedPlay(void* manager,void*){
    if(manager!=t_manager.data())t_argFailure=true;SetLastError(Outgoing);return MasterResult;
}
__declspec(naked) void ObservePlayEntry(){
    __asm{
        mov t_entryEax,eax
        mov t_entryEcx,ecx
        mov t_entryEdx,edx
        pushfd
        pop t_entryFlags
        mov eax,dword ptr fs:[0x34]
        mov t_entryError,eax
        mov eax,t_entryEax
        jmp ObservedPlay
    }
}
void __cdecl IgnoreLog(const char*,...){}
DWORD __stdcall FakeClock(){const DWORD value=t_clock;t_clock+=t_clockStep;return value;}
DWORD __cdecl OriginalMaster(){if(t_expectedError&&GetLastError()!=t_expectedError)t_argFailure=true;++t_masterCalls;t_expectedError=MasterOutgoing;SetLastError(MasterOutgoing);return MasterResult;}
void __fastcall PrepareTrack(void* manager,void*,const char*){t_events.push_back('P');Put<DWORD>(manager,8,1);t_audioAlive=t_graphAlive=true;t_playing=false;}
void __fastcall PlayTrack(void* manager,void*){t_events.push_back('R');Put<DWORD>(manager,8,Get<DWORD>(manager,8)|2);t_playing=true;}
void ObserveLog(const char* line){++t_logCalls;t_lastLog=line?line:"";SetLastError(0xBAD00BAD);}
__declspec(naked) DWORD __stdcall NativeSite(void*,void*,void*,LONG,unsigned){
    __asm{
        push ebx
        push esi
        push edi
        push ebp
        mov edi,dword ptr[esp+20]
        mov esi,dword ptr[esp+24]
        mov ebx,esi
        mov ebp,0x44556677
        mov eax,dword ptr[esp+28]
        mov ecx,dword ptr[esp+36]
        cmp ecx,6
        je beforePlay
        cmp ecx,1
        je ecxGraph
        cmp ecx,4
        je ecxGraph
        cmp ecx,2
        je eaxEdx
        mov ecx,dword ptr[esp+32]
        mov edx,dword ptr[eax]
        jmp invokeSite
    ecxGraph:
        mov ecx,eax
        mov edx,dword ptr[esp+32]
        mov eax,dword ptr[ecx]
        jmp invokeSite
    eaxEdx:
        mov edx,dword ptr[esp+32]
        mov ecx,dword ptr[eax]
        jmp invokeSite
    beforePlay:
        mov ecx,esi
        mov eax,0x13579BDF
        mov edx,0x2468ACE0
    invokeSite:
        pushfd
        pop t_beforeFlags
        mov t_before,esp
        fnstcw t_cwBefore
        fnstsw t_swBefore
        call edi
        fnstsw t_swAfter
        fnstcw t_cwAfter
        mov t_result,eax
        mov t_after,esp
        mov t_bx,ebx
        mov t_si,esi
        mov t_di,edi
        mov t_bp,ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 20
    }
}
__declspec(naked) DWORD __stdcall NativeMethod(void*,void*,DWORD,bool){
    __asm{
        push ebx
        push esi
        push edi
        push ebp
        mov edx,dword ptr[esp+20]
        mov ecx,dword ptr[esp+24]
        mov ebx,0x11223344
        mov esi,0x22334455
        mov edi,0x33445566
        mov ebp,0x44556677
        mov t_before,esp
        cmp dword ptr[esp+32],0
        je noArgument
        push dword ptr[esp+28]
    noArgument:
        call edx
        mov t_result,eax
        mov t_after,esp
        mov t_bx,ebx
        mov t_si,esi
        mov t_di,edi
        mov t_bp,ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 16
    }
}
void CheckMethod(){Require(t_before==t_after&&t_bx==0x11223344&&t_si==0x22334455&&t_di==0x33445566&&t_bp==0x44556677,"Native method ABI/RET mismatch");Require(!t_argFailure,"Native fake COM receiver/lifetime/LastError mismatch");}
void CheckSite(DWORD rva){Require(t_before==t_after&&t_bx==reinterpret_cast<uintptr_t>(t_manager.data())&&t_si==reinterpret_cast<uintptr_t>(t_manager.data())&&t_di==reinterpret_cast<uintptr_t>(t_image+rva)&&t_bp==0x44556677,"Patched sequence ABI mismatch");Require(!t_argFailure,"Patched fake COM mismatch");Require(t_cwBefore==t_cwAfter&&((t_swBefore^t_swAfter)&0x3800)==0,"Patched sequence changed x87 control word or stack depth");}
std::vector<unsigned char> PatchBytes(){std::vector<unsigned char> out;for(const auto& s:Sites)out.insert(out.end(),t_image+s.rva,t_image+s.rva+s.length);return out;}
void Active(unsigned master,float raw=0.8f,unsigned flags=3){t_manager.fill(0);t_world.fill(0);Put(t_world.data(),0x34,t_manager.data());Put(t_manager.data(),8,flags);t_manager[0x290]=static_cast<unsigned char>(master);Put(t_manager.data(),0x294,raw);Put<LONG>(t_manager.data(),0x29C,0);Put(t_manager.data(),0x2A0,&t_graph);Put(t_manager.data(),0x2A4,&t_audio);const auto world=t_world.data();Write(t_image+0x3C67DC,&world,4);t_audioAlive=t_graphAlive=t_playing=true;t_qiFails=false;t_putResult=t_stopResult=S_OK;t_expectedError=0;t_argFailure=false;t_volumes.clear();t_events.clear();t_clock=0;t_clockStep=60;t_initialDb=0;t_backendDb=0;t_runDb=0;t_runCount=0;}
void Setter(float raw){DWORD bits=0;std::memcpy(&bits,&raw,4);SetLastError(Incoming);NativeMethod(t_image+0x3A10,t_manager.data(),bits,true);CheckMethod();Require(GetLastError()==Outgoing,"Real music setter LastError changed");}
void Master(unsigned value){t_manager[0x290]=static_cast<unsigned char>(value);t_expectedError=Incoming;SetLastError(Incoming);NativeSite(t_image+0x1A1C9E,t_manager.data(),&t_audio,0,5);CheckSite(0x1A1C9E);Require(t_result==MasterResult&&GetLastError()==MasterOutgoing,"Master wrapper lost native EAX/LastError");t_expectedError=0;}
void NativeSetterProof(){
    struct Input{float raw;LONG native;};const Input values[]={{0,-10000},{0.2f,-3200},{0.8f,-800},{1.0f,0}};
    for(unsigned master:{0u,125u,250u})for(const auto& v:values){Active(master);const size_t before=t_volumes.size();t_expectedError=Incoming;Setter(v.raw);Require(t_volumes.size()==before+1,"Real setter did not call native graph");const LONG expected=master==0?-10000:v.native==-10000?-10000:master==125?v.native-602:v.native;Require(t_volumes.back().db==expected,"Real MusicSlider/native curve/Master mix unexpected");Require(Get<DWORD>(t_manager.data(),0x294)==Get<DWORD>(&v.raw,0),"Raw MusicSlider value changed");Require(Get<LONG>(t_manager.data(),0x29C)==0,"Native saved graph volume changed");++t_native;}
    for(auto result:{S_OK,E_FAIL,E_ACCESSDENIED}){Active(125);t_putResult=result;t_expectedError=Incoming;Setter(0.8f);Require(t_result==static_cast<DWORD>(result),"COM HRESULT not preserved");++t_native;}
}
void FadesAndLifetime(){
    // Real update fade arithmetic: only unrelated media-event processing at
    // 0x4037CC is bypassed to the original function's own epilogue.
    Active(250);t_manager[0x7C]='x';Put<DWORD>(t_world.data(),0x20,100);Put<DWORD>(t_manager.data(),0x284,0);Put<DWORD>(t_manager.data(),0x288,200);
    SetLastError(Incoming);NativeMethod(t_image+0x3650,t_manager.data(),0,false);CheckMethod();Require(t_volumes.size()==1&&t_volumes.back().db==-2400,"Real update fade math unexpected");const DWORD rawBits=Get<DWORD>(t_manager.data(),0x294);++t_native;
    Master(0);Require(t_volumes.back().db==-10000,"Already-playing fade did not mute immediately");
    Put<DWORD>(t_world.data(),0x20,150);SetLastError(Incoming);NativeMethod(t_image+0x3650,t_manager.data(),0,false);CheckMethod();Require(t_volumes.back().db==-10000,"Fade frame unmuted Master0");
    Master(250);Require(t_volumes.back().db==-3200,"Restoring Master restarted initial gain instead of current fade");Require(Get<DWORD>(t_manager.data(),0x294)==rawBits,"Fade/Master changed raw Music");++t_native;
    // Real pending-track commit clamps/stores its native requested value and
    // sends the initial graph volume before native play. Only file/COM graph
    // creation and Run services are represented by recording stubs.
    Active(0);Put<DWORD>(t_manager.data(),8,0);t_audioAlive=t_graphAlive=t_playing=false;t_manager[0x7C]='x';Put<DWORD>(t_world.data(),0x20,201);Put<DWORD>(t_manager.data(),0x288,200);Put<float>(t_manager.data(),0x28C,0.6f);
    SetLastError(Incoming);NativeMethod(t_image+0x3650,t_manager.data(),0,false);CheckMethod();Require(t_volumes.size()==1&&t_volumes.back().db==-10000&&!t_volumes.back().playing&&Get<float>(t_manager.data(),0x294)==0.6f,"Initial/track-change volume escaped Master0");Require(std::find(t_events.begin(),t_events.end(),'P')!=t_events.end()&&std::find(t_events.begin(),t_events.end(),'R')!=t_events.end(),"Native track commit did not use prepare/play boundaries");++t_native;
    // Real blocking fade + Stop + restore + releases, including failed Stop.
    for(bool stopFails:{false,true})for(unsigned master:{0u,250u}){
        Active(master);t_stopResult=stopFails?E_FAIL:S_OK;SetLastError(Incoming);NativeMethod(t_image+0x3B20,t_manager.data(),100,true);CheckMethod();Require(!t_volumes.empty()&&!t_audioAlive&&!t_graphAlive&&Get<DWORD>(t_manager.data(),8)==0,"Blocking fade did not release native graph");if(!master)for(size_t i=0;i+1<t_volumes.size();++i)Require(t_volumes[i].db==-10000,"Blocking active fade unmuted Master0");Require(t_volumes.back().db==0,"Blocking release did not pass original saved dB");const auto stop=std::find(t_events.begin(),t_events.end(),'S'),release=std::find(t_events.begin(),t_events.end(),'A');Require(stop!=t_events.end()&&stop<release,"Native blocking fade Stop/Release order differs");const size_t rows=t_volumes.size();Master(125);Require(t_volumes.size()==rows,"Master used cached graph after blocking fade release");++t_lifetime;
    }
    // Real native Stop path proves restore invalidation before release. Same
    // pointer then deliberately represents a newly allocated graph (ABA).
    for(bool stopFails:{false,true}){
        Active(250);Setter(0.8f);t_stopResult=stopFails?E_FAIL:S_OK;t_manager[0x290]=0;
        SetLastError(Incoming);NativeMethod(t_image+0x3420,t_manager.data(),0,false);CheckMethod();Require(t_volumes.back().db==0&&!t_audioAlive&&!t_graphAlive,"Stop restore did not pass saved dB or release");const size_t rows=t_volumes.size();Master(250);Require(t_volumes.size()==rows,"Master invoked stale stopped graph");
        t_audioAlive=t_graphAlive=t_playing=true;Put<DWORD>(t_manager.data(),8,3);Put<float>(t_manager.data(),0x294,0.3f);Master(125);Require(t_volumes.back().db==-3402&&t_lastLog.find("cached=0")!=std::string::npos,"Graph address reuse consumed previous lifetime's cached fade");Require(Get<float>(t_manager.data(),0x294)==0.3f,"Bootstrap changed raw Music value");++t_lifetime;
    }
    // Start replacement and destructor restore/release fragments execute their
    // real register setup/Release calls. Their outer non-audio work is omitted.
    for(DWORD fragment:{0x3320u,0x1D4Au}){
        Active(250);Setter(0.8f);t_manager[0x290]=0;SetLastError(Incoming);NativeSite(t_image+fragment,t_manager.data(),&t_audio,0,0);CheckSite(fragment);Require(t_volumes.back().db==0&&!t_audioAlive&&!t_graphAlive,"Lifecycle fragment lost saved-volume restore/release");
        t_audioAlive=t_graphAlive=t_playing=true;Put<DWORD>(t_manager.data(),8,3);Put<float>(t_manager.data(),0x294,0.6f);Master(250);Require(t_volumes.back().db==-1600&&t_lastLog.find("cached=0")!=std::string::npos,"Lifecycle fragment did not invalidate ABA cache");++t_lifetime;
    }
    // Invalidation still runs during World teardown; original restore remains
    // unscaled when World is unavailable, with no extra cached refresh.
    Active(250);Setter(0.8f);t_world[0xDB]=1;Put<LONG>(t_manager.data(),0x29C,-123);SetLastError(Incoming);NativeMethod(t_image+0x3420,t_manager.data(),0,false);CheckMethod();Require(t_volumes.back().db==-123,"Quit restore did not preserve original native value");
    t_world[0xDB]=0;t_audioAlive=t_graphAlive=t_playing=true;Put<DWORD>(t_manager.data(),8,3);Put<float>(t_manager.data(),0x294,0.2f);Master(250);Require(t_volumes.back().db==-3200&&t_lastLog.find("cached=0")!=std::string::npos,"Quit restore retained stale graph cache");++t_lifetime;
}
void ReleasePassthroughCases(){
    // Every actual lifecycle patch is invoked with live/quit/null/invalid World.
    // Even unavailable World must not skip invalidation or alter saved dB.
    for(const auto& site:Sites)if(site.release)for(unsigned master:{0u,250u})for(unsigned worldCase:{0u,1u,2u,3u}){
        Active(250);Setter(0.8f);t_manager[0x290]=static_cast<unsigned char>(master);Put<LONG>(t_manager.data(),0x29C,-1234);
        const auto saved=t_image[site.rva+site.length];Byte(site.rva+static_cast<DWORD>(site.length),0xC3);
        if(worldCase==1)t_world[0xDB]=1;
        if(worldCase>=2){void* world=worldCase==2?nullptr:reinterpret_cast<void*>(1);Write(t_image+0x3C67DC,&world,4);}
        t_expectedError=Incoming;SetLastError(Incoming);NativeSite(t_image+site.rva,t_manager.data(),&t_audio,-1234,site.type);CheckSite(site.rva);
        Require(t_volumes.back().db==-1234&&t_result==S_OK&&GetLastError()==Outgoing,"Lifecycle changed original saved dB/HRESULT/LastError");
        Require(Get<float>(t_manager.data(),0x294)==0.8f&&Get<LONG>(t_manager.data(),0x29C)==-1234,"Lifecycle wrote raw/saved native setting");Byte(site.rva+static_cast<DWORD>(site.length),saved);
        t_world[0xDB]=0;const auto world=t_world.data();Write(t_image+0x3C67DC,&world,4);Put<float>(t_manager.data(),0x294,0.3f);Master(250);
        Require(t_volumes.back().db==-2800&&t_lastLog.find("cached=0")!=std::string::npos,"Lifecycle bypass failed to invalidate cached graph epoch");++t_lifetime;
    }
    // Failure to acquire IMediaControl does not remove the original restore
    // contract. The backend-failure silence limit is deliberately unchanged.
    Active(250);Setter(0.8f);t_qiFails=true;t_manager[0x290]=0;SetLastError(Incoming);NativeMethod(t_image+0x3420,t_manager.data(),0,false);CheckMethod();Require(t_volumes.back().db==0&&!t_audioAlive&&!t_graphAlive,"QI failure changed restore/release contract");Require(std::find(t_events.begin(),t_events.end(),'S')==t_events.end(),"QI negative control unexpectedly called Stop");++t_lifetime;
}
void NonfadeGraphProof(){
    // Original World::PlayMusic(false), LoadGraph, QI/get_Volume and Run all
    // execute. Only OS/file/COM services use recording doubles.
    // Negative control: the other twelve patches alone leave a fresh graph
    // at its native full level. The thirteenth seam is necessary before Run.
    unsigned char patched[5]{};std::memcpy(patched,t_image+0x10FE8,5);
    const unsigned char original[]{0xE8,0xD3,0x24,0xFF,0xFF};Write(t_image+0x10FE8,original,5);
    Active(250);Put<DWORD>(t_manager.data(),8,1);NativeMethod(t_image+0x3420,t_manager.data(),0,false);CheckMethod();
    Active(0,0.37f,0);t_playing=false;t_audioAlive=t_graphAlive=false;
    using WorldPlay=void(__thiscall*)(void*,const char*,bool);
    reinterpret_cast<WorldPlay>(t_image+0x10FA0)(t_world.data(),"fixture.mp3",false);
    Require(!t_argFailure&&t_runCount==1&&t_runDb==0&&t_volumes.empty(),"Old-seam negative control failed to reproduce unattenuated nonfade start");
    Write(t_image+0x10FE8,patched,5);++t_nonfade;
    for(LONG initial:{0L,-345L})for(unsigned master:{0u,125u,250u})for(HRESULT result:{S_OK,E_FAIL,E_ACCESSDENIED}){
        Active(250);Put<DWORD>(t_manager.data(),8,1);t_playing=false;NativeMethod(t_image+0x3420,t_manager.data(),0,false);CheckMethod(); // invalidate prior lifetime
        Active(master,0.37f,0);t_playing=false;t_audioAlive=t_graphAlive=false;t_initialDb=initial;t_putResult=result;
        const DWORD raw=Get<DWORD>(t_manager.data(),0x294);SetLastError(Incoming);
        reinterpret_cast<WorldPlay>(t_image+0x10FA0)(t_world.data(),"fixture.mp3",false);
        Require(!t_argFailure&&t_runCount==1&&t_playing,"Original nonfade LoadGraph/Run graph services failed");
        const LONG expected=master==0?-10000:master==125?initial-602:initial;
        Require(t_runDb==(SUCCEEDED(result)?expected:initial),"Fresh nonfade graph began Run with unexpected backend gain");
        Require(t_volumes.size()==(master==250?0u:1u),"Master250 changed original graph volume before Run");
        if(master<250)Require(!t_volumes.front().playing&&t_volumes.front().db==expected,"Nonfade attenuation occurred after Run or requested wrong gain");
        Require(Get<LONG>(t_manager.data(),0x29C)==initial&&Get<DWORD>(t_manager.data(),0x294)==raw,"Nonfade bootstrap changed saved/raw native fields");
        Require((Get<DWORD>(t_manager.data(),8)&3)==3&&Get<DWORD>(t_world.data(),0x350)==2,"Original nonfade state/publication not preserved");
        t_putResult=S_OK;
        Master(0);Require(t_backendDb==-10000,"Nonfade live Master did not mute");
        Master(250);Require(t_backendDb==initial,"Nonfade Master restore lost original graph gain");
        ++t_nonfade;
    }
    // A replay of an existing fade graph must use latest unscaled fade, not
    // saved initial graph volume. Same filename makes original LoadGraph skip.
    Active(250);Setter(0.2f);strcpy_s(reinterpret_cast<char*>(t_manager.data()+0x180),0x104,"fixture.mp3");
    t_manager[0x290]=125;t_playing=false;SetLastError(Incoming);
    reinterpret_cast<WorldPlay>(t_image+0x10FA0)(t_world.data(),"fixture.mp3",false);
    Require(!t_argFailure&&t_runDb==-3802,"Nonfade replay discarded current fade cache");
    Master(250);Require(t_backendDb==-3200,"Nonfade replay Master restore restarted initial gain");++t_nonfade;
}
void SequenceAbi(){
    for(const Site& site:Sites)Byte(site.rva+static_cast<DWORD>(site.length),0xC3);
    for(const Site& site:Sites)for(unsigned master:{0u,125u,250u})for(HRESULT result:{S_OK,E_FAIL,E_ACCESSDENIED}){
        Active(master);t_putResult=result;t_expectedError=Incoming;if(site.type==5){Put<DWORD>(t_manager.data(),8,0);}
        if(site.type==6){t_expectedError=0;t_entryError=t_entryEax=t_entryEcx=t_entryEdx=t_entryFlags=0;}
        SetLastError(Incoming);NativeSite(t_image+site.rva,t_manager.data(),&t_audio,-2600,site.type);CheckSite(site.rva);
        if(site.type==5){Require(t_result==MasterResult&&GetLastError()==MasterOutgoing&&t_volumes.empty(),"Master callback ABI/bypass mismatch");}
        else if(site.type==6){Require(t_result==MasterResult&&GetLastError()==Outgoing,"BeforePlay lost native Run return/LastError");Require(t_entryEax==0x13579BDF&&t_entryEcx==reinterpret_cast<uintptr_t>(t_manager.data())&&t_entryEdx==0x2468ACE0&&t_entryFlags==t_beforeFlags&&t_entryError==Incoming,"BeforePlay changed native incoming volatile registers/flags/LastError");}
        else {const LONG native=site.type==3||site.type==4?-10000:-2600;const LONG expected=site.release?native:!master?-10000:native==-10000?-10000:master==125?native-602:native;Require(t_volumes.size()==1&&t_volumes.back().db==expected&&t_result==static_cast<DWORD>(result)&&GetLastError()==Outgoing,"Native sequence changed COM args/EAX/LastError");}
        Require(Get<float>(t_manager.data(),0x294)==0.8f&&Get<LONG>(t_manager.data(),0x29C)==0,"Patched sequence changed native stored volume fields");++t_abi;
    }
}
} // namespace
int wmain(int argc,wchar_t** argv){
    HANDLE game=INVALID_HANDLE_VALUE,mapping=nullptr;
    try{
        Require(argc==2,"Pass original Morrowind.exe");game=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);Require(game!=INVALID_HANDLE_VALUE,"Cannot open original EXE");mapping=CreateFileMappingW(game,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);Require(mapping!=nullptr,"Cannot create private image");t_image=static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Require(t_image!=nullptr,"Cannot map private image");
        Require(!InstallMusicVolumeHooks(nullptr)&&!InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(1)),"Accepted invalid image");t_guards+=2;
        std::set<DWORD> corrupt;for(const auto& s:Sites)for(DWORD i=0;i<s.length;++i)corrupt.insert(s.rva+i);for(const auto& range:std::vector<std::pair<DWORD,unsigned>>{{0x3a10,170},{0x3650,16},{0x3b20,18},{0x1a1c8f,20},{0x1a1e10,21},{0x33e9,34},{0x34b8f0,16},{0x10fa0,91},{0x34c0,174}})for(DWORD i=0;i<range.second;++i)corrupt.insert(range.first+i);
        for(DWORD rva:corrupt){const auto original=t_image[rva];Byte(rva,static_cast<unsigned char>(original^0x5A));const auto before=PatchBytes();Require(!InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(t_image)),"Accepted corrupt music instruction/ABI/IID guard");Require(PatchBytes()==before,"Rejected music image was partially patched");Byte(rva,original);++t_guards;}
        for(const auto failure:{std::pair<int,int>{0,1},{0,2},{0,3},{0,4},{1,0},{2,0},{3,0},{4,0},{0,5},{0,6},{0,7},{0,8}}){
            const auto before=PatchBytes();std::array<DWORD,4> modes{};size_t index=0;for(DWORD rva:{0x1000u,0x3000u,0x10000u,0x1A1000u}){MEMORY_BASIC_INFORMATION info{};Require(VirtualQuery(t_image+rva,&info,sizeof(info))!=0,"Protection query failed");modes[index++]=info.Protect;}
            SetMusicVolumeTestFailures(failure.first,failure.second);Require(!InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(t_image)),"Installation API failure ignored");Require(PatchBytes()==before,"Music rollback did not restore all13 sequences");index=0;for(DWORD rva:{0x1000u,0x3000u,0x10000u,0x1A1000u}){MEMORY_BASIC_INFORMATION info{};Require(VirtualQuery(t_image+rva,&info,sizeof(info))&&info.Protect==modes[index++],"Music rollback lost original page protection");}++t_rollback;
        }
        SetMusicVolumeTestFailures(0,0);Require(InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(t_image)),"Music install failed");Require(InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(t_image)),"Music idempotence failed");Require(!InstallMusicVolumeHooks(reinterpret_cast<HMODULE>(1)),"Accepted a different installed image");
        t_audioTable[0x1C/4]=reinterpret_cast<void*>(&FakePutVolume);t_audioTable[0x20/4]=reinterpret_cast<void*>(&FakeGetVolume);t_audioTable[8/4]=reinterpret_cast<void*>(&FakeAudioRelease);t_audio.table=t_audioTable.data();t_graphTable[0]=reinterpret_cast<void*>(&FakeQuery);t_graphTable[8/4]=reinterpret_cast<void*>(&FakeGraphRelease);t_graph.table=t_graphTable.data();t_graphTable[0x34/4]=reinterpret_cast<void*>(&FakeRender);t_controlTable[0x1C/4]=reinterpret_cast<void*>(&FakeRun);t_controlTable[0x20/4]=reinterpret_cast<void*>(&FakePause);t_positionTable[0x20/4]=reinterpret_cast<void*>(&FakeSeek);t_positionTable[8/4]=reinterpret_cast<void*>(&FakePositionRelease);t_position.table=t_positionTable.data();t_controlTable[0x24/4]=reinterpret_cast<void*>(&FakeStop);t_controlTable[8/4]=reinterpret_cast<void*>(&FakeControlRelease);t_control.table=t_controlTable.data();
        RebaseRange(0x3A10,170);RebaseRange(0x3650,0x1B0);RebaseRange(0x3420,0x95);RebaseRange(0x3B20,0x144);RebaseRange(0x1D4A,0x32);RebaseRange(0x3320,0x32);RebaseRange(0x3270,0x1AD);RebaseRange(0x34C0,174);
        const auto compare=&FakeCompare;Write(t_image+0x3462F0,&compare,4);const auto access=&FakeAccess;Write(t_image+0x34625C,&access,4);const auto create=&FakeCreate;Write(t_image+0x346440,&create,4);const auto wide=&MultiByteToWideChar;Write(t_image+0x346124,&wide,4);
        const auto clock=&FakeClock;Write(t_image+0x3463E8,&clock,4);Jump(0x2E9A00,reinterpret_cast<const void*>(&IgnoreLog));Jump(0x76E20,reinterpret_cast<const void*>(&IgnoreLog));Jump(0x1A1E10,reinterpret_cast<const void*>(&OriginalMaster));
        // The native update's media-event loop is unrelated to volume. Use its
        // own original function epilogue after the measured fade/commit branch.
        RelativeJump(0x37CC,0x3995);Byte(0x1D7C,0xC3);Byte(0x1A1CA3,0xC3);SetMusicVolumeLogCallback(&ObserveLog);
        NativeSetterProof();NonfadeGraphProof();Jump(0x3270,reinterpret_cast<const void*>(&PrepareTrack));Jump(0x34C0,reinterpret_cast<const void*>(&PlayTrack));Byte(0x3352,0xC3);FadesAndLifetime();ReleasePassthroughCases();Jump(0x34C0,reinterpret_cast<const void*>(&ObservePlayEntry));SequenceAbi();
        std::printf("PASS native=%u lifecycle=%u guards=%u patchedSequenceABI=%u masterCallbacks=%u logCalls=%u nonfade=%u rollback=%u\n",t_native,t_lifetime,t_guards,t_abi,t_masterCalls,t_logCalls,t_nonfade,t_rollback);
        UnmapViewOfFile(t_image);CloseHandle(mapping);CloseHandle(game);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());if(t_image)UnmapViewOfFile(t_image);if(mapping)CloseHandle(mapping);if(game!=INVALID_HANDLE_VALUE)CloseHandle(game);return 1;}
}
