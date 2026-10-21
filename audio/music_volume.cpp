#define NOMINMAX
#include "music_volume.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
static_assert(sizeof(void*)==4,"Music volume hooks require x86");
namespace {
HMODULE g_image=nullptr;
bool g_installed=false;
const char* g_status="not installed";
MusicVolumeLogCallback g_log=nullptr;
SRWLOCK g_cacheLock=SRWLOCK_INIT;
struct Cached {void* manager=nullptr;void* graph=nullptr;LONG unscaled=0;unsigned long long epoch=0;bool valid=false;};
Cached g_cached{};
unsigned long long g_epoch=0;
using MasterOriginal=DWORD(__cdecl*)();
using MusicSetter=DWORD(__thiscall*)(void*,float);
using PlayOriginal=DWORD(__thiscall*)(void*);
MasterOriginal g_master=nullptr;
MusicSetter g_setter=nullptr;
PlayOriginal g_play=nullptr;
#ifdef MUSIC_VOLUME_TESTING
int g_failFlush=0,g_failProtect=0,g_flushCalls=0,g_protectCalls=0;
#endif
BOOL Protect(void* address,SIZE_T size,DWORD mode,DWORD* old){
#ifdef MUSIC_VOLUME_TESTING
    if(++g_protectCalls==g_failProtect){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
#endif
    return VirtualProtect(address,size,mode,old);
}
BOOL FlushCode(void* address,SIZE_T size){
#ifdef MUSIC_VOLUME_TESTING
    if(++g_flushCalls==g_failFlush){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
#endif
    return FlushInstructionCache(GetCurrentProcess(),address,size);
}
struct Active {void* manager=nullptr;void* graph=nullptr;unsigned master=0;float rawMusic=0;LONG savedDb=0;bool valid=false;};
Active Current(void* expected=nullptr){
    Active state{};
    __try {
        if(!g_image)return state;
        const auto* image=reinterpret_cast<const unsigned char*>(g_image);
        const auto* world=*reinterpret_cast<unsigned char* const*>(image+0x3C67DC);
        if(!world||world[0xDB])return state;
        auto* manager=*reinterpret_cast<unsigned char* const*>(world+0x34);
        if(!manager||!(manager[8]&1))return state;
        void* graph=*reinterpret_cast<void**>(manager+0x2A4);
        if(!graph||(expected&&graph!=expected))return state;
        state.manager=manager;state.graph=graph;state.master=manager[0x290];
        std::memcpy(&state.rawMusic,manager+0x294,4);std::memcpy(&state.savedDb,manager+0x29C,4);state.valid=true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return state;
}
LONG Mixed(LONG nativeDb,unsigned master){
    if(!master)return -10000;
    if(master>=250)return nativeDb; // exact original value at full Master
    if(nativeDb<=-10000)return -10000;
    const double attenuation=2000.0*std::log10(static_cast<double>(master)/250.0);
    const double result=static_cast<double>(nativeDb)+attenuation;
    if(result<=-10000.0)return -10000;
    if(result>=0.0)return 0;
    return static_cast<LONG>(std::lround(result));
}
void Invalidate(){AcquireSRWLockExclusive(&g_cacheLock);++g_epoch;g_cached.valid=false;g_cached.manager=nullptr;g_cached.graph=nullptr;ReleaseSRWLockExclusive(&g_cacheLock);}
void Remember(const Active& state,LONG unscaled){AcquireSRWLockExclusive(&g_cacheLock);g_cached.manager=state.manager;g_cached.graph=state.graph;g_cached.unscaled=unscaled;g_cached.epoch=g_epoch;g_cached.valid=true;ReleaseSRWLockExclusive(&g_cacheLock);}
bool Remembered(const Active& state,LONG* unscaled){AcquireSRWLockShared(&g_cacheLock);const bool valid=g_cached.valid&&g_cached.manager==state.manager&&g_cached.graph==state.graph&&g_cached.epoch==g_epoch;if(valid)*unscaled=g_cached.unscaled;ReleaseSRWLockShared(&g_cacheLock);return valid;}
DWORD PutVolume(void* graph,LONG db){auto table=*reinterpret_cast<void***>(graph);return reinterpret_cast<DWORD(__stdcall*)(void*,LONG)>(table[0x1C/4])(graph,db);}
DWORD __cdecl Apply(void* graph,LONG nativeDb,bool release){
    const DWORD incoming=GetLastError();
    // Invalidate even if World has already entered teardown. No cached pointer
    // survives a native restore/release boundary, including address reuse.
    if(release){Invalidate();SetLastError(incoming);return PutVolume(graph,nativeDb);}
    const Active state=Current(graph);
    const LONG db=state.valid?Mixed(nativeDb,state.master):nativeDb;
    if(state.valid)Remember(state,nativeDb);
    SetLastError(incoming);
    return PutVolume(graph,db); // real native COM HRESULT and LastError
}
#define MUSIC_THUNK(name,graphRegister,volumeRegister,releaseFlag) \
__declspec(naked) DWORD name(){__asm{push releaseFlag} __asm{push volumeRegister} __asm{push graphRegister} __asm{call Apply} __asm{add esp,12} __asm{ret}}
MUSIC_THUNK(NormalEaxEcx,eax,ecx,0)
MUSIC_THUNK(NormalEcxEdx,ecx,edx,0)
MUSIC_THUNK(NormalEaxEdx,eax,edx,0)
MUSIC_THUNK(RestoreEaxEdx,eax,edx,1)
#undef MUSIC_THUNK
__declspec(naked) DWORD MuteEax(){__asm{push 0}__asm{push -10000}__asm{push eax}__asm{call Apply}__asm{add esp,12}__asm{ret}}
__declspec(naked) DWORD MuteEcx(){__asm{push 0}__asm{push -10000}__asm{push ecx}__asm{call Apply}__asm{add esp,12}__asm{ret}}
void Refresh(){
    const Active state=Current();if(!state.valid)return;
    LONG unscaled=0,applied=0;DWORD result=0;const bool cached=Remembered(state,&unscaled);
    if(cached){applied=Mixed(unscaled,state.master);result=PutVolume(state.graph,applied);}
    else {
        // Bootstrap with the SAME native raw value, never with a product. The
        // original setter stores this value and our call hooks mix only its dB.
        // Refuse out-of-invariant raw values so bootstrap cannot clamp/change it.
        if(!std::isfinite(state.rawMusic)||state.rawMusic<0.0f||state.rawMusic>1.0f)return;
        result=g_setter(state.manager,state.rawMusic);
        if(Remembered(state,&unscaled))applied=Mixed(unscaled,state.master);
    }
    if(g_log){char line[320]{};sprintf_s(line,"MusicMaster master=%u rawMusic=%.6f unscaledDb=%ld appliedDb=%ld result=0x%08lX cached=%u",state.master,static_cast<double>(state.rawMusic),unscaled,applied,result,cached?1u:0u);g_log(line);}
}
DWORD __cdecl MasterChanged(){const DWORD incoming=GetLastError();SetLastError(incoming);const DWORD result=g_master();const DWORD outgoing=GetLastError();Refresh();SetLastError(outgoing);return result;}
void __cdecl PrepareRun(void* manager){
    const DWORD incoming=GetLastError();
    const Active state=Current();
    if(state.valid&&state.manager==manager){
        LONG unscaled=0;bool known=Remembered(state,&unscaled);
        if(!known&&state.savedDb>=-10000&&state.savedDb<=0){unscaled=state.savedDb;Remember(state,unscaled);known=true;}
        // The immediate/nonfade native path has no volume setter between graph
        // creation and Run. Seed from its actual get_Volume result (+29C).
        // At Master250 leave that graph's current level completely untouched.
        if(state.master<250&&(known||state.master==0))PutVolume(state.graph,state.master==0?-10000:Mixed(unscaled,state.master));
        if(g_log){char line[256]{};sprintf_s(line,"MusicPreRun master=%u rawMusic=%.6f nativeDb=%ld seedKnown=%u",state.master,static_cast<double>(state.rawMusic),unscaled,known?1u:0u);g_log(line);}
    }
    SetLastError(incoming);
}
__declspec(naked) void BeforePlay(){
    __asm{
        pushfd
        pushad
        push dword ptr[esp+24]
        call PrepareRun
        add esp,4
        popad
        popfd
        jmp dword ptr[g_play]
    }
}
struct Patch {DWORD rva;size_t length;unsigned char original[9];const void* target;};
const Patch Patches[]={
    {0x3715,5,{0x51,0x50,0xff,0x52,0x1c},reinterpret_cast<const void*>(&NormalEaxEcx)},
    {0x37bd,5,{0x51,0x50,0xff,0x52,0x1c},reinterpret_cast<const void*>(&NormalEaxEcx)},
    {0x3aaf,5,{0x52,0x51,0xff,0x50,0x1c},reinterpret_cast<const void*>(&NormalEcxEdx)},
    {0x3bc3,5,{0x52,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&NormalEaxEdx)},
    {0x3707,9,{0x68,0xf0,0xd8,0xff,0xff,0x50,0xff,0x52,0x1c},reinterpret_cast<const void*>(&MuteEax)},
    {0x37a7,9,{0x68,0xf0,0xd8,0xff,0xff,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&MuteEax)},
    {0x3a95,9,{0x68,0xf0,0xd8,0xff,0xff,0x51,0xff,0x52,0x1c},reinterpret_cast<const void*>(&MuteEcx)},
    {0x1d58,5,{0x52,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&RestoreEaxEdx)},
    {0x332e,5,{0x52,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&RestoreEaxEdx)},
    {0x348e,5,{0x52,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&RestoreEaxEdx)},
    {0x3c37,5,{0x52,0x50,0xff,0x51,0x1c},reinterpret_cast<const void*>(&RestoreEaxEdx)},
    {0x1a1c9e,5,{0xe8,0x6d,0x01,0x00,0x00},reinterpret_cast<const void*>(&MasterChanged)},
    {0x10fe8,5,{0xe8,0xd3,0x24,0xff,0xff},reinterpret_cast<const void*>(&BeforePlay)}
};
void Replacement(unsigned char* site,const Patch& patch,unsigned char (&bytes)[9]){std::memset(bytes,0x90,sizeof(bytes));bytes[0]=0xE8;const DWORD delta=static_cast<DWORD>(reinterpret_cast<uintptr_t>(patch.target))-static_cast<DWORD>(reinterpret_cast<uintptr_t>(site)+5);std::memcpy(bytes+1,&delta,4);}
bool Range(const IMAGE_NT_HEADERS32* nt,DWORD rva,size_t length,bool executable){const IMAGE_SECTION_HEADER* s=IMAGE_FIRST_SECTION(nt);for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const uint64_t start=s[i].VirtualAddress,end=start+s[i].Misc.VirtualSize;if(rva>=start&&static_cast<uint64_t>(rva)+length<=end&&(!executable||(s[i].Characteristics&IMAGE_SCN_MEM_EXECUTE)))return true;}return false;}
bool Guard(HMODULE image){
    __try {
        if(!image)return false;const auto* b=reinterpret_cast<const unsigned char*>(image);const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<sizeof(*dos)||dos->e_lfanew>0x100000)return false;
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(b+dos->e_lfanew);
        if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||!nt->FileHeader.NumberOfSections||nt->FileHeader.NumberOfSections>96||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.ImageBase!=0x400000||nt->OptionalHeader.SizeOfImage<0x3C6800)return false;
        for(const Patch& p:Patches)if(!Range(nt,p.rva,p.length,true)||std::memcmp(b+p.rva,p.original,p.length))return false;
        struct Signature{DWORD rva;const char* bytes;size_t size;bool executable;};
        const Signature signatures[]={
        {0x3a10,"\x55\x8b\xec\x51\xd9\x45\x08\x8b\x45\x08\xd8\x1d\x68\x64\x74\x00\x89\x81\x94\x02\x00\x00\xdf\xe0\xf6\xc4\x01\x74\x0c\xc7\x81\x94\x02\x00\x00\x00\x00\x00\x00\xeb\x1a\xd9\x45\x08\xd8\x1d\x70\x64\x74\x00\xdf\xe0\xf6\xc4\x41\x75\x0a\xc7\x81\x94\x02\x00\x00\x00\x00\x80\x3f\xd9\x81\x94\x02\x00\x00\xd8\x0d\xd8\x64\x74\x00\xd8\x05\xd4\x64\x74\x00\xd8\x0d\xd0\x64\x74\x00\xd9\x5d\x08\xd9\x45\x08\xdb\x5d\xfc\xf6\x41\x08\x01\x74\x3a\xd9\x81\x94\x02\x00\x00\x8b\x89\xa4\x02\x00\x00\xd8\x1d\xc8\x64\x74\x00\xdf\xe0\xf6\xc4\x01\x74\x11\x8b\x11\x68\xf0\xd8\xff\xff\x51\xff\x52\x1c\x8b\xe5\x5d\xc2\x04\x00\x8b\x55\xfc\x8b\x01\x81\xc2\xf0\xd8\xff\xff\x52\x51\xff\x50\x1c\x8b\xe5\x5d\xc2\x04\x00",170,true},
        {0x3650,"\x55\x8b\xec\x83\xec\x1c\x56\x8b\xf1\x8a\x46\x7c\x84\xc0\x57\x8d",16,true},
        {0x3b20,"\x55\x8b\xec\x83\xec\x14\xa1\xdc\x67\x7c\x00\x8b\x40\x20\x53\x56\x8b\xf1",18,true},
        {0x1a1c8f,"\x6a\x01\x8b\xce\x88\x82\x90\x02\x00\x00\xe8\xd2\x1e\xfe\xff\xe8\x6d\x01\x00\x00",20,true},
        {0x1a1e10,"\xa1\xe0\x67\x7c\x00\x85\xc0\x0f\x84\x23\x02\x00\x00\x8b\x88\x34\xb5\x00\x00\x56\x57",21,true},
        {0x33e9,"\x8b\x36\x8b\x06\x8d\xbb\xa4\x02\x00\x00\x57\x68\xf0\xb8\x74\x00\x56\xff\x10\x8b\x3f\x8b\x0f\x8d\x93\x9c\x02\x00\x00\x52\x57\xff\x51\x20",34,true},
        {0x34b8f0,"\xb3\x68\xa8\x56\xd4\x0a\xce\x11\xb0\x3a\x00\x20\xaf\x0b\xa7\x70",16,false},
        {0x10fa0,"\x56\x8b\xf1\x8b\x4e\x34\x85\xc9\x74\x4d\x8a\x44\x24\x0c\x84\xc0\x74\x29\xe8\x19\x18\xff\xff\x8b\x44\x24\x08\x51\x8b\x4e\x34\xd9\x1c\x24\x68\xe8\x03\x00\x00\x50\xe8\xf3\x2a\xff\xff\xc7\x86\x50\x03\x00\x00\x02\x00\x00\x00\x5e\xc2\x08\x00\x8b\x54\x24\x08\x52\xe8\x8b\x22\xff\xff\x8b\x4e\x34\xe8\xd3\x24\xff\xff\xc7\x86\x50\x03\x00\x00\x02\x00\x00\x00\x5e\xc2\x08\x00",91,true},
        {0x34c0,"\x83\xec\x08\x56\x8b\xf1\xf6\x46\x08\x01\x0f\x84\x99\x00\x00\x00\x8b\x86\xa0\x02\x00\x00\x8b\x08\x8d\x54\x24\x04\x52\x68\x20\xb9\x74\x00\x50\xff\x11\x85\xc0\x7c\x73\x8b\x86\xa0\x02\x00\x00\x8b\x08\x8d\x54\x24\x08\x52\x68\x00\xb9\x74\x00\x50\xff\x11\x85\xc0\x7c\x0e\x8b\x44\x24\x08\x8b\x08\x6a\x00\x6a\x00\x50\xff\x51\x20\x8b\x44\x24\x08\x8b\x10\x57\x50\xff\x52\x08\x8b\x44\x24\x08\x8b\x08\x50\xff\x51\x1c\x8b\xf8\xf6\x46\x08\x04\x74\x0a\x8b\x44\x24\x08\x8b\x10\x50\xff\x52\x20\x8b\x44\x24\x08\x8b\x08\x50\xff\x51\x08\x85\xff\x5f\x7c\x04\x83\x4e\x08\x02\x68\xa0\x4d\x77\x00\xe8\xac\x64\x2e\x00\x83\xc4\x04\x5e\x83\xc4\x08\xc3\x68\x80\x4d\x77\x00\xe8\xba\x38\x07\x00\x83\xc4\x04\x5e\x83\xc4\x08\xc3",174,true}
        };
        for(const auto& s:signatures)if(!Range(nt,s.rva,s.size,s.executable)||std::memcmp(b+s.rva,s.bytes,s.size))return false;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
} // namespace
bool InstallMusicVolumeHooks(HMODULE image){
    if(g_installed){if(image!=g_image){g_status="already installed for another image";return false;}auto* b=reinterpret_cast<unsigned char*>(image);for(const Patch& p:Patches){unsigned char bytes[9]{};Replacement(b+p.rva,p,bytes);if(std::memcmp(b+p.rva,bytes,p.length)){g_status="installed music calls changed";return false;}}g_status="already installed";return true;}
    if(!Guard(image)){g_status="unsupported original music paths, lifetime boundaries or callback";return false;}
    auto* b=reinterpret_cast<unsigned char*>(image);SYSTEM_INFO system{};GetSystemInfo(&system);
    struct Page{unsigned char* address;DWORD old;};Page pages[16]{};size_t count=0;
    for(const Patch& p:Patches){const auto first=reinterpret_cast<uintptr_t>(b+p.rva)&~(static_cast<uintptr_t>(system.dwPageSize)-1);const auto last=reinterpret_cast<uintptr_t>(b+p.rva+p.length-1)&~(static_cast<uintptr_t>(system.dwPageSize)-1);if(first!=last){g_status="music patch crosses page";return false;}bool found=false;for(size_t i=0;i<count;++i)found=found||pages[i].address==reinterpret_cast<unsigned char*>(first);if(!found)pages[count++].address=reinterpret_cast<unsigned char*>(first);}
    size_t writable=0;for(;writable<count;++writable)if(!Protect(pages[writable].address,system.dwPageSize,PAGE_EXECUTE_READWRITE,&pages[writable].old))break;
    if(writable!=count){bool restored=true;for(size_t i=0;i<writable;++i){DWORD ignored=0;restored=(Protect(pages[i].address,system.dwPageSize,pages[i].old,&ignored)!=FALSE)&&restored;}g_status=restored?"cannot protect music pages; no calls changed":"cannot protect music pages; protection restore failed";return false;}
    g_master=reinterpret_cast<MasterOriginal>(b+0x1A1E10);g_setter=reinterpret_cast<MusicSetter>(b+0x3A10);g_play=reinterpret_cast<PlayOriginal>(b+0x34C0);
    for(const Patch& p:Patches){unsigned char bytes[9]{};Replacement(b+p.rva,p,bytes);std::memcpy(b+p.rva,bytes,p.length);}
    bool flushed=true,restored=true;for(size_t i=0;i<count;++i)flushed=(FlushCode(pages[i].address,system.dwPageSize)!=FALSE)&&flushed;for(size_t i=0;i<count;++i){DWORD ignored=0;restored=(Protect(pages[i].address,system.dwPageSize,pages[i].old,&ignored)!=FALSE)&&restored;}
    if(!flushed||!restored){bool rollback=true;for(size_t i=0;i<count;++i){DWORD ignored=0;if(!Protect(pages[i].address,system.dwPageSize,PAGE_EXECUTE_READWRITE,&ignored))rollback=false;else for(const Patch& p:Patches)if(b+p.rva>=pages[i].address&&b+p.rva<pages[i].address+system.dwPageSize)std::memcpy(b+p.rva,p.original,p.length);}for(size_t i=0;i<count;++i)rollback=(FlushCode(pages[i].address,system.dwPageSize)!=FALSE)&&rollback;for(size_t i=0;i<count;++i){DWORD ignored=0;rollback=(Protect(pages[i].address,system.dwPageSize,pages[i].old,&ignored)!=FALSE)&&rollback;}g_status=rollback?"music installation failed; all original calls restored":"music installation failed; rollback failed";return false;}
    g_image=image;g_installed=true;g_status="installed; music follows Master without changing raw Music setting";return true;
}
const char* MusicVolumeHookStatus(){return g_status;}
void SetMusicVolumeLogCallback(MusicVolumeLogCallback callback){g_log=callback;}
#ifdef MUSIC_VOLUME_TESTING
void SetMusicVolumeTestFailures(int flushCall,int protectionCall){g_failFlush=flushCall;g_failProtect=protectionCall;g_flushCalls=0;g_protectCalls=0;}
#endif
