#define NOMINMAX
#include "options_layout.h"
#include "menu_style.h"
#include "video_modes.h"
#include "native_ui.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
using ModernUi::Element; using ModernUi::Get; using ModernUi::Put;
namespace {
struct Node { unsigned char object[0x184]{}; Element* children[64]{}; int part=0; };
Node g_nodes[96]; unsigned g_used=0,g_tab=0,g_ctor=0,g_theme=0,g_layoutCalls=0;
Element* g_root=nullptr; unsigned char* g_image=nullptr;
DWORD g_extra=0xDEADBEEF; bool g_bad=false,g_measure=false;unsigned g_measureCases=0,g_modeCalls=0,g_audioZeroLayouts=0,g_audioCommittedLayouts=0,g_audioClipCases=0;
uintptr_t g_before=0,g_after=0;DWORD g_bx=0,g_si=0,g_di=0,g_bp=0;
constexpr DWORD kIncoming=0x12345678,kOutgoing=0x87654321;
constexpr DWORD kCalls[]={0x5A1A9A,0x601CFA,0x637F06,0x5BC3FA,0x63856A};
constexpr DWORD kCtors[]={0x5A10D0,0x600FD0,0x6372A0,0x5BB9F0};
constexpr DWORD kMenus[]={0x7D265C,0x7D522C,0x7D7718,0x7D33E8};
constexpr DWORD kOks[]={0x7D2684,0x7D5118,0x7D76B8,0x7D33C8};
unsigned char g_world[0x100]{},g_rootScene[0xA0]{},g_okScene[0xA0]{};
void* g_sceneTable[0x28]{};unsigned g_syncCalls=0,g_sceneRefreshCalls=0,g_worldRefreshCalls=0;unsigned char g_originalScene[5]{};
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
Node& Model(Element* element){for(auto& n:g_nodes)if(reinterpret_cast<Element*>(n.object)==element)return n;throw std::runtime_error("Unknown fake element");}
void Append(Element* parent,Element* child){auto& n=Model(parent);auto* end=Get<Element**>(parent,0x20);size_t count=size_t(end-n.children);Require(count<64,"Fake vector overflow");n.children[count]=child;Put(parent,0x20,n.children+count+1);Put(child,0x28,parent);}
Element* New(Element* parent,int id){Require(g_used<96,"Fake arena overflow");auto& n=g_nodes[g_used++];std::memset(&n,0,sizeof(n));Element* e=reinterpret_cast<Element*>(n.object);Put<char>(e,0,'X');Put<short>(e,0x14,static_cast<short>(id));Put(e,0x1C,n.children);Put(e,0x20,n.children);Put<int>(e,0xEC,200);Put<int>(e,0xF0,20);Put<float>(e,0x118,-1);Put<float>(e,0x11C,-1);Put<float>(e,0x120,-1);Put<float>(e,0x124,-1);Put<int>(e,0x104,-1);Put<int>(e,0x108,-1);Put<unsigned char>(e,0x74,1);Put<unsigned char>(e,0x75,1);if(parent)Append(parent,e);return e;}
Element* __fastcall Find(Element* root,void*,int id){if(!root)return nullptr;if(Get<short>(root,0x14)==id)return root;ModernUi::Children c(root);for(size_t i=0;i<c.Count();++i)if(auto* e=Find(c.first[i],nullptr,id))return e;return nullptr;}
Element* Last(Element* root,int id){Element* result=Get<short>(root,0x14)==id?root:nullptr;ModernUi::Children children(root);for(size_t i=0;i<children.Count();++i)if(Element* match=Last(children.first[i],id))result=match;return result;}
short __cdecl Id(const char* name){if(!std::strcmp(name,"PartButton"))return 20;if(!std::strcmp(name,"MM_Options_scroll_body"))return 21;if(!std::strcmp(name,"PartScrollPane_pane"))return 22;if(!std::strcmp(name,"MWModern_Video_DisplayModes"))return 23;if(!std::strncmp(name,"MWModern_Menu_",14))return 500;g_bad=true;return 0;}
void __fastcall SetProperty(Element*,void*,int key,DWORD value,int type){if(key!=-0x7FA7||type!=0x10||static_cast<short>(value)!=-0x7F34)g_bad=true;SetLastError(0xBAD);}
void* __fastcall GetProperty(Element* e,void*,DWORD* out,int key,int type,const Element* inherited,bool check){if(g_measure){*out=static_cast<DWORD>(static_cast<int>(key==static_cast<int>(0xFFFF8071)?(ModernUi::Children(e).Count()?0:static_cast<int>(0xFFFF80BD)):key==static_cast<int>(0xFFFF8059)?static_cast<int>(0xFFFF80CC):static_cast<int>(0xFFFF80BD)));return out;}if(key!=-0x7FA8||type!=0x10||inherited||check)g_bad=true;*out=static_cast<DWORD>(Model(e).part);return out;}
void MeasureAudioLayout(Element* root);
Element* __fastcall Layout(Element* e,void*,bool timestamp){++g_layoutCalls;if(g_tab==0)MeasureAudioLayout(e);Put(e,0x88,g_rootScene);Put(Last(e,60+g_tab),0x88,g_okScene);Put(g_rootScene,0,g_sceneTable);Put(g_okScene,0,g_sceneTable);Put(g_okScene,0x18,g_rootScene);if(timestamp!=(g_tab==2)||e!=g_root||(g_tab==2&&GetLastError()!=kIncoming))g_bad=true;SetLastError(g_tab==2?kOutgoing:0xBAD);return e;}
void* __fastcall DetachScene(void* scene,void*,void** out,void* child){if(scene!=g_rootScene||child!=g_okScene||Get<void*>(child,0x18)!=scene)g_bad=true;*out=child;Put<void*>(child,0x18,nullptr);++g_syncCalls;SetLastError(0xBAD);return out;}
void __fastcall AttachScene(void* scene,void*,void* child,bool first){if(scene!=g_rootScene||child!=g_okScene||!first)g_bad=true;Put(child,0x18,scene);SetLastError(0xBAD);}
void __fastcall RefreshWorld(void* scene,void*,float time,bool controllers,bool geometry){if(scene!=g_rootScene||time!=0.0f||controllers||!geometry||g_syncCalls!=g_worldRefreshCalls+1||g_sceneRefreshCalls!=g_syncCalls)g_bad=true;++g_worldRefreshCalls;SetLastError(0xBAD);}
void __fastcall RefreshScene(Element* child,void*){if(child!=Last(g_root,60+g_tab)||Get<unsigned char>(child,0x78)!=1)g_bad=true;++g_sceneRefreshCalls;SetLastError(0xBAD);}
void __fastcall CompactScene(void* array,void*){if(array!=g_rootScene+0x90)g_bad=true;SetLastError(0xBAD);}
void __fastcall ReleaseScene(void** ref,void*){if(!ref||*ref!=g_okScene)g_bad=true;*ref=nullptr;SetLastError(0xBAD);}
Element* __fastcall Widget(Element* parent,void*,int id,void* factory,bool replace){if(id!=21||factory!=g_image+0x248FD0||replace)g_bad=true;Element* widget=New(parent,id);New(widget,22);return widget;}
short __fastcall Remove(Element* parent,void*,Element* child){ModernUi::Children prior(parent);for(size_t i=0;i<prior.Count();++i)if(prior.first[i]==child){std::memmove(prior.first+i,prior.first+i+1,(prior.Count()-i-1)*sizeof(Element*));Put(parent,0x20,prior.last-1);return short(-0x7FFF);}g_bad=true;return 0;}
void __fastcall Insert(void* vector,void*,Element** position,unsigned count,Element* const* value){auto* parent=reinterpret_cast<Element*>(static_cast<unsigned char*>(vector)-0x18);if(count!=1||position!=Get<Element**>(parent,0x20)||!value){g_bad=true;return;}Append(parent,*value);}
void __cdecl Constructor(Element* root,DWORD extra){++g_ctor;if(extra!=g_extra||GetLastError()!=kIncoming)g_bad=true;if(root==nullptr){SetLastError(kOutgoing);return;}Require(root==reinterpret_cast<Element*>(g_nodes[0].object),"Constructor root argument changed");g_used=0;Element* created=New(nullptr,40+g_tab);Require(created==root,"Arena root changed");Element* content=New(root,50);Element* tabs=New(content,10);for(int i=0;i<4;++i){Element* button=New(tabs,100+i);Model(button).part=20;Element* text=New(button,200+i);Put<short>(text,0x160,-0x7F4A);}Element* body=content;if(g_tab==0){body=New(content,30);Put(body,0x11C,1.0f);Put(body,0x118,1.0f);Put(body,0xF0,0);}
for(int i=0;i<12;++i){Element* row=New(body,300+i);Put(row,0x118,1.0f);Put<DWORD>(row,0x170,0xFEED0000+i);Element* text=New(row,400+i);Put<short>(text,0x160,-0x7F4A);}if(g_tab==3){Element* list=New(content,31);Put(list,0x11C,1.0f);Put(list,0x118,1.0f);Put(list,0xF0,0);Element* keyPane=New(list,32);Put<unsigned char>(keyPane,0x115,1);for(int i=0;i<34;++i){Element* key=New(keyPane,600+i);Put(key,0xF0,24);Put<DWORD>(key,0x170,0xB17D0000u+i);}}
if(g_tab==3){Element* reset=New(content,63);Put<DWORD>(reset,0x170,0xCAFECAFE);Model(reset).part=20;}Element* footer=New(content,11);Element* ok=New(footer,60+g_tab);Model(ok).part=20;Put<DWORD>(ok,0x170,0xCA11BAC0);SetLastError(kOutgoing);}
__declspec(naked) void __stdcall Invoke(void*) { __asm {
    push ebx
    push esi
    push edi
    push ebp
    mov eax,[esp+20]
    mov ebx,13579BDFh
    mov esi,g_root
    mov edi,2468ACE0h
    mov ebp,31415926h
    mov g_before,esp
    push offset returned
    push g_extra
    jmp eax
returned:
    mov g_after,esp
    mov g_bx,ebx
    mov g_si,esi
    mov g_di,edi
    mov g_bp,ebp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret 4
} }
__declspec(naked) Element* __stdcall InvokeVideo(void*) { __asm {
    push ebx
    push esi
    push edi
    push ebp
    mov eax,[esp+20]
    mov ebx,g_root
    mov esi,11223344h
    mov edi,2468ACE0h
    mov ebp,31415926h
    mov g_before,esp
    call eax
    mov g_after,esp
    mov g_bx,ebx
    mov g_si,esi
    mov g_di,edi
    mov g_bp,ebp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret 4
} }
__declspec(naked) Element* __stdcall InvokeResize(void*) { __asm {
    push ebx
    push esi
    push edi
    push ebp
    mov eax,[esp+20]
    mov ebx,13579BDFh
    mov esi,g_root
    mov edi,2468ACE0h
    mov ebp,31415926h
    mov g_before,esp
    call eax
    mov g_after,esp
    mov g_bx,ebx
    mov g_si,esi
    mov g_di,edi
    mov g_bp,ebp
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret 4
} }
void Write(void* dest,const void* src,size_t length){DWORD old=0,ignored=0;Require(VirtualProtect(dest,length,PAGE_EXECUTE_READWRITE,&old)!=FALSE,"Private image protection failed");std::memcpy(dest,src,length);Require(FlushInstructionCache(GetCurrentProcess(),dest,length)!=FALSE,"Private image flush failed");Require(VirtualProtect(dest,length,old,&ignored)!=FALSE,"Private image protection restore failed");}
void Jump(DWORD address,const void* function){unsigned char bytes[5]{0xE9};const auto delta=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(function)-reinterpret_cast<uintptr_t>(g_image+address-0x400000+5));std::memcpy(bytes+1,&delta,4);Write(g_image+address-0x400000,bytes,5);}
void InitializeNativeSizer() {
    // Execute the original native recursive natural-size pass. Only its six
    // float constant operands are rebased; property storage uses the fixture.
    static bool rebased=false;
    if(!rebased){Require(!std::memcmp(g_image+0x183E60,"\x83\xEC\x28\x53\x55\x56\x8B\xF1",8),"Native natural-size signature differs");
        for(DWORD va:{0x583FECu,0x584081u,0x584235u,0x584283u,0x5846A2u,0x584732u}){auto* op=g_image+va-0x400000;Require(op[0]==0xD8&&op[1]==0x1D,"Native size float opcode differs");DWORD target=Get<DWORD>(op,2);Require(target==0x746468||target==0x7464B8,"Native size float constant differs");target=static_cast<DWORD>(reinterpret_cast<uintptr_t>(g_image+target-0x400000));Write(op+2,&target,4);}rebased=true;}
}
void MeasureAudioLayout(Element* root) {
    InitializeNativeSizer();
    Element* pane=Find(root,nullptr,22);Element* body=Find(root,nullptr,30);
    const int before=Get<int>(body,0xF0);int extent[2]{};
    g_measure=true;reinterpret_cast<int* (__thiscall*)(Element*,int*)>(g_image+0x183E60)(pane,extent);g_measure=false;
    if(before==0){++g_audioZeroLayouts;Require(Get<int>(body,0x110)>0&&Get<int>(body,0xF0)==0,"Native auto-height pass must reproduce measured extent without F0 commit");}
    else{++g_audioCommittedLayouts;Require(before==Get<int>(body,0x110)&&!Get<unsigned char>(body,0x115),"Second Audio layout did not use a definite native measured extent");}
    // The native ScrollPane applies its measured pane extent separately. The
    // moved ordinary block above deliberately retains the original F0 bug.
    Put(pane,0xF0,extent[1]);
}
void AudioClipping(Element* root,Element* body) {
    const auto propagate=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x186B00);
    const auto clip=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x186C80);
    Node global{};Element* viewport=reinterpret_cast<Element*>(global.object);
    Put<char>(viewport,0,'X');Put<unsigned char>(viewport,0x75,1);Put(viewport,0x1C,global.children);Put(viewport,0x20,global.children+1);global.children[0]=root;
    const int vw=Get<int>(g_world,0x78),vh=Get<int>(g_world,0x7C),w=Get<int>(root,0xEC),h=Get<int>(root,0xF0);
    Put(viewport,0x128,-vw/2);Put(viewport,0x12C,vw/2);Put(viewport,0x130,-vh/2);Put(viewport,0x134,vh/2);
    const int rootX=Get<int>(root,0xDC),rootY=Get<int>(root,0xE0);Element* parent=ModernUi::Parent(root);
    Put(root,0x28,viewport);Put(root,0xDC,-w/2);Put(root,0xE0,h/2);
    Element* scroller=Find(root,nullptr,21);Put(scroller,0xDC,0);Put(scroller,0xE0,-60);
    Element* pane=Find(root,nullptr,22);Put(pane,0xDC,0);Put(pane,0xE0,0);Put(body,0xDC,0);Put(body,0xE0,0);
    ModernUi::Children rows(body);for(size_t i=0;i<rows.Count();++i){Put(rows.first[i],0xDC,0);Put(rows.first[i],0xE0,-static_cast<int>(i)*20);}
    const auto clipTree=[&](auto&& self,Element* element)->void{clip(element);ModernUi::Children children(element);for(size_t i=0;i<children.Count();++i)self(self,children.first[i]);};
    Require(!std::memcmp(g_image+0x1871B0,"\x55\x8B\xEC\x83\xE4\xF8\x81\xEC\x98\x00\x00\x00",12),"Native clip/cull updater signature differs");
    Require(!std::memcmp(g_image+0x42B10,"\x8A\x44\x24\x04\x84\xC0\x74\x07",8),"Native app-cull setter signature differs");
    alignas(4) unsigned char visual[0xA0]{};void* table[0x28]{};table[0x50/4]=g_image+0x42B10;Put(visual,0,table);Put<unsigned short>(visual,0x14,12);
    Element* probe=rows.first[0];void* previousScene=Get<void*>(probe,0x88);Put(probe,0x88,visual);
    const auto updateClip=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x1871B0);
    const int committed=Get<int>(body,0xF0);Put(body,0xF0,0);propagate(root);clipTree(clipTree,root);
    for(size_t i=0;i<rows.Count();++i)Require(Get<int>(rows.first[i],0x134)==Get<int>(rows.first[i],0x130),"Captured zero-F0 Audio body must clip every retained row");
    Require(Get<unsigned char>(probe,0x7A)==1,"Collapsed native clip did not mark clipDirty");updateClip(probe);Require(Get<unsigned short>(visual,0x14)==13,"Original native clip updater did not app-cull zero-height Audio");
    Put<unsigned char>(probe,0x7A,0);Put(body,0xF0,committed);propagate(root);clipTree(clipTree,root);
    Require(Get<unsigned char>(probe,0x7A)==1,"Committed extent did not mark native clipDirty for reflow");updateClip(probe);Require(Get<unsigned short>(visual,0x14)==12,"Original native clip updater did not reveal formerly culled Audio");
    Put(probe,0x88,previousScene);
    Require(Get<int>(body,0x134)-Get<int>(body,0x130)==committed,"Committed Audio extent is still clipped");
    for(size_t i=0;i<rows.Count();++i)Require(Get<int>(rows.first[i],0x134)-Get<int>(rows.first[i],0x130)==20,"Committed Audio extent still clips a retained row");
    Put(root,0x28,parent);Put(root,0xDC,rootX);Put(root,0xE0,rootY);++g_audioClipCases;
}
void NaturalHeight(Element* root) {
    if(g_tab!=0&&g_tab!=3)return;
    InitializeNativeSizer();
    const auto measure=reinterpret_cast<int* (__thiscall*)(Element*,int*)>(g_image+0x183E60);
    Element* pane=Find(root,nullptr,22);Element* child=Find(root,nullptr,g_tab==0?30:31);
    Require(child&&ModernUi::Parent(child)==pane,"Measured body was not moved into new auto-height pane");
    const float relative=Get<float>(child,0x11C);const unsigned char automatic=Get<unsigned char>(child,0x115);
    const int height=Get<int>(child,0xF0),minimum=Get<int>(child,0x100),maximum=Get<int>(child,0x108);
    Put(child,0x11C,1.0f);Put<unsigned char>(child,0x115,0);Put(child,0xF0,0);Put(child,0x100,0);Put(child,0x108,-1);
    int oldSize[2]{};g_measure=true;measure(child,oldSize);g_measure=false;
    Require(oldSize[1]==0,"Original relative-height body no longer reproduces captured height zero");
    Put(child,0x11C,relative);Put(child,0x115,automatic);Put(child,0xF0,height);Put(child,0x100,minimum);Put(child,0x108,maximum);
    int newSize[2]{},paneSize[2]{};g_measure=true;measure(child,newSize);measure(pane,paneSize);g_measure=false;
    if(!(newSize[1]>0&&paneSize[1]>=newSize[1]))std::fprintf(stderr,"natural tab=%u old=%d new=%d pane=%d rel=%g auto=%u h=%d min=%d max=%d\n",g_tab,oldSize[1],newSize[1],paneSize[1],relative,unsigned(automatic),height,minimum,maximum);
    Require(newSize[1]>0&&paneSize[1]>=newSize[1],"Native natural-size pass still collapses body/list");
    if(g_tab==0)Require(relative<0&&!automatic&&newSize[1]==240&&Get<int>(child,0xF0)==240&&minimum==240&&maximum==240,"Audio must commit the measured extent to final F0 and fixed bounds");
    else {const int expected=(std::max)(180,(std::min)(360,Get<int>(Find(root,nullptr,21),0xF0)*3/5));Require(relative<0&&!automatic&&newSize[1]==expected,"Controls native list viewport is not bounded and definite");for(int i=0;i<34;++i)Require(Get<DWORD>(Find(child,nullptr,600+i),0x170)==0xB17D0000u+i,"Native keybinding row/callback marker lost");}
    if(g_tab==0)AudioClipping(root,child);
    ++g_measureCases;
}
void NativeGeometry() {
    Write(g_image+0x187050,g_originalScene,5);
    // Execute original native screen propagation, clipping and scene translation.
    // Font measurements (OK 30x24, 12/-11 inset) are from the v8 live trees.
    // Complete layout still uses the ABI model above; these functions verify
    // native clipping/scene behaviour rather than mirroring that model.
    Require(!std::memcmp(g_image+0x186B00,"\x56\x8B\xF1\x8A\x46\x75",6),"Native screen propagation signature differs");
    Require(!std::memcmp(g_image+0x186C80,"\x83\xEC\x18\x55\x56\x8B\xF1",7),"Native clip signature differs");
    Require(!std::memcmp(g_image+0x187050,"\x8A\x41\x75\x83\xEC\x18",6),"Native scene translation signature differs");
    Require(!std::memcmp(g_image+0x1870E0,"\xD9\x05\x68\x64\x74\x00",6),"Native scene float operand differs");
    const DWORD zero=static_cast<DWORD>(reinterpret_cast<uintptr_t>(g_image+0x346468));
    Write(g_image+0x1870E2,&zero,4);
    const auto propagate=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x186B00);
    const auto clip=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x186C80);
    const auto scene=reinterpret_cast<void (__thiscall*)(Element*)>(g_image+0x187050);
    constexpr int tabWidth[]={76,83,111,85},tabOffset[]={0,76,159,270};
    for(unsigned tab=0;tab<4;++tab) for(auto viewport:{std::pair<int,int>{640,480},{800,600},{960,540},{1440,900},{1920,1080}}) {
        // global -> menu -> content -> header/tab and footer/OK/text. Native
        // node-less containers and additional scene-owning containers are both
        // exercised, so the immediate logical parent need not own the NiNode.
        Node model[9]{};alignas(4) unsigned char nodes[9][0xA0]{};Element* e[9]{};
        for(size_t i=0;i<9;++i) {
            e[i]=reinterpret_cast<Element*>(model[i].object);Put<char>(e[i],0,'X');Put<unsigned char>(e[i],0x75,1);
            Put(e[i],0x1C,model[i].children);Put(e[i],0x20,model[i].children);
        }
        auto attach=[&](int child,int parent){Put(e[child],0x28,e[parent]);auto* end=Get<Element**>(e[parent],0x20);*end=e[child];Put(e[parent],0x20,end+1);};
        attach(1,0);attach(2,1);attach(3,2);attach(4,3);attach(5,2);attach(6,5);attach(7,6);attach(8,4);
        const int width=(std::min)(760,viewport.first-64),height=(std::min)(720,viewport.second-64),iw=width-48,ih=height-48;
        auto rect=[&](int i,int x,int y,int w,int h){Put(e[i],0xDC,x);Put(e[i],0xE0,y);Put(e[i],0xEC,w);Put(e[i],0xF0,h);Put(e[i],0x9C,0);Put(e[i],0xA0,w);Put(e[i],0xA4,-h);Put(e[i],0xA8,0);};
        rect(0,0,0,viewport.first,viewport.second);rect(1,-width/2,height/2,width,height);rect(2,24,-24,iw,ih);
        rect(3,0,0,iw,48);rect(4,(iw-355)/2+tabOffset[tab],0,tabWidth[tab],44);
        rect(5,0,-(ih-52),iw,52);rect(6,iw-124,-4,120,44);rect(7,12,-11,30,24);rect(8,12,-11,tabWidth[tab]-24,24);
        for(int i:{4,6}){Put(e[i],0xC8,6);Put(e[i],0xCC,12);Put(e[i],0xD0,12);}
        for(int i:{7,8}){Put(e[i],0xA4,0);Put(e[i],0xA8,16);}
        Put(e[0],0x128,-viewport.first/2);Put(e[0],0x12C,viewport.first/2);Put(e[0],0x130,-viewport.second/2);Put(e[0],0x134,viewport.second/2);
        for(int i:{0,1,4,6,7,8})Put(e[i],0x88,nodes[i]);
        if(tab!=1)Put(e[2],0x88,nodes[2]);if(tab==2)Put(e[3],0x88,nodes[3]);if(tab==3)Put(e[5],0x88,nodes[5]);
        for(int i=1;i<9;++i)if(Get<void*>(e[i],0x88)){Element* owner=ModernUi::Parent(e[i]);while(owner&&!Get<void*>(owner,0x88))owner=ModernUi::Parent(owner);Put(nodes[i],0x18,Get<void*>(owner,0x88));}
        auto layoutGeometry=[&](){propagate(e[1]);for(size_t i=1;i<9;++i)clip(e[i]);for(int i=1;i<9;++i)if(Get<void*>(e[i],0x88))scene(e[i]);};
        layoutGeometry();
        auto world=[&](unsigned char* node,size_t component){float result=0;for(int depth=0;node&&depth<12;++depth){result+=Get<float>(node,component);node=Get<unsigned char*>(node,0x18);}return result;};
        for(int i:{3,5,7,8}) {
            const int x=Get<int>(e[i],0xE4),y=Get<int>(e[i],0xE8),w=Get<int>(e[i],0xEC),h=Get<int>(e[i],0xF0);
            Require(Get<int>(e[i],0x128)==x&&Get<int>(e[i],0x12C)==x+w&&Get<int>(e[i],0x130)==y-h&&Get<int>(e[i],0x134)==y,"Original native clipping truncates header/footer/button text");
        }
        for(int i:{4,6}) {
            const int x=Get<int>(e[i],0xE4),y=Get<int>(e[i],0xE8),w=Get<int>(e[i],0xEC);
            Require(Get<int>(e[i],0x128)==x+12&&Get<int>(e[i],0x12C)==x+w-12&&Get<int>(e[i],0x130)==y-38&&Get<int>(e[i],0x134)==y-6,"Native button padding or footer clipping changed");
        }
        for(int i:{4,6,7,8})Require(world(nodes[i],0x30)==float(Get<int>(e[i],0xE4))&&world(nodes[i],0x38)==float(Get<int>(e[i],0xE8)),"Scene ancestry does not match clipped button/text coordinates");
        if(tab==2){Put(e[6],0xE0,-42);layoutGeometry();Require(Get<int>(e[7],0x130)==Get<int>(e[7],0x134),"Captured Video OK text clipping no longer reproduces");Put(e[6],0xE0,-4);layoutGeometry();Require(Get<int>(e[7],0x134)-Get<int>(e[7],0x130)==24,"Fixed Video OK text remains clipped");}
        // Captured early-layout defect: native position updates alone cannot
        // repair a scene node attached globally instead of below its menu.
        void* expectedParent=Get<void*>(nodes[4],0x18);Put(nodes[4],0x18,nodes[0]);scene(e[4]);
        Require(Get<unsigned char*>(nodes[4],0x18)==nodes[0]&&world(nodes[4],0x38)!=float(Get<int>(e[4],0xE8)),"Native regression no longer reproduces stale scene ancestry");
        Put(nodes[4],0x18,expectedParent);
        Require(world(nodes[4],0x30)==float(Get<int>(e[4],0xE4))&&world(nodes[4],0x38)==float(Get<int>(e[4],0xE8)),"Parent repair alone does not restore native logical scene coordinates");
    }
}
void CheckTree(Element* root,std::unordered_set<Element*>& visited){Require(visited.insert(root).second,"Duplicate UI ownership/cycle");ModernUi::Children children(root);for(size_t i=0;i<children.Count();++i){Require(ModernUi::Parent(children.first[i])==root,"Incorrect parent after reflow");CheckTree(children.first[i],visited);}}
}
bool ApplyVideoModeControls(HMODULE image,Element* root,Element* parent,int width){Require(reinterpret_cast<unsigned char*>(image)==g_image&&root==g_root&&Get<short>(parent,0x14)==50&&width==Get<int>(root,0xEC)-48,"Video mode row receives wrong parent/width");++g_modeCalls;Element* row=Find(root,nullptr,23);if(!row)row=New(parent,23);ModernUi::FixedSize(row,width,VideoModeControlsHeight);Put(row,0x120,-1.0f);Put(row,0x124,-1.0f);SetLastError(0xBAD);return true;}
void ApplyMenuTheme(HMODULE image,Element* root){Require(reinterpret_cast<unsigned char*>(image)==g_image&&root==g_root,"Theme receives wrong image/root");++g_theme;SetLastError(0xBAD);}
int wmain(int argc,wchar_t** argv){HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;void* view=nullptr;try{
    Require(argc==2,"Pass original Morrowind.exe path");file=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);Require(file!=INVALID_HANDLE_VALUE,"Cannot open game");mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);Require(mapping!=nullptr,"Cannot map image");view=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);Require(view!=nullptr,"Cannot view image");g_image=static_cast<unsigned char*>(view);
    Require(!InstallOptionsLayoutHooks(nullptr),"Null image accepted");Require(!InstallOptionsLayoutHooks(reinterpret_cast<HMODULE>(0x30)),"Unreadable image accepted");
    for(DWORD address:{0x5A1A9Au,0x583E60u,0x5844FFu,0x588980u,0x58AD30u,0x595370u,0x5A1322u,0x5BBC52u,0x601CFAu,0x637F06u,0x637F02u,0x637F0Bu,0x63856Au,0x638566u,0x63856Fu,0x5BC3FAu,0x5BC36Cu,0x5BC384u,0x581F30u,0x57B850u,0x57B4A0u,0x4228A0u,0x58C3D0u,0x40A880u,0x587050u,0x6EB000u,0x588140u,0x58DF10u,0x582DE0u,0x583B70u,0x581440u,0x648FD0u,0x5A10D0u,0x600FD0u,0x6372A0u,0x5BB9F0u,0x588340u,0x588BE0u}){auto* p=g_image+address-0x400000;unsigned char before=*p,bad=before^0x55;Write(p,&bad,1);std::array<std::array<unsigned char,5>,5> calls{};for(size_t i=0;i<5;++i)std::memcpy(calls[i].data(),g_image+kCalls[i]-0x400000,5);Require(!InstallOptionsLayoutHooks(reinterpret_cast<HMODULE>(g_image)),"Corrupt signature accepted");for(size_t i=0;i<5;++i)Require(!std::memcmp(calls[i].data(),g_image+kCalls[i]-0x400000,5),"Rejected image was partially patched");Write(p,&before,1);}
    Require(InstallOptionsLayoutHooks(reinterpret_cast<HMODULE>(g_image)),OptionsLayoutHookStatus());Require(InstallOptionsLayoutHooks(reinterpret_cast<HMODULE>(g_image)),"Idempotent install failed");
    for(DWORD address:kCtors)Jump(address,reinterpret_cast<const void*>(&Constructor));Jump(0x58DF10,reinterpret_cast<const void*>(&Id));Jump(0x582DE0,reinterpret_cast<const void*>(&Find));Jump(0x583B70,reinterpret_cast<const void*>(&Layout));Jump(0x581F30,reinterpret_cast<const void*>(&SetProperty));Jump(0x581440,reinterpret_cast<const void*>(&GetProperty));Jump(0x588140,reinterpret_cast<const void*>(&Widget));Jump(0x57B4A0,reinterpret_cast<const void*>(&Remove));Jump(0x4228A0,reinterpret_cast<const void*>(&Insert));
    std::memcpy(g_originalScene,g_image+0x187050,5);Jump(0x587050,reinterpret_cast<const void*>(&RefreshScene));Jump(0x6EB000,reinterpret_cast<const void*>(&RefreshWorld));g_sceneTable[0x98/4]=reinterpret_cast<void*>(&DetachScene);g_sceneTable[0x94/4]=reinterpret_cast<void*>(&AttachScene);Jump(0x58C3D0,reinterpret_cast<const void*>(&CompactScene));Jump(0x40A880,reinterpret_cast<const void*>(&ReleaseScene));
    void* world=g_world;Write(g_image+0x3C67DC,&world,sizeof(world));short contentId=50,controlsListId=31;Write(g_image+0x3D7E50,&contentId,2);Write(g_image+0x3D3360,&controlsListId,2);const unsigned char resizeReturn=0xC3;Write(g_image+0x23856F,&resizeReturn,1);
    for(g_tab=0;g_tab<4;++g_tab){short menu=static_cast<short>(40+g_tab),ok=static_cast<short>(60+g_tab);Write(g_image+kMenus[g_tab]-0x400000,&menu,2);Write(g_image+kOks[g_tab]-0x400000,&ok,2);const unsigned char ret=0xC3;Write(g_image+kCalls[g_tab]-0x400000+(g_tab==2?5:8),&ret,1);
        for(auto size:{std::pair<int,int>{640,480},{800,600},{1440,900},{1920,1080}}){Put(g_world,0x78,size.first);Put(g_world,0x7C,size.second);g_root=reinterpret_cast<Element*>(g_nodes[0].object);SetLastError(kIncoming);if(g_tab==2){Constructor(g_root,g_extra);Element* oldOk=Find(g_root,nullptr,60+g_tab);Put(oldOk,0xE0,-42);SetLastError(kIncoming);Require(InvokeVideo(g_image+kCalls[g_tab]-0x400000-4)==g_root,"Video native layout return value changed");}else Invoke(g_image+kCalls[g_tab]-0x400000-1);Require(GetLastError()==kOutgoing&&!g_bad,"Original stack arguments/LastError or UI API arguments changed");Require(g_before==g_after&&(g_tab==2?(g_bx==reinterpret_cast<uintptr_t>(g_root)&&g_si==0x11223344):(g_bx==0x13579BDF&&g_si==reinterpret_cast<uintptr_t>(g_root)))&&g_di==0x2468ACE0&&g_bp==0x31415926,"Stack/nonvolatile registers changed");const int w=Get<int>(g_root,0xEC),h=Get<int>(g_root,0xF0);Require(w<=size.first-64&&h<=size.second-64,"Panel exceeds logical viewport");Require(Get<int>(g_root,0xDC)==0&&Get<int>(g_root,0xE0)==0&&Get<float>(g_root,0x120)==0.5f&&Get<float>(g_root,0x124)==0.5f,"Native centered coordinates changed");Element* content=Find(g_root,nullptr,50);Require(Get<int>(content,0xDC)==24&&Get<int>(content,0xE0)==-24,"Options inset must translate downward to native negative Y");ModernUi::Children order(content);Require(order.Count()==(g_tab==2?4u:3u)&&Get<short>(order.first[0],0x14)==10&&Get<short>(order.first[g_tab==2?2:1],0x14)==21&&Get<short>(order.last[-1],0x14)==11,"Header/mode/body/footer order wrong");if(g_tab==2)Require(Get<short>(order.first[1],0x14)==23&&Get<int>(order.first[1],0xF0)==VideoModeControlsHeight,"Video modes must remain between tabs and scroller");Element* pane=Find(g_root,nullptr,22);Require(ModernUi::Children(pane).Count()==(g_tab==0?1u:g_tab==3?14u:12u),"Body controls missing from scroller");Element* button=Last(g_root,60+g_tab);if(g_tab==3)Require(Get<DWORD>(Find(pane,nullptr,63),0x170)==0xCAFECAFE,"Controls Reset/OK duplicate ID selected wrong action");Require(Get<DWORD>(button,0x170)==0xCA11BAC0&&Get<int>(button,0x100)==44&&Get<int>(button,0xEC)==120&&Get<int>(button,0xDC)==w-48-124&&Get<int>(button,0xE0)==-4,"OK callback marker lost/button too short");for(int i=0;i<12;++i)Require(Get<DWORD>(Find(g_root,nullptr,300+i),0x170)==0xFEED0000u+i,"Native control marker lost");NaturalHeight(g_root);std::unordered_set<Element*> seen;CheckTree(g_root,seen);Require(seen.size()==g_used,"Detached/orphan UI elements");}
        if(g_tab==2) {
            const unsigned priorUsed=g_used,priorCtor=g_ctor,priorTheme=g_theme;
            Element* originalRoot=g_root;Element* originalOk=Find(g_root,nullptr,62);
            for(auto viewport:{std::pair<int,int>{960,540},{640,480},{1440,900},{1920,1080}}) for(int repeat=0;repeat<2;++repeat) {
                Put(g_world,0x78,viewport.first);Put(g_world,0x7C,viewport.second);SetLastError(kIncoming);
                Require(InvokeResize(g_image+0x238566)==originalRoot,"Retained-root resize changed native return");
                Require(GetLastError()==kOutgoing&&!g_bad&&g_before==g_after&&g_bx==0x13579BDF&&g_si==reinterpret_cast<uintptr_t>(originalRoot)&&g_di==0x2468ACE0&&g_bp==0x31415926,"Resize CALL ABI changed");
                Require(g_used==priorUsed&&g_ctor==priorCtor&&g_theme==priorTheme&&Find(g_root,nullptr,62)==originalOk,"Resize created/replaced native controls");
                const int w=(std::min)(760,viewport.first-64),h=(std::min)(720,viewport.second-64);
                Require(Get<int>(g_root,0xEC)==w&&Get<int>(g_root,0xF0)==h,"Retained panel did not adapt to logical viewport");
                Require(Get<int>(Find(g_root,nullptr,21),0xF0)==h-48-48-52-24-VideoModeControlsHeight,"Retained scroll body height wrong");
                Require(Get<int>(originalOk,0xDC)==w-48-124&&Get<int>(originalOk,0xE0)==-4&&Get<DWORD>(originalOk,0x170)==0xCA11BAC0,"Retained OK moved outside footer/lost callback");
                std::unordered_set<Element*> seen;CheckTree(g_root,seen);Require(seen.size()==g_used,"Resize orphaned native controls");
            }
        }
    }
    Require(g_ctor==16&&g_theme==16&&g_layoutCalls==28&&g_syncCalls==24&&g_sceneRefreshCalls==24&&g_worldRefreshCalls==24&&g_measureCases==8&&g_modeCalls==12&&g_audioZeroLayouts==4&&g_audioCommittedLayouts==4&&g_audioClipCases==4,"Construction/theme invocation count changed");NativeGeometry();std::printf("PASS: 24 actual patched calls (3 constructor ABIs, first Video root-layout ABI and retained-root resize ABI) across 4 tabs x 4 logical viewports; retained second stack slot, ESP/registers/LastError, bounded geometry/native upward Y, sticky OK, native control markers and unique tree ownership; 38 signature guards rejected before mutation; 20 original native screen/clip/scene button-and-text cases and captured bad-parent reproduction; 8 original native natural-size height regressions for Audio/Controls; 4 committed-F0 Audio clipping/cull regressions through original5871B0/442B10.\n");UnmapViewOfFile(view);CloseHandle(mapping);CloseHandle(file);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());if(view)UnmapViewOfFile(view);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return 1;}}
