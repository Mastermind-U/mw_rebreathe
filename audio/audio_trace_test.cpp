// Isolated x86 fixture: real original footstep arithmetic, native SetVolume,
// per-sound factor and both actual patched caller fragments. No game is run.
#define NOMINMAX
#include "audio_trace.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
static_assert(sizeof(void*)==4,"x86 fixture required");
namespace {
unsigned char* t_image=nullptr;
std::array<unsigned char,0x200> t_world{};
std::array<unsigned char,0x300> t_manager{};
std::array<void*,18> t_bufferTable{};
struct Buffer { void** table=nullptr; } t_buffer;
std::array<unsigned char,0x60> t_sound{},t_record{};
unsigned t_setCalls=0,t_abiCases=0,t_nativeCases=0,t_guards=0;
LONG t_decibels=0;
void* t_receiver=nullptr;
HRESULT t_hresult=S_OK;
uintptr_t t_before=0,t_after=0;
DWORD t_result=0,t_bx=0,t_si=0,t_di=0,t_bp=0;
DWORD t_observerEax=0,t_observerEcx=0,t_observerSound=0,t_observerVolume=0,t_observerError=0;
constexpr DWORD Incoming=0x12345678,Outgoing=0x87654321,EntryEax=0x5A5AA5A5;
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T>T Get(const void* p,size_t o){T value{};std::memcpy(&value,static_cast<const unsigned char*>(p)+o,sizeof(value));return value;}
template<class T>void Put(void* p,size_t o,T value){std::memcpy(static_cast<unsigned char*>(p)+o,&value,sizeof(value));}
void Write(void* destination,const void* source,size_t length){DWORD old=0,ignored=0;Require(VirtualProtect(destination,length,PAGE_EXECUTE_READWRITE,&old)!=FALSE,"Cannot write private image");std::memcpy(destination,source,length);Require(FlushInstructionCache(GetCurrentProcess(),destination,length)!=FALSE,"Private code flush failed");Require(VirtualProtect(destination,length,old,&ignored)!=FALSE,"Cannot restore private page");}
void Byte(DWORD rva,unsigned char value){Write(t_image+rva,&value,1);}
void RebaseOperand(DWORD rva,DWORD original){Require(Get<DWORD>(t_image,rva)==original,"Original absolute operand differs");const DWORD relocated=static_cast<DWORD>(reinterpret_cast<uintptr_t>(t_image))+original-0x400000;Write(t_image+rva,&relocated,4);}
HRESULT __stdcall RecordSetVolume(void* receiver,LONG volume){t_receiver=receiver;t_decibels=volume;++t_setCalls;SetLastError(Outgoing);return t_hresult;}
DWORD __fastcall ObserveOriginal(void*,void*,void*,DWORD){t_observerError=GetLastError();SetLastError(Outgoing);return 0xF13579BD;}
__declspec(naked) DWORD ObserveEntry(){
    __asm {
        mov t_observerEax,eax
        mov t_observerEcx,ecx
        mov edx,dword ptr[esp+4]
        mov t_observerSound,edx
        mov edx,dword ptr[esp+8]
        mov t_observerVolume,edx
        jmp ObserveOriginal
    }
}
__declspec(naked) DWORD __stdcall NativeMix(void*,void*){
    __asm {
        push ebp
        mov edx,dword ptr[esp+8]
        mov eax,dword ptr[esp+12]
        xor ebp,ebp
        call edx
        pop ebp
        ret 8
    }
}
__declspec(naked) DWORD __stdcall NativeSetter(void*,void*,void*,DWORD,DWORD){
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov edx,dword ptr[esp+20]
        mov ecx,dword ptr[esp+24]
        mov eax,dword ptr[esp+36]
        mov ebx,0x11223344
        mov esi,0x22334455
        mov edi,0x33445566
        mov ebp,0x44556677
        mov t_before,esp
        push dword ptr[esp+32]
        push dword ptr[esp+32]
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
        ret 20
    }
}
// Both original caller fragments allocate the two callee arguments themselves.
// The new-buffer fragment additionally reads its original ESP+14 volume slot.
__declspec(naked) DWORD __stdcall PatchedCaller(void*,void*,DWORD){
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov edi,dword ptr[esp+20]
        mov esi,dword ptr[esp+24]
        mov eax,dword ptr[esp+28]
        mov edx,dword ptr[esi+0x54]
        mov ebx,0x11223344
        mov ebp,0x44556677
        mov t_before,esp
        sub esp,0x14
        mov dword ptr[esp+0x10],eax
        call edi
        add esp,0x14
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
        ret 12
    }
}
std::vector<unsigned char> Calls(){std::vector<unsigned char> out;for(DWORD rva:{0x8BE18u,0x110B33u})out.insert(out.end(),t_image+rva,t_image+rva+5);return out;}
std::string Read(const std::wstring& path){HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);Require(f!=INVALID_HANDLE_VALUE,"Cannot read trace CSV");LARGE_INTEGER n{};Require(GetFileSizeEx(f,&n)&&n.QuadPart<1024*1024,"Unexpected CSV size");std::string s(static_cast<size_t>(n.QuadPart),'\0');DWORD read=0;Require(ReadFile(f,s.data(),static_cast<DWORD>(s.size()),&read,nullptr)&&read==s.size(),"CSV read failed");CloseHandle(f);return s;}
std::vector<std::vector<std::string>> Rows(const std::wstring& path){std::istringstream in(Read(path));std::string line;std::vector<std::vector<std::string>> out;std::getline(in,line);while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();std::istringstream row(line);std::string cell;std::vector<std::string> cells;while(std::getline(row,cell,','))cells.push_back(cell);if(!cells.empty())out.push_back(cells);}return out;}
void CheckAbi(bool direct,void* fragment=nullptr){Require(t_before==t_after,"Stack/RET8 mismatch");Require(t_bx==0x11223344&&t_bp==0x44556677,"Native nonvolatile register mismatch");if(direct)Require(t_si==0x22334455&&t_di==0x33445566,"Setter nonvolatile mismatch");else Require(t_si==reinterpret_cast<uintptr_t>(t_record.data())&&t_di==reinterpret_cast<uintptr_t>(fragment),"Patched caller nonvolatile mismatch");}
void Reset(unsigned master,unsigned detail,bool ds=true){t_manager.fill(0);t_manager[0x290]=static_cast<unsigned char>(master);t_manager[0x29A]=static_cast<unsigned char>(detail);t_sound.fill(0);t_record.fill(0);Put(t_sound.data(),0,ds?&t_buffer:nullptr);Put(t_record.data(),0x54,t_sound.data());t_record[0x51]=250;t_receiver=nullptr;t_setCalls=0;t_decibels=12345;t_hresult=S_OK;}
void NativeProof(){
    constexpr unsigned char mixBytes[]={0x33,0xd2,0x8a,0x90,0x90,0x02,0x00,0x00,0x33,0xc9,0x8a,0x88,0x9a,0x02,0x00,0x00,0xb8,0xd3,0x4d,0x62,0x10,0x0f,0xaf,0xca,0xf7,0xe9,0xc1,0xfa,0x04,0x8b,0xc2,0xc1,0xe8,0x1f,0x03,0xd0,0x66,0x33,0xc0,0x85,0xed,0x8a,0xc2,0x74,0x7a};
    Require(!std::memcmp(t_image+0x6F7F9,mixBytes,sizeof(mixBytes)),"Original footstep arithmetic differs");
    // Stop the isolated arithmetic block exactly before its native playSound
    // argument setup; all executed mix instructions remain original.
    Byte(0x6F8A0,0xC3);
    struct Case {unsigned master,detail,mixed;LONG db;};
    const Case cases[]={{0,24,0,-10000},{250,250,250,-66},{125,250,125,-1700},{250,24,24,-3020},{125,24,12,-3177},{250,0,0,-10000}};
    for(const auto& c:cases){Reset(c.master,c.detail);const DWORD mixed=NativeMix(t_image+0x6F7F9,t_manager.data());Require(mixed==c.mixed,"Original footstep mix unexpected");SetLastError(Incoming);NativeSetter(t_image+0x29F0,t_manager.data(),t_sound.data(),mixed,EntryEax);CheckAbi(true);Require(t_result==S_OK&&t_setCalls==1&&t_receiver==&t_buffer&&t_decibels==c.db,"Original mix/backend volume unexpected");Require(GetLastError()==Outgoing&&t_sound[0x4C]==c.mixed,"Original backend state/error mismatch");++t_nativeCases;}
    for(const auto result:{S_OK,E_FAIL,E_ACCESSDENIED}){Reset(250,250);t_hresult=result;SetLastError(Incoming);NativeSetter(t_image+0x29F0,t_manager.data(),t_sound.data(),0,EntryEax);CheckAbi(true);Require(t_result==static_cast<DWORD>(result)&&t_decibels==-10000&&t_setCalls==1,"COM HRESULT/EAX not preserved by native setter");++t_nativeCases;}
    Reset(250,250);SetLastError(Incoming);NativeSetter(t_image+0x29F0,t_manager.data(),nullptr,250,EntryEax);CheckAbi(true);Require(t_result==EntryEax&&!t_setCalls&&GetLastError()==Incoming,"Null sound raw EAX mismatch");++t_nativeCases;
    Reset(250,250,false);SetLastError(Incoming);NativeSetter(t_image+0x29F0,t_manager.data(),t_sound.data(),125,EntryEax);CheckAbi(true);Require(t_result==static_cast<DWORD>(-1700)&&!t_setCalls&&GetLastError()==Incoming,"Null DS buffer is not a HRESULT return");++t_nativeCases;
    // Execute the original per-sound +51 factor function as well; this path
    // multiplies by 1/250 and then calls the same real 0x4029F0 backend.
    using Factor=void(__thiscall*)(void*,DWORD);
    for(const auto factor:{0u,125u,250u}){Reset(250,250);t_record[0x51]=static_cast<unsigned char>(factor);SetLastError(Incoming);reinterpret_cast<Factor>(t_image+0x110C30)(t_record.data(),250);Require(t_setCalls==1&&t_decibels==(factor==0?-10000:factor==125?-1700:-66)&&t_sound[0x4C]==factor,"Original per-sound factor unexpected");Require(GetLastError()==Outgoing,"Factor changed backend LastError");++t_nativeCases;}
}
void WrappedProof(const std::wstring& csv){
    for(DWORD entry:{0x8BE0Eu,0x110B21u}){
        void* fragment=t_image+entry;
        const char* caller=entry==0x8BE0E?"0x0048BE18":"0x00510B33";
        for(const auto volume:{0u,125u,250u,0xABCD007Du})for(const auto result:{S_OK,E_FAIL,E_ACCESSDENIED}){
            const size_t before=Rows(csv).size();Reset(0,24);t_hresult=result;
            SetLastError(Incoming);PatchedCaller(fragment,t_record.data(),volume);CheckAbi(false,fragment);
            Require(t_setCalls==1&&t_result==static_cast<DWORD>(result)&&t_receiver==&t_buffer,"Wrapper lost args/receiver/native EAX");
            Require(t_decibels==((volume&255)==0?-10000:(volume&255)==125?-1700:-66)&&t_sound[0x4C]==(volume&255),"Wrapper changed requested volume");
            Require(GetLastError()==Outgoing,"Wrapper changed native LastError");
            const auto rows=Rows(csv);Require(rows.size()==before+1,"Setter row remained buffered until exit");const auto& row=rows.back();
            Require(row.size()==22&&row[1]==caller&&std::stoul(row[9])==volume&&std::stoul(row[10])==(volume&255),"CSV caller/arguments mismatch");
            Require(row[11]=="1"&&row[12]=="0"&&row[13]=="24"&&row[14]=="1"&&row[15]=="1"&&row[16]==row[17],"CSV observed state mismatch");
            Require(std::stoul(row[19])==(volume&255)&&std::stoul(row[20],nullptr,16)==static_cast<DWORD>(result),"CSV native raw EAX/cache mismatch");
            Require(std::stoll(row[2])<=std::stoll(row[3])&&std::stoll(row[4])>0,"CSV QPC interval invalid");++t_abiCases;
        }
        for(bool nullSound:{false,true}){
            const size_t before=Rows(csv).size();Reset(0,24,false);if(nullSound)Put<void*>(t_record.data(),0x54,nullptr);
            const DWORD volume=nullSound?37:125;SetLastError(Incoming);PatchedCaller(fragment,t_record.data(),volume);CheckAbi(false,fragment);
            const DWORD expected=nullSound?(entry==0x8BE0E?static_cast<DWORD>(reinterpret_cast<uintptr_t>(t_world.data())):volume):static_cast<DWORD>(-1700);
            Require(t_result==expected&&!t_setCalls&&GetLastError()==Incoming,"Wrapper changed non-COM raw return/LastError");
            const auto rows=Rows(csv);Require(rows.size()==before+1&&std::stoul(rows.back()[20],nullptr,16)==expected,"Non-COM raw EAX was omitted or interpreted as HRESULT");++t_abiCases;
        }
        const size_t before=Rows(csv).size();Reset(0,24);t_world[0xDB]=1;SetLastError(Incoming);PatchedCaller(fragment,t_record.data(),250);CheckAbi(false,fragment);
        Require(t_setCalls==1&&t_result==S_OK&&GetLastError()==Outgoing,"Shutdown bypass changed original setter");t_world[0xDB]=0;Require(Rows(csv).size()==before,"Shutdown bypass performed CSV I/O");++t_abiCases;
    }
    // Invoke the installed thunk directly to isolate its teardown guard from
    // the original caller's own (required) World dereference.
    const DWORD delta=Get<DWORD>(t_image,0x110B34);void* thunk=t_image+0x110B38+delta;
    const auto validWorld=t_world.data();
    for(void* world:{static_cast<void*>(nullptr),reinterpret_cast<void*>(1)}){
        const size_t before=Rows(csv).size();Write(t_image+0x3C67DC,&world,4);Reset(0,24);SetLastError(Incoming);NativeSetter(thunk,t_manager.data(),t_sound.data(),250,EntryEax);CheckAbi(true);
        Require(t_result==S_OK&&t_setCalls==1&&GetLastError()==Outgoing,"Invalid/null World bypass changed original");Require(Rows(csv).size()==before,"Invalid/null World attempted diagnostic write");++t_abiCases;
    }
    Write(t_image+0x3C67DC,&validWorld,4);
    // Invalid manager inspection must not affect the original native routine,
    // which does not dereference ECX on this path.
    const size_t before=Rows(csv).size();Reset(0,24);SetLastError(Incoming);NativeSetter(thunk,reinterpret_cast<void*>(1),t_sound.data(),125,EntryEax);CheckAbi(true);
    Require(t_result==S_OK&&t_setCalls==1&&GetLastError()==Outgoing,"Invalid manager capture changed native result");const auto rows=Rows(csv);Require(rows.size()==before+1&&rows.back()[11]=="0","Invalid manager was recorded as valid");++t_abiCases;
    // Observe the original entry registers/arguments directly, after all real
    // native execution proofs above. The thunk still calls this mapped target.
    unsigned char jump[5]{0xE9};const DWORD jumpDelta=static_cast<DWORD>(reinterpret_cast<uintptr_t>(&ObserveEntry))-static_cast<DWORD>(reinterpret_cast<uintptr_t>(t_image+0x29F5));std::memcpy(jump+1,&jumpDelta,4);Write(t_image+0x29F0,jump,5);
    for(DWORD site:{0x8BE18u,0x110B33u}){
        const DWORD callDelta=Get<DWORD>(t_image,site+1);void* wrapper=t_image+site+5+callDelta;
        SetLastError(Incoming);NativeSetter(wrapper,reinterpret_cast<void*>(0x1234),reinterpret_cast<void*>(0x20),0xABCD007D,EntryEax);CheckAbi(true);
        Require(t_observerEcx==0x1234&&t_observerSound==0x20&&t_observerVolume==0xABCD007D&&t_observerEax==EntryEax,"Wrapper changed original this/arguments/incoming EAX");
        Require(t_observerError==Incoming&&GetLastError()==Outgoing&&t_result==0xF13579BD,"Wrapper changed entry/exit LastError/raw EAX");++t_abiCases;
    }
    SetLastError(Incoming);FlushAudioTrace();Require(GetLastError()==Incoming,"Explicit flush changed LastError");
}
} // namespace
int wmain(int argc,wchar_t** argv){
    HANDLE game=INVALID_HANDLE_VALUE,mapping=nullptr;HMODULE crt=nullptr;
    try{
        Require(argc==2,"Pass original Morrowind.exe");game=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);Require(game!=INVALID_HANDLE_VALUE,"Cannot open original EXE");mapping=CreateFileMappingW(game,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);Require(mapping!=nullptr,"Cannot map image");t_image=static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Require(t_image!=nullptr,"Cannot view private image");
        wchar_t folder[128]{};swprintf_s(folder,L"audio-fixtures-%lu-%llu",GetCurrentProcessId(),GetTickCount64());Require(CreateDirectoryW(folder,nullptr)!=FALSE,"Cannot create private fixture directory");const std::wstring directory=folder,log=directory+L"\\game.log",csv=directory+L"\\audio-volume.csv";SetEnvironmentVariableW(L"MW_MODERN_LOG",log.c_str());
        SetEnvironmentVariableW(L"MW_MODERN_TRACE",L"0");Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Trace enabled without opt-in");SetEnvironmentVariableW(L"MW_MODERN_TRACE",L"1");Require(!InstallAudioTrace(nullptr)&&!InstallAudioTrace(reinterpret_cast<HMODULE>(1)),"Accepted invalid image");t_guards+=2;
        SetEnvironmentVariableW(L"MW_MODERN_LOG",nullptr);Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Missing log path accepted");SetEnvironmentVariableW(L"MW_MODERN_LOG",L"game.log");Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Path without directory accepted");SetEnvironmentVariableW(L"MW_MODERN_LOG",log.c_str());
        std::vector<DWORD> corrupt;for(DWORD i=0;i<109;++i)corrupt.push_back(0x29F0+i);for(DWORD i=0;i<12;++i)corrupt.push_back(0x8BE14+i);for(DWORD i=0;i<12;++i)corrupt.push_back(0x110B30+i);
        for(DWORD rva:corrupt){const auto before=Calls();const auto value=t_image[rva];Byte(rva,static_cast<unsigned char>(value^0x5A));Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Accepted changed native instruction/signature");const auto modified=Calls();if(rva<0x8BE18||rva>=0x8BE1D){if(rva<0x110B33||rva>=0x110B38)Require(modified==before,"Rejected image was partially patched");}Byte(rva,value);Require(Calls()==before,"Guard test did not restore calls");++t_guards;}
        for(const auto failure:{std::pair<int,int>{0,1},{0,2},{1,0},{2,0},{0,3},{0,4}}){const auto before=Calls();std::array<DWORD,2> modes{};size_t modeIndex=0;for(DWORD rva:{0x8BE18u,0x110B33u}){MEMORY_BASIC_INFORMATION info{};Require(VirtualQuery(t_image+rva,&info,sizeof(info))!=0,"Cannot query initial code protection");modes[modeIndex++]=info.Protect;}SetAudioTraceTestFailures(failure.first,failure.second);Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Injected installation failure was ignored");Require(Calls()==before,"Installation failure did not roll back both calls");modeIndex=0;for(DWORD rva:{0x8BE18u,0x110B33u}){MEMORY_BASIC_INFORMATION info{};Require(VirtualQuery(t_image+rva,&info,sizeof(info))!=0&&info.Protect==modes[modeIndex++],"Installation failure did not restore code page protection");}}
        SetAudioTraceTestFailures(0,0);Require(InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Trace install failed");Require(InstallAudioTrace(reinterpret_cast<HMODULE>(t_image)),"Idempotent trace install failed");Require(!InstallAudioTrace(reinterpret_cast<HMODULE>(1)),"Accepted second image");
        t_bufferTable[0x3C/4]=reinterpret_cast<void*>(&RecordSetVolume);t_buffer.table=t_bufferTable.data();Put(t_world.data(),0x34,t_manager.data());const auto worldPtr=t_world.data();Write(t_image+0x3C67DC,&worldPtr,4);
        // Rebase only absolute operands/IAT addresses in the private mapping.
        // The executed game instructions and actual Windows MSVCRT _ftol stay
        // original; no game/assets or live process are loaded or patched.
        RebaseOperand(0x2A1A,0x746480);RebaseOperand(0x3276A0,0x74626C);crt=LoadLibraryW(L"msvcrt.dll");Require(crt!=nullptr,"Cannot load system MSVCRT");const auto ftol=GetProcAddress(crt,"_ftol");Require(ftol!=nullptr,"Cannot find original CRT _ftol");Write(t_image+0x34626C,&ftol,4);
        RebaseOperand(0x110C35,0x7C67DC);RebaseOperand(0x110C81,0x7C67DC);RebaseOperand(0x110C6C,0x74A9E4);
        NativeProof();
        RebaseOperand(0x8BE10,0x7C67DC);RebaseOperand(0x110B2A,0x7C67DC);Byte(0x8BE1D,0xC3);Byte(0x110B38,0xC3);
        WrappedProof(csv);
        std::printf("PASS native=%u guards=%u patchedCALL_ABI=%u\n",t_nativeCases,t_guards,t_abiCases);
        if(crt)FreeLibrary(crt);UnmapViewOfFile(t_image);CloseHandle(mapping);CloseHandle(game);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());if(crt)FreeLibrary(crt);if(t_image)UnmapViewOfFile(t_image);if(mapping)CloseHandle(mapping);if(game!=INVALID_HANDLE_VALUE)CloseHandle(game);return 1;}
}
