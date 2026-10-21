// Private original-EXE keyboard ABI fixture. It never acquires a real device.
#include "display.cpp"
#include <array>
#include <vector>

namespace {
unsigned checks=0,failures=0,protectCalls=0,failProtectAt=0,cooperativeCalls=0;
unsigned flushCalls=0,failFlushMask=0;
uintptr_t beforeStack=0,afterStack=0,result=0,seenBx=0,seenSi=0,seenDi=0,seenBp=0;
void* receivedDevice=nullptr;HWND receivedWindow=nullptr;DWORD receivedFlags=0;
HRESULT nativeResult=S_OK;
constexpr uintptr_t WindowToken=0x12345678;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",text);}}
bool Write(void* destination,const void* source,size_t count){
    DWORD old=0,ignored=0;if(!VirtualProtect(destination,count,PAGE_EXECUTE_READWRITE,&old))return false;
    std::memcpy(destination,source,count);FlushInstructionCache(GetCurrentProcess(),destination,count);
    return VirtualProtect(destination,count,old,&ignored)!=FALSE;
}
BOOL WINAPI Protect(void* address,SIZE_T size,DWORD protection,DWORD* old){
    ++protectCalls;if(protectCalls==failProtectAt){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return VirtualProtect(address,size,protection,old);
}
BOOL WINAPI Flush(HANDLE process,const void* address,SIZE_T size){
    ++flushCalls;if(failFlushMask&(1u<<flushCalls)){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return FlushInstructionCache(process,address,size);
}
HWND WINAPI ActiveWindow(){return reinterpret_cast<HWND>(WindowToken);}
HRESULT WINAPI Cooperative(void* device,HWND window,DWORD flags){
    ++cooperativeCalls;receivedDevice=device;receivedWindow=window;receivedFlags=flags;return nativeResult;
}
__declspec(naked) void ReturnFromBlock(){__asm ret}
// The original block sets EBP to the device vtable; ESI is the native input
// object. Observe exact COM stdcall arguments, RET12 and result propagation.
__declspec(naked) void __stdcall Invoke(void*,void*){
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax,dword ptr[esp+20]
        mov esi,dword ptr[esp+24]
        mov ebx,0x11223344
        mov edi,0x33445566
        mov ebp,0x44556677
        mov beforeStack,esp
        call eax
        mov result,eax
        mov afterStack,esp
        mov seenBx,ebx
        mov seenSi,esi
        mov seenDi,edi
        mov seenBp,ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 8
    }
}
bool PrepareBlock(unsigned char* image){
    const auto active=&ActiveWindow;const DWORD slot=DWORD(reinterpret_cast<uintptr_t>(image+0x346374));
    auto* exit=image+0x62A1;unsigned char jump[5]={0xE9};
    const DWORD delta=DWORD(reinterpret_cast<uintptr_t>(&ReturnFromBlock)-reinterpret_cast<uintptr_t>(exit+5));
    std::memcpy(jump+1,&delta,4);
    // Rebase only the two absolute GetActiveWindow IAT operands. All flag
    // pushes, both branches and the COM call are original executable bytes.
    return Write(image+0x346374,&active,4)&&Write(image+0x6281,&slot,4)&&Write(image+0x6295,&slot,4)&&Write(exit,jump,5);
}
void RunBlock(unsigned char* image,DWORD entry,DWORD expected){
    std::array<void*,14> table{};table[13]=reinterpret_cast<void*>(&Cooperative);
    struct Device{void** table;} device{table.data()};
    std::array<unsigned char,0x2C> state{};auto* ptr=&device;std::memcpy(state.data()+0x28,&ptr,4);
    for(HRESULT returned:{S_OK,E_FAIL}){
        nativeResult=returned;const unsigned prior=cooperativeCalls;SetLastError(0x24681357);
        Invoke(image+entry-0x400000,state.data());
        Check(cooperativeCalls==prior+1,"exactly one native SetCooperativeLevel call");
        Check(receivedDevice==&device&&receivedWindow==reinterpret_cast<HWND>(WindowToken)&&receivedFlags==expected,"original COM arguments match selected keyboard flags");
        Check(beforeStack==afterStack,"original COM stdcall RET12 balances stack");
        Check(seenBx==0x11223344&&seenDi==0x33445566&&seenSi==reinterpret_cast<uintptr_t>(state.data())&&seenBp==reinterpret_cast<uintptr_t>(table.data()),"original block register effects preserved");
        Check(DWORD(result)==DWORD(returned),"native HRESULT success/failure preserved");
        Check(GetLastError()==0x24681357,"native block preserves LastError");
    }
}
}

int main(int argc,char** argv){
    if(argc!=2)return 2;
    HANDLE file=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE mapping=file==INVALID_HANDLE_VALUE?nullptr:CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    auto* original=mapping?static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    auto* patched=mapping?static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    if(!original||!patched)return 3;
    const auto module=reinterpret_cast<HMODULE>(patched);
    std::array<unsigned char,41> native{};std::memcpy(native.data(),original+0x6278,native.size());
    Check(!PatchKeyboardCooperativeLevel(nullptr),"null image rejected");
    for(size_t i=0;i<native.size();++i){
        auto corrupted=native;corrupted[i]^=0x40;
        Check(Write(patched+0x6278,corrupted.data(),corrupted.size()),"private corruption fixture writable");
        Check(!PatchKeyboardCooperativeLevel(module)&&!std::memcmp(patched+0x6278,corrupted.data(),corrupted.size()),"each signature/flags corruption rejected without partial patch");
        Check(Write(patched+0x6278,native.data(),native.size()),"private original signature restored");
    }
    MEMORY_BASIC_INFORMATION priorProtection{};VirtualQuery(patched+0x6278,&priorProtection,sizeof(priorProtection));
    g_keyboardProtect=&Protect;g_keyboardFlush=&Flush;
    for(unsigned failure:{1u,2u}){
        protectCalls=0;flushCalls=0;failProtectAt=failure;
        Check(!PatchKeyboardCooperativeLevel(module),"VirtualProtect failure rejects installation");
        Check(!std::memcmp(patched+0x6278,native.data(),native.size()),"API failure leaves original keyboard flags");
        Check(protectCalls==(failure==1?1u:3u),"restore failure invokes protection rollback");
        MEMORY_BASIC_INFORMATION actual{};VirtualQuery(patched+0x6278,&actual,sizeof(actual));
        Check(actual.Protect==priorProtection.Protect,"protect API failure restores original page protection");
        Check(flushCalls==(failure==1?0u:2u),"protection failure performs only necessary cache flushes");
    }
    for(unsigned scenario:{0u,1u,2u}){
        protectCalls=0;flushCalls=0;failProtectAt=scenario==2?2u:0u;
        failFlushMask=scenario==1?6u:2u; // initial flush; optionally rollback flush too.
        Check(!PatchKeyboardCooperativeLevel(module),"initial instruction-cache flush failure rejects installation");
        Check(!std::memcmp(patched+0x6278,native.data(),native.size()),"flush failure rolls back both flag bytes");
        Check(protectCalls==2&&flushCalls==2,"flush failure attempts rollback flush and protection restore");
        MEMORY_BASIC_INFORMATION actual{};VirtualQuery(patched+0x6278,&actual,sizeof(actual));
        if(scenario==2){
            Check(actual.Protect==PAGE_EXECUTE_READWRITE,"failed rollback protection remains observable");
            DWORD ignored=0;Check(VirtualProtect(patched+0x6278,native.size(),priorProtection.Protect,&ignored)!=FALSE,"private failed-rollback page protection cleaned up");
        }else Check(actual.Protect==priorProtection.Protect,"flush failure restores original page protection");
        Check(std::strstr(g_status,scenario?"rollback failed":"flags rolled back")!=nullptr,"partial rollback failure has a distinct diagnostic status");
    }
    g_keyboardProtect=&VirtualProtect;g_keyboardFlush=&FlushInstructionCache;
    Check(PatchKeyboardCooperativeLevel(module),"keyboard patch installs");
    Check(PatchKeyboardCooperativeLevel(module),"keyboard patch is idempotent");
    unsigned differences=0;
    for(size_t i=0x61C0;i<0x63B0;++i)if(original[i]!=patched[i]){++differences;Check(i==0x627E||i==0x6292,"only keyboard flag immediate changes");}
    Check(differences==2&&patched[0x627E]==0x0A&&patched[0x6292]==0x06,"only two keyboard bytes patched, scope preference preserved");
    Check(!std::memcmp(original+0x6330,patched+0x6330,0x3B),"mouse cooperative level and data format unchanged");
    Check(PrepareBlock(original)&&PrepareBlock(patched),"private original COM-call fixture prepared");
    RunBlock(original,0x406278,0x19);RunBlock(original,0x40628C,0x15);
    RunBlock(patched,0x406278,0x0A);RunBlock(patched,0x40628C,0x06);
    UnmapViewOfFile(original);UnmapViewOfFile(patched);CloseHandle(mapping);CloseHandle(file);
    std::printf("%u checks, %u failures; keyboard original19/15 -> nonexclusive0A/06, native ABI/guards/rollback\n",checks,failures);
    return failures?1:0;
}
