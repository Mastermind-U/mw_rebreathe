#define NOMINMAX
#include "audio_trace.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

static_assert(sizeof(void*)==4,"Audio trace requires x86");
namespace {
constexpr DWORD TargetRva=0x29F0;
constexpr size_t Capacity=64*1024;
SRWLOCK g_lock=SRWLOCK_INIT;
HANDLE g_file=INVALID_HANDLE_VALUE;
HMODULE g_image=nullptr;
bool g_installed=false;
const char* g_status="not installed";
char g_pending[Capacity]{};
size_t g_length=0;
unsigned long long g_rows=0,g_dropped=0;
LARGE_INTEGER g_frequency{};
using Original=DWORD(__thiscall*)(void*,void*,DWORD);
Original g_original=nullptr;
#ifdef AUDIO_TRACE_TESTING
int g_failFlush=0,g_failProtect=0,g_flushCalls=0,g_protectCalls=0;
#endif
BOOL Protect(void* address,SIZE_T length,DWORD mode,DWORD* previous){
#ifdef AUDIO_TRACE_TESTING
    if(++g_protectCalls==g_failProtect){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
#endif
    return VirtualProtect(address,length,mode,previous);
}
BOOL FlushCode(void* address,SIZE_T length){
#ifdef AUDIO_TRACE_TESTING
    if(++g_flushCalls==g_failFlush){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
#endif
    return FlushInstructionCache(GetCurrentProcess(),address,length);
}
struct State { DWORD master=0,detail=0,ds=0,cached=0; bool managerValid=false,soundValid=false; };
bool RecordingAllowed(){
    __try {
        if(!g_image)return false;
        const auto* image=reinterpret_cast<const unsigned char*>(g_image);
        const auto* world=*reinterpret_cast<unsigned char* const*>(image+0x3C67DC);
        return world&&world[0xDB]==0;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
State Capture(void* manager,void* sound){
    State state{};
    __try {
        if(manager){const auto* m=static_cast<const unsigned char*>(manager);state.master=m[0x290];state.detail=m[0x29A];state.managerValid=true;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        if(sound){const auto* s=static_cast<const unsigned char*>(sound);std::memcpy(&state.ds,s,4);state.cached=s[0x4C];state.soundValid=true;}
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return state;
}
void FlushLocked(){
    size_t consumed=0;
    while(consumed<g_length){DWORD written=0;if(!WriteFile(g_file,g_pending+consumed,static_cast<DWORD>(g_length-consumed),&written,nullptr)||!written)break;consumed+=written;}
    if(consumed){g_length-=consumed;if(g_length)std::memmove(g_pending,g_pending+consumed,g_length);}
}
void Record(DWORD site,void* manager,void* sound,DWORD requested,DWORD raw,LARGE_INTEGER start,LARGE_INTEGER end,const State& before,const State& after){
    if(g_file==INVALID_HANDLE_VALUE)return;
    AcquireSRWLockExclusive(&g_lock);
    char row[640]{};
    const int length=sprintf_s(row,"%llu,0x%08lX,%lld,%lld,%lld,%.6f,%lu,0x%08lX,0x%08lX,%lu,%lu,%u,%lu,%lu,%u,%u,0x%08lX,0x%08lX,%lu,%lu,0x%08lX,%llu\r\n",
        ++g_rows,static_cast<unsigned long>(site),start.QuadPart,end.QuadPart,g_frequency.QuadPart,
        (end.QuadPart-start.QuadPart)*1000.0/g_frequency.QuadPart,GetCurrentThreadId(),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(manager)),static_cast<unsigned long>(reinterpret_cast<uintptr_t>(sound)),
        requested,requested&0xFF,before.managerValid?1u:0u,before.master,before.detail,
        before.soundValid?1u:0u,after.soundValid?1u:0u,before.ds,after.ds,before.cached,after.cached,raw,g_dropped);
    if(length>0){
        if(g_length+static_cast<size_t>(length)>Capacity)FlushLocked();
        if(g_length+static_cast<size_t>(length)<=Capacity){std::memcpy(g_pending+g_length,row,static_cast<size_t>(length));g_length+=static_cast<size_t>(length);}else ++g_dropped;
    }
    // Every observed setter is a diagnostic boundary. No atexit/IAT exit hook
    // is needed to make its row visible; WriteFile reaches the OS file cache.
    FlushLocked();
    ReleaseSRWLockExclusive(&g_lock);
}
// Native 0x4029F0 has a null-sound early return that leaves EAX untouched.
// Restore the exact incoming EAX as well as ECX/stack arguments before calling.
__declspec(naked) DWORD __stdcall CallOriginal(void*,void*,DWORD,DWORD){
    __asm {
        mov ecx,dword ptr[esp+4]
        mov eax,dword ptr[esp+16]
        push dword ptr[esp+12]
        push dword ptr[esp+12]
        call dword ptr[g_original]
        ret 16
    }
}
DWORD __cdecl Invoke(DWORD site,void* manager,void* sound,DWORD requested,DWORD incomingEax){
    const DWORD incomingError=GetLastError();
    if(!RecordingAllowed()){SetLastError(incomingError);return CallOriginal(manager,sound,requested,incomingEax);}
    const State before=Capture(manager,sound);
    LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
    SetLastError(incomingError);
    const DWORD raw=CallOriginal(manager,sound,requested,incomingEax);
    const DWORD outgoingError=GetLastError();QueryPerformanceCounter(&end);
    const State after=Capture(manager,sound);
    Record(site,manager,sound,requested,raw,start,end,before,after);
    SetLastError(outgoingError);
    return raw;
}
// These thunks have the original thiscall RET8 shape. The C helper's return is
// the unmodified native EAX. EDX is volatile and is not an original argument.
__declspec(naked) DWORD TraceReuse(){
    __asm {
        push eax
        push dword ptr[esp+12]
        push dword ptr[esp+12]
        push ecx
        push 0x48BE18
        call Invoke
        add esp,20
        ret 8
    }
}
__declspec(naked) DWORD TraceNew(){
    __asm {
        push eax
        push dword ptr[esp+12]
        push dword ptr[esp+12]
        push ecx
        push 0x510B33
        call Invoke
        add esp,20
        ret 8
    }
}
struct Patch { DWORD rva; unsigned char original[5]; const void* target; };
const Patch Patches[]={
    {0x8BE18,{0xE8,0xD3,0x6B,0xF7,0xFF},reinterpret_cast<const void*>(&TraceReuse)},
    {0x110B33,{0xE8,0xB8,0x1E,0xEF,0xFF},reinterpret_cast<const void*>(&TraceNew)},
};
constexpr unsigned char TargetBytes[]={0x56,0x8b,0x74,0x24,0x08,0x85,0xf6,0x74,0x60,0x53,0x8b,0x5c,0x24,0x10,0x84,0xdb,0x75,0x07,0xb9,0xf0,0xd8,0xff,0xff,0xeb,0x21,0x8b,0xc3,0x25,0xff,0x00,0x00,0x00,0x89,0x44,0x24,0x0c,0xdb,0x44,0x24,0x0c,0xd8,0x0d,0x80,0x64,0x74,0x00,0xe8,0x7b,0x4c,0x32,0x00,0xb9,0xfb,0xf2,0xff,0xff,0x2b,0xc8,0x88,0x5e,0x4c,0x80,0xfb,0xfd,0x5b,0x73,0x03,0x2b,0x4e,0x48,0x33,0xc0,0x85,0xc9,0x0f,0x9d,0xc0,0x48,0x23,0xc1,0x3d,0xf0,0xd8,0xff,0xff,0x7f,0x05,0xb8,0xf0,0xd8,0xff,0xff,0x8b,0x0e,0x85,0xc9,0x74,0x07,0x8b,0x11,0x50,0x51,0xff,0x52,0x3c,0x5e,0xc2,0x08,0x00};
bool Executable(const IMAGE_NT_HEADERS32* nt,DWORD rva,size_t length){
    const IMAGE_SECTION_HEADER* section=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const uint64_t start=section[i].VirtualAddress,end=start+section[i].Misc.VirtualSize;if(rva>=start&&static_cast<uint64_t>(rva)+length<=end&&(section[i].Characteristics&IMAGE_SCN_MEM_EXECUTE))return true;}
    return false;
}
bool Guard(HMODULE image){
    __try {
        if(!image)return false;
        const auto* b=reinterpret_cast<const unsigned char*>(image);const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<sizeof(*dos)||dos->e_lfanew>0x100000)return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(b+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||!nt->FileHeader.NumberOfSections||nt->FileHeader.NumberOfSections>96||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.ImageBase!=0x400000||nt->OptionalHeader.SizeOfImage<0x3C6800)return false;
        for(const Patch& p:Patches)if(!Executable(nt,p.rva-4,13)||std::memcmp(b+p.rva,p.original,5))return false;
        return Executable(nt,TargetRva,sizeof(TargetBytes))&&!std::memcmp(b+TargetRva,TargetBytes,sizeof(TargetBytes))&&
            !std::memcmp(b+0x8BE14,"\x8B\x48\x34\x52",4)&&!std::memcmp(b+0x8BE1D,"\x8B\x4E\x04",3)&&
            !std::memcmp(b+0x110B30,"\x8B\x4A\x34",3)&&!std::memcmp(b+0x110B38,"\x8B\x44\x24\x10",4);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Replacement(unsigned char* site,const void* target,unsigned char (&bytes)[5]){bytes[0]=0xE8;const DWORD delta=static_cast<DWORD>(reinterpret_cast<uintptr_t>(target))-static_cast<DWORD>(reinterpret_cast<uintptr_t>(site)+5);std::memcpy(bytes+1,&delta,4);}
bool OpenTrace(){
    wchar_t path[32768]{};const DWORD length=GetEnvironmentVariableW(L"MW_MODERN_LOG",path,32768);if(!length||length>=32768)return false;
    wchar_t* name=nullptr;for(wchar_t* p=path;*p;++p)if(*p==L'\\'||*p==L'/')name=p+1;
    if(!name||!*name)return false;
    if(wcscpy_s(name,32768-static_cast<size_t>(name-path),L"audio-volume.csv"))return false;
    g_file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(g_file==INVALID_HANDLE_VALUE)return false;
    constexpr char header[]="call,caller,qpc_start,qpc_end,qpc_frequency,elapsed_ms,thread_id,manager,sound_data,requested_raw,requested_byte,manager_state_valid,master,detail,sound_state_before_valid,sound_state_after_valid,ds_buffer_before,ds_buffer_after,cached_byte_before,cached_byte_after,original_raw_eax,dropped_rows\r\n";
    DWORD written=0;if(!WriteFile(g_file,header,static_cast<DWORD>(sizeof(header)-1),&written,nullptr)||written!=sizeof(header)-1){CloseHandle(g_file);g_file=INVALID_HANDLE_VALUE;return false;}return true;
}
} // namespace
bool InstallAudioTrace(HMODULE image){
    if(g_installed){
        if(image!=g_image){g_status="already installed for another image";return false;}
        auto* b=reinterpret_cast<unsigned char*>(image);for(const Patch& p:Patches){unsigned char expected[5]{};Replacement(b+p.rva,p.target,expected);if(std::memcmp(b+p.rva,expected,5)){g_status="installed calls changed";return false;}}
        g_status="already installed";return true;
    }
    wchar_t enabled[4]{};if(GetEnvironmentVariableW(L"MW_MODERN_TRACE",enabled,4)!=1||enabled[0]!=L'1'){g_status="disabled (MW_MODERN_TRACE is not 1)";return false;}
    if(!Guard(image)){g_status="unsupported image, calls or complete native setter";return false;}
    if(!QueryPerformanceFrequency(&g_frequency)||g_frequency.QuadPart<=0){g_status="performance counter unavailable";return false;}
    if(!OpenTrace()){g_status="cannot open audio-volume.csv";return false;}
    auto* b=reinterpret_cast<unsigned char*>(image);DWORD old[2]{};size_t protectedCount=0;
    for(;protectedCount<2;++protectedCount)if(!Protect(b+Patches[protectedCount].rva,5,PAGE_EXECUTE_READWRITE,&old[protectedCount]))break;
    if(protectedCount!=2){bool restored=true;for(size_t i=0;i<protectedCount;++i){DWORD ignored=0;restored=(Protect(b+Patches[i].rva,5,old[i],&ignored)!=FALSE)&&restored;}CloseHandle(g_file);g_file=INVALID_HANDLE_VALUE;g_status=restored?"cannot protect calls; original calls unchanged":"cannot protect calls; page restore failed";return false;}
    g_original=reinterpret_cast<Original>(b+TargetRva);
    for(const Patch& p:Patches){unsigned char bytes[5]{};Replacement(b+p.rva,p.target,bytes);std::memcpy(b+p.rva,bytes,5);}
    bool flushed=true,restored=true;
    for(const Patch& p:Patches)flushed=(FlushCode(b+p.rva,5)!=FALSE)&&flushed;
    for(size_t i=0;i<2;++i){DWORD ignored=0;restored=(Protect(b+Patches[i].rva,5,old[i],&ignored)!=FALSE)&&restored;}
    if(!flushed||!restored){
        bool rollback=true;
        for(size_t i=0;i<2;++i){DWORD ignored=0;if(Protect(b+Patches[i].rva,5,PAGE_EXECUTE_READWRITE,&ignored)){std::memcpy(b+Patches[i].rva,Patches[i].original,5);}else rollback=false;}
        for(const Patch& p:Patches)rollback=(FlushCode(b+p.rva,5)!=FALSE)&&rollback;
        for(size_t i=0;i<2;++i){DWORD ignored=0;rollback=(Protect(b+Patches[i].rva,5,old[i],&ignored)!=FALSE)&&rollback;}
        CloseHandle(g_file);g_file=INVALID_HANDLE_VALUE;g_status=rollback?"installation failed; original calls restored":"installation failed; rollback failed";return false;
    }
    g_image=image;g_installed=true;g_status="installed; two setters measured, no volume changes";return true;
}
void FlushAudioTrace(){const DWORD error=GetLastError();AcquireSRWLockExclusive(&g_lock);if(g_file!=INVALID_HANDLE_VALUE)FlushLocked();ReleaseSRWLockExclusive(&g_lock);SetLastError(error);}
const char* AudioTraceStatus(){return g_status;}
#ifdef AUDIO_TRACE_TESTING
void SetAudioTraceTestFailures(int flushCall,int protectionCall){g_failFlush=flushCall;g_failProtect=protectionCall;g_flushCalls=0;g_protectCalls=0;}
#endif
