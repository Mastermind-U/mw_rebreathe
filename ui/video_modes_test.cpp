#include "video_modes.h"
#include "../display/display.h"
#include <map>
#include <string>
#include <cstdio>

namespace {
using namespace ModernUi;
struct PropertyNode {PropertyNode* left;PropertyNode* parent;PropertyNode* right;short key;short padding;DWORD value;DWORD type;};
struct Node {unsigned char bytes[0x184]{};Element* children[32]{};PropertyNode header{};PropertyNode nativeProperties[16]{};};
Node pool[64];unsigned used=0,layouts=0,changes=0;Element* live=nullptr;Element* layoutRoot=nullptr;
PropertyNode nil{};Element* globalRoot=nullptr;
bool closeDuringChange=false;DisplayMode selected=DisplayMode::Borderless;
std::map<std::string,short> ids;
struct Property {DWORD value;int type;};
std::map<Element*,std::map<int,Property>> properties;
Element* New(Element* parent,int id) {
    Node& node=pool[used++];node=Node{};auto* e=reinterpret_cast<Element*>(node.bytes);
    Put<char>(e,0,'X');Put<short>(e,0x14,short(id));Put<unsigned char>(e,0x75,1);
    Put(e,0x1C,node.children);Put(e,0x20,node.children);Put(e,0x24,node.children+32);
    node.header.left=node.header.parent=node.header.right=&nil;Put(e,0x30,&node.header);
    if(parent){auto** end=Get<Element**>(parent,0x20);*end=e;Put(parent,0x20,end+1);Put(e,0x28,parent);}return e;
}
Element* Find(Element* e,int id){if(!e)return nullptr;if(Get<short>(e,0x14)==short(id))return e;Children c(e);for(size_t i=0;i<c.Count();++i)if(auto* f=Find(c.first[i],id))return f;return nullptr;}
short __cdecl Id(const char* s){auto f=ids.find(s);if(f!=ids.end())return f->second;return ids[s]=short(1000+ids.size());}
Element* __cdecl Menu(int){return live;}
Element* __fastcall FindChild(Element* e,void*,int id){return Find(e,id);}
Element* __fastcall Block(Element* p,void*,int id,bool){return New(p,id);}
Element* __fastcall Rect(Element* p,void*,int id,bool,bool){return New(p,id);}
Element* __fastcall Label(Element* p,void*,int id,const char*,bool,bool){return New(p,id);}
void __fastcall Text(Element*,void*,const char*){}
void __fastcall SetProperty(Element* e,void*,int key,DWORD value,int type){
    properties[e][key]={value,type};
    Node& node=*reinterpret_cast<Node*>(e);size_t index=0;
    for(const auto& property:properties[e]){
        PropertyNode& p=node.nativeProperties[index];p.left=&nil;p.right=&nil;
        p.parent=index?node.nativeProperties+index-1:&node.header;p.key=short(property.first);
        p.value=property.second.value;p.type=DWORD(property.second.type);
        if(index)node.nativeProperties[index-1].right=&p;++index;
    }
    node.header.left=index?node.nativeProperties:&node.header;
    node.header.parent=index?node.nativeProperties:&nil;
    node.header.right=index?node.nativeProperties+index-1:&node.header;
    Put(e,0x38,unsigned(index));
}
void* __fastcall GetProperty(Element* e,void*,DWORD* out,int key,int type,const Element*,bool inherited){
    *out=0;
    for(Element* current=e;current;current=inherited?Parent(current):nullptr){
        const auto owner=properties.find(current);if(owner==properties.end())continue;
        const auto found=owner->second.find(key);if(found!=owner->second.end()&&found->second.type==type){*out=found->second.value;break;}
    }
    return out;
}
void Bounds(Element* e,int parentX=0,int parentY=0){
    const int x=parentX+Get<int>(e,0xDC),y=parentY+Get<int>(e,0xE0);
    Put(e,0xE4,x);Put(e,0xE8,y);Put(e,0x128,x);Put(e,0x12C,x+Get<int>(e,0xEC));
    Put(e,0x130,y-Get<int>(e,0xF0));Put(e,0x134,y);
    Children children(e);for(size_t i=0;i<children.Count();++i)Bounds(children.first[i],x,y);
}
Element* __fastcall Layout(Element* e,void*,bool){++layouts;layoutRoot=e;Bounds(e);return e;}
void Global(unsigned char* image,DWORD va,const void* value){
    DWORD old=0,ignored=0;auto* p=image+va-0x400000;VirtualProtect(p,4,PAGE_READWRITE,&old);
    std::memcpy(p,&value,4);VirtualProtect(p,4,old,&ignored);
}
bool RebaseGlobal(unsigned char* image,DWORD start,DWORD end,DWORD address){
    unsigned matches=0;const DWORD rebased=DWORD(reinterpret_cast<uintptr_t>(image+address-0x400000));
    for(DWORD va=start;va+4<=end;++va){
        auto* operand=image+va-0x400000;DWORD value=0;std::memcpy(&value,operand,4);
        if(value!=address)continue;
        DWORD old=0,ignored=0;if(!VirtualProtect(operand,4,PAGE_EXECUTE_READWRITE,&old))return false;
        std::memcpy(operand,&rebased,4);VirtualProtect(operand,4,old,&ignored);++matches;va+=3;
    }
    return matches!=0;
}
bool Mock(unsigned char* image,DWORD va,const void* target){
    unsigned char* p=image+va-0x400000;DWORD old=0,ignored=0;if(!VirtualProtect(p,5,PAGE_EXECUTE_READWRITE,&old))return false;
    p[0]=0xE9;const DWORD delta=DWORD(reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(p+5));std::memcpy(p+1,&delta,4);
    FlushInstructionCache(GetCurrentProcess(),p,5);return VirtualProtect(p,5,old,&ignored)!=FALSE;
}
}
DisplayMode GetDisplayMode(){return selected;}
bool SetDisplayMode(DisplayMode mode){++changes;selected=mode;if(closeDuringChange)live=nullptr;return true;}

