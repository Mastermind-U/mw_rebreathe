// Private PE hook/ABI and native-element-tree contract regression. No game start.
#include "menu_style.cpp"
#include <map>
#include <string>

namespace {
struct Node { alignas(4) unsigned char data[0x184]{}; Element* children[128]{}; alignas(4) unsigned char scene[0xB0]{}; void* sceneChildren[128]{}; };
Node nodes[256]; size_t used=0;
void* sceneVtable[39]{};
void* __fastcall FakeRtti(void*,void*) { return nullptr; }
unsigned worldUpdates=0;
bool geometryOnlyUpdates=true;
void ModelWorldCoordinates(void* node) {
    void* parent=Get<void*>(node,0x18);
    Put(node,0x64,Get<float>(node,0x30)+(parent?Get<float>(parent,0x64):0.0f));
    Put(node,0x68,Get<float>(node,0x34)+(parent?Get<float>(parent,0x68):0.0f));
    Put(node,0x6C,Get<float>(node,0x38)+(parent?Get<float>(parent,0x6C):0.0f));
    auto** children=Get<void**>(node,0x94); const unsigned count=Get<unsigned>(node,0x9C);
    for(unsigned i=0;i<count;++i) if(children[i]) ModelWorldCoordinates(children[i]);
}
void __fastcall FakeDownwardPass(void* node,void*,float time,bool controllers,bool bounds) {
    ++worldUpdates; geometryOnlyUpdates&=time==0.0f&&!controllers&&bounds;
    ModelWorldCoordinates(node); // Callback model; the6EB000 dispatcher itself is original code.
}
void (__thiscall* nativeArtworkUpdate)(Element*)=nullptr;
std::map<std::string,short> ids;
std::map<Element*,std::map<int,DWORD>> properties;
Element* liveRoot=nullptr;
unsigned layouts=0,visualCalls=0;
bool closeOnVisual=false;
unsigned char world[0x400]{};
void Materialize(Element* element) {
    auto* node=reinterpret_cast<Node*>(element);
    Put(element,0x88,node->scene);
}
Element* Make(Element* parent,int id) {
    if(used==256) return nullptr;
    auto& node=nodes[used++]; node=Node{};
    auto* element=reinterpret_cast<Element*>(node.data);
    Put<char>(element,0,'X'); Put<short>(element,0x14,short(id));
    Put<unsigned char>(element,0x75,1); Put<unsigned char>(element,0x7F,1);
    Put(element,0x1C,node.children); Put(element,0x20,node.children); Put(element,0x24,node.children+128);
    Put(node.scene,0,sceneVtable); Put<unsigned>(node.scene,4,1); Put<unsigned short>(node.scene,0x14,0x1A);
    Put(node.scene,0x94,node.sceneChildren); Put<unsigned>(node.scene,0x98,128); Put<unsigned>(node.scene,0xA4,1);
    if(id>=300 && id<=320) {
        Materialize(element);
    }
    if(!parent) Materialize(element);
    if(parent) {
        auto end=Get<Element**>(parent,0x20); *end=element; Put(parent,0x20,end+1); Put(element,0x28,parent);
        Put(parent,0x3C,element);
    }
    return element;
}
Element* Find(Element* element,int id) {
    if(!element) return nullptr;
    if(Get<short>(element,0x14)==short(id)) return element;
    Children children(element);
    for(size_t i=0;i<children.Count();++i) if(auto found=Find(children.first[i],id)) return found;
    return nullptr;
}
Element* Destination(Element* parent) {
    const DWORD redirect=properties[parent][-0x7F99];
    return redirect ? Find(parent,int(redirect)) : parent;
}
short __cdecl FakeId(const char* name) {
    auto found=ids.find(name); if(found!=ids.end()) return found->second;
    const short id=short(1000+ids.size()); ids[name]=id; return id;
}
Element* __cdecl FakeMenu(int) { return liveRoot; }
Element* __fastcall FakeFind(Element* parent,void*,int id) { return Find(parent,id); }
Element* __fastcall FakeRect(Element* parent,void*,int id,bool,bool) {
    Element* result=Make(Destination(parent),id); Materialize(result); return result;
}
Element* __fastcall FakeLabel(Element* parent,void*,int id,const char*,bool,bool) {
    auto* result=Make(Destination(parent),id); Materialize(result); Put(result,0x160,-0x7F4A); return result;
}
void __fastcall FakeText(Element*,void*,const char*) {}
void __fastcall FakeSetProperty(Element* parent,void*,int key,DWORD value,int) { properties[parent][key]=value; }
void* __fastcall FakeGetProperty(Element* parent,void*,DWORD* value,int key,int,const Element*,bool) {
    *value=properties[parent][key]; return value;
}
bool IsArtwork(Element* element) { const short id=Get<short>(element,0x14); return id>=300&&id<=320; }
void UpdateArtwork(Element* element) {
    if(IsArtwork(element)) { nativeArtworkUpdate(element); return; }
    Children children(element);
    for(size_t i=0;i<children.Count();++i) UpdateArtwork(children.first[i]);
}
Element* __fastcall FakeLayout(Element* parent,void*,bool) { ++layouts; UpdateArtwork(parent); return parent; }
bool __cdecl NativeVisual(Element* element,int eventId,int,int,Element*) {
    ++visualCalls;
    if(closeOnVisual) liveRoot=nullptr;
    else {
        // The original hover/down/leave callbacks re-show their bitmap state.
        Children children(element);
        const int state=eventId==kLeave?0:eventId==kOver?1:2;
        for(size_t i=0;i<children.Count();++i) if(IsArtwork(children.first[i])&&(Get<short>(children.first[i],0x14)-300)%3==state) {
            g_ui.Visible(children.first[i],true); break;
        }
    }
    return true;
}
bool ArtworkHidden(Element* button) {
    Children children(button); unsigned count=0;
    for(size_t i=0;i<children.Count();++i) if(IsArtwork(children.first[i])) {
        ++count; Element* art=children.first[i]; auto* scene=Get<void*>(art,0x88);
        if(Get<unsigned char>(art,0x75)||Get<unsigned char>(art,0x76)||!scene||!(Get<unsigned short>(scene,0x14)&1)) return false;
    }
    return count==3;
}
bool __cdecl NativeClick(Element*,int,int,int,Element*) { return true; }
bool WriteBytes(void* target,const void* bytes,size_t size) {
    DWORD old=0,ignored=0; if(!VirtualProtect(target,size,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(target,bytes,size); FlushInstructionCache(GetCurrentProcess(),target,size);
    return VirtualProtect(target,size,old,&ignored)!=FALSE;
}
bool Mock(unsigned char* image,DWORD address,const void* function) {
    unsigned char jump[5]={0xE9}; const DWORD delta=DWORD(reinterpret_cast<uintptr_t>(function)-reinterpret_cast<uintptr_t>(image+address-0x400000+5));
    std::memcpy(jump+1,&delta,4); return WriteBytes(image+address-0x400000,jump,5);
}
void* Thunk(unsigned char* image,DWORD call) {
    const unsigned char push[]={0xFF,0x74,0x24,0x04},ret[]={0xC2,0x04,0x00};
    return WriteBytes(image+call-4,push,4)&&WriteBytes(image+call+5,ret,3) ? image+call-4 : nullptr;
}
void Fixture(unsigned char* image,int width,int height,bool hideReturn) {
    used=0; properties.clear(); // Deliberately reuse the exact previous root address.
    liveRoot=Make(nullptr,100);
    Put<DWORD>(liveRoot,0x70,1); // Native Visible's already-open update epoch.
    auto* content=Make(liveRoot,101); properties[liveRoot][-0x7F99]=101;
    auto* stack=Make(content,102);
    for(size_t i=0;i<7;++i) {
        const short id=short(200+i); std::memcpy(image+kActions[i].id-0x400000,&id,2);
        Element* button=Make(stack,id);
        Put(button,0x128,0); Put(button,0x12C,520); Put(button,0x130,0); Put(button,0x134,492);
        for(int n=0;n<3;++n) Make(button,300+int(i)*3+n);
        properties[button][kLeave]=properties[button][kOver]=properties[button][kDown]=DWORD(reinterpret_cast<uintptr_t>(&NativeVisual));
        properties[button][-0x7FCB]=DWORD(reinterpret_cast<uintptr_t>(&NativeClick));
        if(i==3) properties[button][-0x7F94]=1; // disabled Save retains input state.
        if(i==0 && hideReturn) Put<unsigned char>(button,0x75,0);
    }
    Put(world,0x78,width); Put(world,0x7C,height); Put<void*>(world,0x30,reinterpret_cast<void*>(1));
    void* pointer=world; std::memcpy(image+0x3C67DC,&pointer,4);
    const short menuId=100; std::memcpy(image+0x3D4F10,&menuId,2);
}
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    HANDLE file=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    HANDLE mapping=file==INVALID_HANDLE_VALUE?nullptr:CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    auto* image=mapping?static_cast<unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    if(!image) return 3;
    bool ok=InstallMenuStyle(reinterpret_cast<HMODULE>(image));
    ok&=!InstallMenuStyle(reinterpret_cast<HMODULE>(image)); // already-changed call refuses unrelated writes.
    ok&=Mock(image,0x58DF10,&FakeId)&&Mock(image,0x595370,&FakeMenu)&&Mock(image,0x582DE0,&FakeFind)&&
        Mock(image,0x588340,&FakeRect)&&Mock(image,0x588BE0,&FakeLabel)&&Mock(image,0x58AD30,&FakeText)&&
        Mock(image,0x581F30,&FakeSetProperty)&&Mock(image,0x581440,&FakeGetProperty)&&
        Mock(image,0x583B70,&FakeLayout);
    auto initial=reinterpret_cast<Element* (__thiscall*)(Element*,bool)>(Thunk(image,0x1FCA28));
    auto refresh=reinterpret_cast<Element* (__thiscall*)(Element*,bool)>(Thunk(image,0x1FF250));
    if(!ok || !initial || !refresh) return 4;
    // Execute the original scene-position stage as well as the hooked call ABI.
    // Its only used absolute operand in this zero-child NiNode path is the
    // float-zero constant. Rebase that operand inside this private image.
    const unsigned char sceneConstant[]={0xD9,0x05,0x68,0x64,0x74,0x00};
    const DWORD constant=DWORD(reinterpret_cast<uintptr_t>(image+0x346468));
    if(std::memcmp(image+0x1870E0,sceneConstant,sizeof(sceneConstant)) ||
       !WriteBytes(image+0x1870E2,&constant,sizeof(constant))) return 5;
    const auto nativeScene=reinterpret_cast<void (__thiscall*)(Element*)>(image+0x187050);
    // Original Visible walks native UI-root globals. Rebase these operands in
    // the private image; execute the original setter and scene-culling code.
    const DWORD uiRoot=DWORD(reinterpret_cast<uintptr_t>(image+0x3D1C28));
    const DWORD helpRoot=DWORD(reinterpret_cast<uintptr_t>(image+0x3D1C74));
    const DWORD zero=0;
    if(!WriteBytes(image+0x17F2A2,&uiRoot,4)||!WriteBytes(image+0x17F2AA,&helpRoot,4)||
       !WriteBytes(image+0x3D1C28,&zero,4)||!WriteBytes(image+0x3D1C74,&zero,4)) return 6;
    nativeArtworkUpdate=reinterpret_cast<void (__thiscall*)(Element*)>(image+0x186A00);
    sceneVtable[20]=image+0x42B10; // Original NiAVObject::setAppCulled, not a mock.
    sceneVtable[1]=reinterpret_cast<void*>(&FakeRtti); // No fixture geometry has a texturing depth property.
    sceneVtable[37]=image+0x2C8410; // Original NiNode::AttachChild.
    sceneVtable[38]=image+0x2C8680; // Original NiNode::DetachChild.
    sceneVtable[24]=reinterpret_cast<void*>(&FakeDownwardPass);
    const int sizes[][2]={{640,480},{1024,768},{1440,900},{1920,1080}};
    for(const auto& size:sizes) for(int hidden=0;hidden<2;++hidden) {
        Fixture(image,size[0],size[1],hidden!=0);
        Element* root=liveRoot; initial(root,true);
        const size_t afterFirst=used; refresh(root,true); ok&=used==afterFirst;
        const int x=Get<int>(root,0xDC)+size[0]/2,y=size[1]/2-Get<int>(root,0xE0);
        const int width=Get<int>(root,0xEC),height=Get<int>(root,0xF0);
        ok&=x>=0&&y>=0&&x+width<=size[0]&&y+height<=size[1];
        auto* panel=g_ui.Find(root,g_ui.Id("MWModern_Menu_Panel"));
        ok&=panel&&Parent(panel)==root&&Get<unsigned char>(panel,0x7F)==0;
        int previousBottom=0;
        for(size_t i=0;i<7;++i) {
            auto& binding=g_buttons[i]; Element* button=binding.button;
            ok&=button&&binding.label&&binding.face;
            ok&=properties[button][-0x7FCB]==DWORD(reinterpret_cast<uintptr_t>(&NativeClick));
            ok&=Get<unsigned char>(binding.label,0x7F)==0&&Get<unsigned char>(binding.face,0x7F)==0;
            ok&=Get<unsigned char>(binding.label,0x75)==1&&Get<unsigned char>(binding.face,0x75)==1;
            ok&=binding.previous[0]==NativeVisual&&binding.previous[1]==NativeVisual&&binding.previous[2]==NativeVisual;
            ok&=ArtworkHidden(button);
            if(Get<unsigned char>(button,0x75)) {
                const int top=-Get<int>(button,0xE0),bottom=top+Get<int>(button,0xF0);
                ok&=top>=previousBottom&&bottom<=height; previousBottom=bottom;
            }
        }
        ok&=Get<unsigned char>(g_buttons[0].button,0x75)==(hidden?0:1);
        // Native E0 and the scene-node Y translation grow upwards. Position's
        // public Y argument grows downwards, including descendants and root.
        ok&=Get<int>(root,0xE0)==height/2;
        ok&=Get<int>(g_buttons[1].button,0xE0)<0;
        ok&=Get<int>(g_buttons[1].label,0xE0)<0;
        auto* bottom=g_ui.Find(root,g_ui.Id("MWModern_Menu_Bottom"));
        ok&=bottom&&Get<int>(bottom,0xE0)==1-height;
        // Native scene update accumulates only ancestors without a scene node.
        // With a node on the root, the action face is translated relative to it.
        alignas(4) unsigned char rootScene[0xA0]{},faceScene[0xA0]{};
        void* savedRoot=Get<void*>(root,0x88); void* savedFace=Get<void*>(g_buttons[1].face,0x88);
        Put(root,0x88,rootScene); Put(g_buttons[1].face,0x88,faceScene);
        nativeScene(root); nativeScene(g_buttons[1].face);
        const float sceneRootY=Get<float>(rootScene,0x38),sceneFaceY=Get<float>(faceScene,0x38);
        ok&=Get<float>(rootScene,0x30)==float(Get<int>(root,0xDC));
        ok&=sceneRootY==float(height/2)&&sceneFaceY==float(Get<int>(g_buttons[1].button,0xE0));
        ok&=sceneFaceY<0&&sceneRootY+sceneFaceY<sceneRootY;
        ok&=sceneRootY+sceneFaceY-Get<int>(g_buttons[1].button,0xF0)>=sceneRootY-height;
        Put(root,0x88,savedRoot); Put(g_buttons[1].face,0x88,savedFace);
        ok&=StyleEvent(g_buttons[1].button,kOver,0,0,g_buttons[1].button)&&visualCalls>0;
        ok&=Get<float>(g_buttons[1].label,0x14C)==Hover.red;
        ok&=ArtworkHidden(g_buttons[1].button);
        ok&=StyleEvent(g_buttons[1].button,kDown,0,0,g_buttons[1].button)&&ArtworkHidden(g_buttons[1].button);
        ok&=StyleEvent(g_buttons[1].button,kLeave,0,0,g_buttons[1].button)&&ArtworkHidden(g_buttons[1].button);
        closeOnVisual=true; g_buttons[1].label=nullptr;
        ok&=StyleEvent(g_buttons[1].button,kLeave,0,0,g_buttons[1].button)&&liveRoot==nullptr;
        closeOnVisual=false;
        std::printf("native hook ABI/tree/culling contract %dx%d actions=%d panel=%dx%d result=%s\n",size[0],size[1],7-hidden,width,height,ok?"passed":"FAILED");
    }
    Fixture(image,1440,900,false);
    Element* missing=g_ui.FindGlobal(liveRoot,kActions[1].id);
    Element* parent=Parent(missing); Children siblings(parent);
    for(size_t i=0;i<siblings.Count();++i) if(siblings.first[i]==missing) {
        std::memmove(siblings.first+i,siblings.first+i+1,(siblings.Count()-i-1)*sizeof(Element*));
        Put(parent,0x20,siblings.last-1); break;
    }
    initial(liveRoot,true); ok&=g_buttons[1].button==nullptr;
    std::puts(ok?"same-address new root with missing action clears stale binding=passed":"same-address root generation FAILED");
    // Match native parent+3C metadata: remove-last, remove-middle, sole-child,
    // and same-parent no-op all preserve the append/removal invariants.
    used=0; properties.clear();
    Element* oldParent=Make(nullptr,401); Element* newParent=Make(nullptr,402);
    Element* first=Make(oldParent,403); Element* middle=Make(oldParent,404); Element* last=Make(oldParent,405);
    ok&=g_ui.MoveTo(last,newParent)&&Get<Element*>(oldParent,0x3C)==middle&&Get<Element*>(newParent,0x3C)==last;
    ok&=Children(oldParent).Count()==2&&Children(newParent).Count()==1&&Parent(last)==newParent;
    Element* tail=Make(oldParent,406);
    ok&=g_ui.MoveTo(middle,newParent)&&Get<Element*>(oldParent,0x3C)==tail&&Get<Element*>(newParent,0x3C)==middle;
    ok&=Children(oldParent).Count()==2&&Children(oldParent).first[0]==first&&Children(oldParent).first[1]==tail;
    ok&=g_ui.MoveTo(tail,newParent)&&Get<Element*>(oldParent,0x3C)==first&&Get<Element*>(newParent,0x3C)==tail;
    ok&=g_ui.MoveTo(first,newParent)&&Get<Element*>(oldParent,0x3C)==nullptr&&Get<Element*>(newParent,0x3C)==first;
    ok&=Children(oldParent).Count()==0&&Children(newParent).Count()==4;
    ok&=g_ui.MoveTo(first,newParent)&&Children(newParent).Count()==4&&Get<Element*>(newParent,0x3C)==first;
    std::puts(ok?"native child-move last-child metadata and ownership=passed":"native child-move metadata FAILED");
    Element* direct=Make(nullptr,407); Element* native=Make(nullptr,408);
    for(Element* element:{direct,native}) {
        Put(element,0xDC,13); Put(element,0xE0,-7); Put(element,0x94,30); Put(element,0x98,-20);
    }
    Position(direct,25,19);
    const auto nativeSetX=reinterpret_cast<void (__thiscall*)(Element*,int)>(image+0x18A010);
    const auto nativeSetY=reinterpret_cast<void (__thiscall*)(Element*,int)>(image+0x18A040);
    nativeSetX(native,25); nativeSetY(native,-19);
    for(size_t offset:{size_t(0xDC),size_t(0xE0),size_t(0x94),size_t(0x98)}) ok&=Get<int>(direct,offset)==Get<int>(native,offset);
    ok&=Get<unsigned char>(direct,0x78)==Get<unsigned char>(native,0x78);
    Position(direct,25,19); // Idempotent positioning must not accumulate deltas.
    ok&=Get<int>(direct,0x94)==42&&Get<int>(direct,0x98)==-32;
    std::puts(ok?"original native position setters and cached screen coordinates=passed":"native position setter equivalence FAILED");
    used=0; properties.clear();
    Element* sceneRoot=Make(nullptr,500); Element* oldContent=Make(sceneRoot,501); Materialize(oldContent);
    Element* pane=Make(oldContent,502); // The new pane is still node-less before full layout.
    Element* row=Make(oldContent,503); Element* nested=Make(row,504);
    Element* labelA=Make(nested,505); Materialize(labelA);
    Element* labelB=Make(nested,506); Materialize(labelB);
    Element* background=Make(sceneRoot,507); Materialize(background);
    Position(sceneRoot,-720,-450); Position(oldContent,24,24); Position(pane,100,80);
    Position(row,107,89); Position(nested,5,6); Position(labelA,3,4); Position(labelB,20,4);
    nativeScene(sceneRoot);
    auto* labelANode=Get<void*>(labelA,0x88); auto* labelBNode=Get<void*>(labelB,0x88);
    auto* oldScene=Get<void*>(oldContent,0x88);
    properties[labelA][-0x7FCB]=DWORD(reinterpret_cast<uintptr_t>(&NativeClick));
    ok&=g_ui.SyncVisuals(sceneRoot)&&Get<void*>(labelANode,0x18)==oldScene;
    ok&=g_ui.MoveTo(row,pane)&&Parent(row)==pane;
    Position(row,7,9); // Pane offset cancels this change: global label XY stays unchanged.
    Put<unsigned char>(labelA,0x78,0); Put<unsigned char>(labelB,0x78,0);
    // Original57B4A0 recursively57B620 detaches materialized roots below both
    // node-less containers without destroying those roots or UI callbacks.
    ok&=Get<void*>(labelANode,0x18)==nullptr&&Get<void*>(labelBNode,0x18)==nullptr;
    ok&=Get<unsigned>(labelANode,4)==1&&Get<unsigned>(labelBNode,4)==1;
    ok&=g_ui.SyncVisuals(sceneRoot)&&Get<void*>(labelANode,0x18)==oldScene; // Early sync cannot know the future pane.
    Materialize(pane); auto* paneScene=Get<void*>(pane,0x88);
    Element* intrinsicOwner=Make(nullptr,508); auto* intrinsic=Get<void*>(intrinsicOwner,0x88);
    reinterpret_cast<void (__thiscall*)(void*,void*,bool)>(sceneVtable[37])(paneScene,intrinsic,true);
    SendBack(background); SendBack(labelB);
    const unsigned initialRootHighwater=Get<unsigned>(Get<void*>(sceneRoot,0x88),0x9C);
    const unsigned initialUpdates=worldUpdates;
    for(unsigned repeat=0;repeat<100;++repeat) ok&=g_ui.SyncVisuals(sceneRoot);
    ok&=worldUpdates-initialUpdates==100&&geometryOnlyUpdates;
    auto* sceneRootNode=Get<void*>(sceneRoot,0x88);
    auto** rootOrder=Get<void**>(sceneRootNode,0x94); auto** paneOrder=Get<void**>(paneScene,0x94);
    ok&=rootOrder[0]==Get<void*>(background,0x88)&&rootOrder[1]==oldScene;
    ok&=paneOrder[0]==intrinsic&&paneOrder[1]==labelBNode&&paneOrder[2]==labelANode;
    ok&=Get<void*>(labelANode,0x18)==paneScene&&Get<void*>(labelBNode,0x18)==paneScene;
    ok&=Get<void*>(labelA,0x88)==labelANode&&Get<void*>(labelB,0x88)==labelBNode;
    ok&=Get<unsigned>(labelANode,4)==2&&Get<unsigned>(labelBNode,4)==2&&Get<unsigned>(intrinsic,4)==2;
    ok&=Get<unsigned>(sceneRootNode,0x9C)==initialRootHighwater&&Get<unsigned>(paneScene,0x9C)==3;
    // Execute native587050 through production SyncVisuals. The new pane's
    // translation must be excluded from label-local XY and included exactly
    // once through the Ni parent chain, even when layout's dirty flag is clear.
    ok&=Get<float>(labelANode,0x30)==15.0f&&Get<float>(labelANode,0x38)==-19.0f;
    float worldX=0,worldY=0; unsigned ancestors=0;
    for(void* node=labelANode;node&&ancestors++<16;node=Get<void*>(node,0x18)) {
        worldX+=Get<float>(node,0x30); worldY+=Get<float>(node,0x38);
    }
    ok&=ancestors==4&&worldX==-581.0f&&worldY==327.0f;
    ok&=Get<float>(labelANode,0x64)==worldX&&Get<float>(labelANode,0x6C)==worldY;
    ok&=properties[labelA][-0x7FCB]==DWORD(reinterpret_cast<uintptr_t>(&NativeClick));
    std::puts(ok?"native UI remove/append, late-pane parent/world XY, intrinsic geometry and 100 scene-order passes=passed":"native late-pane scene synchronization FAILED");
    UnmapViewOfFile(image); CloseHandle(mapping); CloseHandle(file);
    std::puts(ok?"PASS: 12 native-layout hook ABI, fit, original scene transforms/setters/visibility, action, hover, lifetime, root-generation, shunt, metadata, native reparent and scene-order cases":"FAILED");
    return ok?0:1;
}
