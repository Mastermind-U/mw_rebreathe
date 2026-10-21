// Execute the original NiNode attach/detach/compact code on private test objects.
// No native UI factories, game process, asset loading or production DLL is used.
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

static_assert(sizeof(void*)==4,"This native ABI fixture is x86");
namespace {
unsigned char* g_image=nullptr;
std::array<void*,44> g_vtable{};
unsigned g_destroyed=0;
unsigned g_controllerUpdates=0;
float g_controllerTime=0;
std::array<float,4> g_zeroBound{};
uintptr_t g_before=0,g_after=0,g_result=0;
uint32_t g_bx=0,g_si=0,g_di=0,g_bp=0;
constexpr DWORD ErrorToken=0x13572468;
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T> T Get(const void* object,size_t offset){T x{};std::memcpy(&x,static_cast<const unsigned char*>(object)+offset,sizeof(x));return x;}
template<class T> void Put(void* object,size_t offset,T x){std::memcpy(static_cast<unsigned char*>(object)+offset,&x,sizeof(x));}
void __fastcall UnexpectedDestructor(void*,void*,unsigned){++g_destroyed;}
void* __fastcall ZeroBound(void*,void*){return g_zeroBound.data();}
bool __fastcall BoundUnchanged(void*,void*){return false;}
void __fastcall ObserveController(void*,void*,float time){++g_controllerUpdates;g_controllerTime=time;}
struct Node {
    std::array<unsigned char,0xB0> bytes{};
    std::array<void*,64> slots{};
    std::array<float,9> rotation{1,0,0,0,1,0,0,0,1};
    std::array<float,6> auxiliary{};
    Node(){Put(bytes.data(),0,g_vtable.data());Put<uint32_t>(bytes.data(),4,1);Put(bytes.data(),0x2C,rotation.data());Put<float>(bytes.data(),0x3C,1);Put(bytes.data(),0x74,auxiliary.data());Put(bytes.data(),0x94,slots.data());Put<uint32_t>(bytes.data(),0x98,64);Put<uint32_t>(bytes.data(),0xA4,16);}
    void* Ptr(){return bytes.data();}
    uint32_t Refs()const{return Get<uint32_t>(bytes.data(),4);}
    uint32_t High()const{return Get<uint32_t>(bytes.data(),0x9C);}
    uint32_t Count()const{return Get<uint32_t>(bytes.data(),0xA0);}
    void* Parent()const{return Get<void*>(bytes.data(),0x18);}
    std::vector<void*> Order()const {std::vector<void*> v;Require(High()<=slots.size(),"Native array escaped pinned capacity");for(unsigned i=0;i<High();++i)if(slots[i])v.push_back(slots[i]);return v;}
};
// Original thiscalls both pop two DWORD arguments. Seed the nonvolatile
// registers and capture ESP around the real CALL, not a mock function.
__declspec(naked) uintptr_t __stdcall Invoke2(void*,void*,uintptr_t,uintptr_t){
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax,dword ptr[esp+20]
        mov ecx,dword ptr[esp+24]
        mov ebx,0x11223344
        mov esi,0x22334455
        mov edi,0x33445566
        mov ebp,0x44556677
        mov g_before,esp
        push dword ptr[esp+32]
        push dword ptr[esp+32]
        call eax
        mov g_result,eax
        mov g_after,esp
        mov g_bx,ebx
        mov g_si,esi
        mov g_di,edi
        mov g_bp,ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 16
    }
}
// The native dispatcher consumes time/controllers/bounds with RET 0Ch.
__declspec(naked) uintptr_t __stdcall Invoke3(void*,void*,uintptr_t,uintptr_t,uintptr_t){
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov eax,dword ptr[esp+20]
        mov ecx,dword ptr[esp+24]
        mov ebx,0x11223344
        mov esi,0x22334455
        mov edi,0x33445566
        mov ebp,0x44556677
        mov g_before,esp
        push dword ptr[esp+36]
        push dword ptr[esp+36]
        push dword ptr[esp+36]
        call eax
        mov g_result,eax
        mov g_after,esp
        mov g_bx,ebx
        mov g_si,esi
        mov g_di,edi
        mov g_bp,ebp
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret 20
    }
}
void CheckAbi(){Require(g_before==g_after,"Native RET8/stack mismatch");Require(g_bx==0x11223344&&g_si==0x22334455&&g_di==0x33445566&&g_bp==0x44556677,"Native nonvolatile register mismatch");Require(GetLastError()==ErrorToken,"Native test changed LastError");Require(!g_destroyed,"Native operation destroyed a UI-owned scene node");}
void Attach(Node& parent,Node& child,bool vacant=true){SetLastError(ErrorToken);Invoke2(g_image+0x2C8410,parent.Ptr(),reinterpret_cast<uintptr_t>(child.Ptr()),vacant);CheckAbi();}
void* Detach(Node& parent,Node& child){void* result=nullptr;SetLastError(ErrorToken);const auto returned=Invoke2(g_image+0x2C8680,parent.Ptr(),reinterpret_cast<uintptr_t>(&result),reinterpret_cast<uintptr_t>(child.Ptr()));CheckAbi();Require(returned==reinterpret_cast<uintptr_t>(&result),"Detach did not return output NiPointer address");return result;}
void Release(void* node){if(node)reinterpret_cast<void(__thiscall*)(void*)>(g_image+0x4630)(node);}
void Compact(Node& parent){reinterpret_cast<void(__thiscall*)(void*)>(g_image+0x18C3D0)(parent.bytes.data()+0x90);}
void Reappend(Node& parent,Node& child){void* held=Detach(parent,child);Require(held==child.Ptr(),"Reorder failed to detach existing child");Release(held);Compact(parent);Attach(parent,child,true);}
void CheckOrder(const Node& parent,std::initializer_list<void*> expected){Require(parent.Order()==std::vector<void*>(expected),"Native draw order mismatch");Require(parent.Count()==expected.size(),"Native nonempty child count mismatch");}
void Local(Node& node,float x,float y,float z=0){Put<float>(node.Ptr(),0x30,x);Put<float>(node.Ptr(),0x34,y);Put<float>(node.Ptr(),0x38,z);}
void CheckWorld(Node& node,float x,float y,float z=0){Require(Get<float>(node.Ptr(),0x64)==x&&Get<float>(node.Ptr(),0x68)==y&&Get<float>(node.Ptr(),0x6C)==z,"Native cached NiWorld translation mismatch");Require(Get<float>(node.Ptr(),0x70)==1.0f,"Native cached NiWorld scale mismatch");}
void UpdateWorld(Node& root,float time,bool controllers,bool bounds=true){uintptr_t bits=0;std::memcpy(&bits,&time,sizeof(time));SetLastError(ErrorToken);Invoke3(g_image+0x2EB000,root.Ptr(),bits,controllers,bounds);CheckAbi();}
void RebaseZeroConstant(){
    // SEC_IMAGE was mapped away from the original base. Only this absolute
    // read on the executed path needs relocation; math/control code is original.
    auto* operand=g_image+0x2C8BE2;
    Require(Get<uint32_t>(operand,0)==0x74BFF0,"Native bounds zero operand differs");
    DWORD old=0;Require(VirtualProtect(operand,4,PAGE_EXECUTE_WRITECOPY,&old)!=FALSE,"Cannot relocate private-image constant");
    Put(operand,0,reinterpret_cast<uintptr_t>(g_image+0x34BFF0));
    DWORD ignored=0;Require(VirtualProtect(operand,4,old,&ignored)!=FALSE,"Cannot restore private-image protection");
    Require(FlushInstructionCache(GetCurrentProcess(),operand,4)!=FALSE,"Cannot flush private-image instruction cache");
}
void RunWorldRefresh(){
    Node root,oldPanel,newPanel,row,label;
    std::array<void*,14> controllerVtable{};
    std::array<unsigned char,0x30> controller{};
    controllerVtable[0x34/4]=reinterpret_cast<void*>(&ObserveController);
    Put(controller.data(),0,controllerVtable.data());
    for(Node* n:{&root,&oldPanel,&newPanel,&row,&label})Put(n->Ptr(),0x10,controller.data());
    Local(root,-600,400);Local(oldPanel,20,-30);Local(newPanel,100,-80);Local(row,7,-9);Local(label,15,-19);
    Attach(root,oldPanel);Attach(root,newPanel);Attach(oldPanel,row);Attach(row,label);
    // The same native controller walker is called in the positive control.
    UpdateWorld(root,12.5f,true);
    Require(g_controllerUpdates==5&&g_controllerTime==12.5f,"Native controller positive control did not execute once per node");
    CheckWorld(root,-600,400);CheckWorld(row,-573,361);CheckWorld(label,-558,342);
    const unsigned updates=g_controllerUpdates;
    Attach(newPanel,row);
    CheckWorld(row,-573,361);CheckWorld(label,-558,342); // cached values really are stale
    UpdateWorld(root,0.0f,false);
    Require(g_controllerUpdates==updates,"Transform-only update advanced controllers");
    CheckWorld(row,-493,311);CheckWorld(label,-478,292);
    Local(row,11,-13,3);Local(label,-5,4,2);
    UpdateWorld(root,0.0f,false);
    Require(g_controllerUpdates==updates,"Repeated transform-only update advanced controllers");
    CheckWorld(row,-489,307,3);CheckWorld(label,-494,311,5);
    for(Node* parent:{&row,&newPanel,&oldPanel,&root})for(void* p:parent->Order()){void* held=nullptr;SetLastError(ErrorToken);Invoke2(g_image+0x2C8680,parent->Ptr(),reinterpret_cast<uintptr_t>(&held),reinterpret_cast<uintptr_t>(p));CheckAbi();Release(held);}
    for(Node* n:{&root,&oldPanel,&newPanel,&row,&label})Require(n->Refs()==1&&!n->Parent(),"World refresh fixture leaked scene ownership");
}
void Run(){
    Node oldParent,newParent,ownGeometry,a,b,c;
    Attach(oldParent,a);Attach(oldParent,b);Attach(oldParent,c);
    CheckOrder(oldParent,{a.Ptr(),b.Ptr(),c.Ptr()});
    Require(a.Parent()==oldParent.Ptr()&&a.Refs()==2,"Initial native parent/refcount mismatch");
    Attach(newParent,ownGeometry);
    Attach(newParent,b); // Native SetParent must detach actual old Ni parent.
    CheckOrder(oldParent,{a.Ptr(),c.Ptr()});CheckOrder(newParent,{ownGeometry.Ptr(),b.Ptr()});
    Require(b.Parent()==newParent.Ptr()&&b.Refs()==2,"Reparent changed node identity/ownership");
    void* held=Detach(oldParent,a);
    Require(held==a.Ptr()&&a.Parent()==nullptr&&a.Refs()==2,"Detached temporary NiPointer ownership mismatch");
    Release(held);Require(a.Refs()==1,"Released detach result did not retain sole UI reference");
    void* absent=Detach(oldParent,a);Require(!absent&&a.Refs()==1,"Absent detach changed node reference count");
    Attach(newParent,a);Attach(newParent,c);
    CheckOrder(oldParent,{});CheckOrder(newParent,{ownGeometry.Ptr(),b.Ptr(),a.Ptr(),c.Ptr()});
    const unsigned bound=newParent.High();
    for(unsigned i=0;i<1000;++i){
        Reappend(newParent,a);Reappend(newParent,b);Reappend(newParent,c);
        CheckOrder(newParent,{ownGeometry.Ptr(),a.Ptr(),b.Ptr(),c.Ptr()});
        Require(newParent.High()<=bound,"Reorder grew NiArray highwater");
        Require(a.Refs()==2&&b.Refs()==2&&c.Refs()==2,"Repeated reorder drifted native refcounts");
    }
    // A hole is packed, but the standalone compactor does not reset highwater.
    held=Detach(newParent,b);Release(held);const unsigned holeHigh=newParent.High();
    Compact(newParent);Require(newParent.High()==holeHigh,"Unexpected compactor highwater semantics");
    CheckOrder(newParent,{ownGeometry.Ptr(),a.Ptr(),c.Ptr()});Attach(newParent,b,true);
    Require(newParent.High()==bound&&b.Parent()==newParent.Ptr(),"First-vacancy insertion changed highwater");
    // Explicit original SetParent(nullptr) also detaches and preserves the
    // existing UI-owned object. Reattach is required to gain a parent ref.
    reinterpret_cast<void(__thiscall*)(void*,void*)>(g_image+0x2EACC0)(b.Ptr(),nullptr);
    Require(!b.Parent()&&b.Refs()==1,"Native SetParent(nullptr) failed");Attach(newParent,b);
    for(Node* child:{&ownGeometry,&a,&b,&c}){held=Detach(newParent,*child);Release(held);Require(child->Refs()==1&&!child->Parent(),"Final scene reference cleanup mismatch");}
    CheckOrder(newParent,{});Require(!g_destroyed,"Native scene object was destroyed");
}
}
int wmain(int argc,wchar_t** argv){
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;
    try {
        Require(argc==2,"Pass original Morrowind.exe");
        file=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);Require(file!=INVALID_HANDLE_VALUE,"Cannot open original executable");
        mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);Require(mapping!=nullptr,"Cannot create private image mapping");
        g_image=static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Require(g_image!=nullptr,"Cannot map private executable image");
        Require(!std::memcmp(g_image+0x2C8410,"\x64\xA1\x00\x00\x00\x00\x6A\xFF",8)&&
                !std::memcmp(g_image+0x2C8680,"\x6A\xFF\x68\xD8\x28\x73\x00",7)&&
                !std::memcmp(g_image+0x2EACC0,"\x51\x56\x8B\xF1\x8B\x4E\x18",7)&&
                !std::memcmp(g_image+0x18C3D0,"\x51\x53\x8B\xD9\x8B\x4B\x10",7),"Native NiNode signatures differ");
        g_vtable[0]=reinterpret_cast<void*>(&UnexpectedDestructor);g_vtable[0x94/4]=g_image+0x2C8410;g_vtable[0x98/4]=g_image+0x2C8680;
        Run();
        Require(!std::memcmp(g_image+0x2EB000,"\x8B\x54\x24\x08\x56\x8B\xF1",7)&&
                !std::memcmp(g_image+0x2C8B40,"\x8A\x44\x24\x08\x83\xEC\x08\x84\xC0",9)&&
                !std::memcmp(g_image+0x2FB350,"\x81\xEC\xB0\x00\x00\x00\x56\x8B\xF1",9),"Native world update signatures differ");
        RebaseZeroConstant();
        g_vtable[0x34/4]=reinterpret_cast<void*>(&ZeroBound);g_vtable[0x64/4]=reinterpret_cast<void*>(&BoundUnchanged);
        g_vtable[0x60/4]=g_image+0x2C8B40;g_vtable[0x8C/4]=g_image+0x2FB350;
        RunWorldRefresh();
        std::puts("Passed: original NiAttach/NiDetach/SetParent/compact executed; RET8 and registers; temporary references; actual old-parent removal; 1000 reorder passes (3000 reappends), stable draw order/refcounts/highwater; no scene object destruction.");
        std::puts("Passed: original 6EB000/6C8B40/6FB350 execute with RET12/register preservation; cached world XYZ remain stale after reparent and refresh correctly; actual controller walker positive control=5 calls, transform-only updates=0 calls. Fixture bounds getters are substituted; production geometry/bounds are outside this test.");
        UnmapViewOfFile(g_image);CloseHandle(mapping);CloseHandle(file);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAILED: %s (Win32 %lu)\n",e.what(),GetLastError());if(g_image)UnmapViewOfFile(g_image);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return 1;}
}