int main(int argc,char** argv){
    if(argc!=2)return 2;
    HANDLE file=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE mapping=file==INVALID_HANDLE_VALUE?nullptr:CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    auto* image=mapping?static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;if(!image)return 3;
    nil.left=nil.parent=nil.right=&nil;Global(image,0x7D1CEC,&nil);
    bool ok=Mock(image,0x58DF10,&Id)&&Mock(image,0x595370,&Menu)&&Mock(image,0x582DE0,&FindChild)&&
        Mock(image,0x588980,&Block)&&Mock(image,0x588340,&Rect)&&Mock(image,0x588BE0,&Label)&&
        Mock(image,0x58AD30,&Text)&&Mock(image,0x581F30,&SetProperty)&&Mock(image,0x581440,&GetProperty)&&Mock(image,0x583B70,&Layout);
    // These original functions keep their original instructions/control flow;
    // rebase only their absolute addresses of the private global UI pointers.
    // Property access uses a POD property-table fixture, while ancestor lookup
    // 589180, hit selection 58EDE0/587730 and dispatch 583460 execute natively.
    ok&=RebaseGlobal(image,0x583460,0x583735,0x7D1C28)&&RebaseGlobal(image,0x583460,0x583735,0x7D1C74)&&
        RebaseGlobal(image,0x583460,0x583735,0x7D2ECC)&&RebaseGlobal(image,0x587730,0x587907,0x7D1C28)&&
        RebaseGlobal(image,0x587730,0x587907,0x7D1C74)&&RebaseGlobal(image,0x589180,0x5891EA,0x7D1CEC);
    const char* choices[]={"MWModern_Video_Fullscreen","MWModern_Video_Borderless","MWModern_Video_Window"};
    const DisplayMode modes[]={DisplayMode::Fullscreen,DisplayMode::Borderless,DisplayMode::Window};
    for(int width:{512,712}){
        used=0;properties.clear();globalRoot=New(nullptr,1);live=New(globalRoot,42);Element* parent=New(live,43);
        FixedSize(globalRoot,width,720);FixedSize(live,width,720);FixedSize(parent,width,720);
        Global(image,0x7D1C28,globalRoot);Global(image,0x7D1C74,nullptr);
        // A plain Block has no native is_part. Original dispatch sends the
        // nearest PartFixedFrame as argument 1 and the hit Block as argument 5.
        SetProperty(live,nullptr,-0x7FA8,0xFFFF80A1u,0x10);
        ok&=ApplyVideoModeControls(reinterpret_cast<HMODULE>(image),live,parent,width);
        const unsigned first=used;ok&=ApplyVideoModeControls(reinterpret_cast<HMODULE>(image),live,parent,width)&&used==first;
        Element* row=Find(live,Id("MWModern_Video_DisplayModes"));
        ok&=row&&Get<int>(row,0xF0)==VideoModeControlsHeight&&Get<unsigned char>(row,0x115)==0;
        for(unsigned i=0;i<3;++i){
            Element* button=Find(row,Id(choices[i]));Children visuals(button);
            ok&=button&&Get<int>(button,0xDC)+Get<int>(button,0xEC)<=width&&visuals.Count()==2;
            for(size_t n=0;n<visuals.Count();++n)ok&=Get<unsigned char>(visuals.first[n],0x7F)==0;
            auto callback=reinterpret_cast<Callback>(static_cast<uintptr_t>(properties[button][-0x7FCB].value));
            const unsigned before=changes;ok&=callback&&callback(button,-0x7FCB,0,0,button)&&selected==modes[i]&&changes==before+1&&layoutRoot==live;
        }
        Layout(live,nullptr,true);
        auto pick=reinterpret_cast<Element* (__thiscall*)(void*,Element*,int,int)>(image+0x58EDE0-0x400000);
        auto dispatch=reinterpret_cast<bool (__thiscall*)(Element*,int,int,int,Element*)>(image+0x583460-0x400000);
        auto hitTest=reinterpret_cast<bool (__thiscall*)(const Element*,int,int)>(image+0x587730-0x400000);
        for(unsigned i=0;i<3;++i){
            Element* button=Find(row,Id(choices[i]));const int x=Get<int>(button,0xE4)+Get<int>(button,0xEC)/2;
            const int y=Get<int>(button,0xE8)-Get<int>(button,0xF0)/2;
            Element* hit=pick(globalRoot,globalRoot,x,y);const unsigned before=changes;
            const bool hitOk=hit==button&&hitTest(button,x,y)&&!hitTest(button,x-Get<int>(button,0xEC),y);
            const bool fired=dispatch(hit,-0x7FCB,0,0,hit);
            const bool nativeOk=hitOk&&fired&&changes==before+1&&selected==modes[i];ok&=nativeOk;
            std::printf("original selector 58EDE0 + hit 587730 + dispatch 583460 width=%d mode=%u %s (hit=%d fired=%d changes=%u sourceId=%d expectedId=%d)\n",width,i,nativeOk?"passed":"FAILED",int(hitOk),int(fired),changes-before,hit?Get<short>(hit,0x14):0,Get<short>(button,0x14));
        }
        std::printf("native mode-row ABI/actions/idempotent geometry width=%d %s\n",width,ok?"passed":"FAILED");
    }
    Element* oldRoot=live;Element* button=Find(live,Id(choices[0]));const unsigned before=layouts;closeDuringChange=true;
    auto callback=reinterpret_cast<Callback>(static_cast<uintptr_t>(properties[button][-0x7FCB].value));
    ok&=callback(button,-0x7FCB,0,0,button)&&live==nullptr&&layouts==before;
    ok&=!ApplyVideoModeControls(reinterpret_cast<HMODULE>(image),oldRoot,oldRoot,299);
    UnmapViewOfFile(image);CloseHandle(mapping);CloseHandle(file);
    std::puts(ok?"PASS: native mode-row callbacks, three modes, full-root layout, non-consuming labels, reuse and lifetime":"FAILED");return ok?0:1;
}
