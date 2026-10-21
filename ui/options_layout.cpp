// Adaptive native Options windows. Independently written, original callbacks retained.
#define NOMINMAX
#include "options_layout.h"
#include "native_ui.h"
#include "menu_style.h"
#include "video_modes.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace {
using ModernUi::Element; using ModernUi::Get; using ModernUi::Put;
ModernUi::Bridge g_ui;
const char* g_status = "Adaptive Options layout is not installed.";
struct Tab { DWORD call, constructor, menuId, okId; const char* name; };
constexpr Tab kTabs[] = {
    {0x5A1A9A,0x5A10D0,0x7D265C,0x7D2684,"Audio"},
    {0x601CFA,0x600FD0,0x7D522C,0x7D5118,"Preferences"},
    {0x637F06,0x6372A0,0x7D7718,0x7D76B8,"Video"},
    {0x5BC3FA,0x5BB9F0,0x7D33E8,0x7D33C8,"Controls"},
};
constexpr DWORD kVideoResizeCall=0x63856A;
constexpr int kFont = -0x7FEA, kIsPart = -0x7FA8;

bool Readable(const void* pointer,size_t length) {
    if (!pointer || !length) return false;
    const uintptr_t begin=reinterpret_cast<uintptr_t>(pointer);
    if (begin+length<begin) return false;
    uintptr_t current=begin;
    while(current<begin+length) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(current),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
        const auto end=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(end<=current) return false; current=end;
    }
    return true;
}
bool Valid(Element* element) {
    if(!Readable(element,0x184)) return false;
    const char tag=Get<char>(element,0); return tag=='X'||tag=='x';
}
std::vector<Element*> Snapshot(Element* parent) {
    ModernUi::Children children(parent);
    const size_t count=children.Count();
    if(count>256 || (count && !Readable(children.first,count*sizeof(Element*)))) return {};
    std::vector<Element*> result;
    for(size_t i=0;i<count;++i) if(Valid(children.first[i]) && ModernUi::Parent(children.first[i])==parent) result.push_back(children.first[i]);
    return result;
}
Element* LastId(Element* root,short id,unsigned depth=0) {
    if(!Valid(root)||depth>32) return nullptr;
    Element* found=Get<short>(root,0x14)==id?root:nullptr;
    for(Element* child:Snapshot(root)) if(Element* match=LastId(child,id,depth+1)) found=match;
    return found;
}
Element* Action(Element* root,const Tab& tab) {
    const short id=static_cast<short>(g_ui.GlobalId(tab.okId));
    // Controls deliberately reuses its Reset button ID for the final OK.
    // Its last constructed occurrence carries the native close callback.
    return tab.menuId==0x7D33E8?LastId(root,id):g_ui.Find(root,id);
}
Element* DirectAncestor(Element* element,Element* parent) {
    for(unsigned depth=0;Valid(element)&&depth<32;++depth) {
        Element* next=ModernUi::Parent(element);
        if(next==parent) return element;
        element=next;
    }
    return nullptr;
}
void FlowWidth(Element* element,int width) {
    Put(element,0xEC,width); Put(element,0xFC,0); Put(element,0x104,-1);
    Put(element,0x118,1.0f); Put<unsigned char>(element,0x114,0);
    Put(element,0x120,-1.0f); Put(element,0x124,-1.0f);
    Put<unsigned char>(element,0x79,1);
}
void ImproveControls(Element* element,int buttonPart,unsigned depth=0) {
    if(!Valid(element)||depth>24) return;
    const int part=static_cast<short>(g_ui.Property(element,kIsPart,0x10));
    if(part==buttonPart) {
        Put<unsigned char>(element,0x115,1);
        Put(element,0x100,44); Put(element,0x108,-1);
        Put(element,0xC8,6); Put(element,0xCC,12); Put(element,0xD0,12);
        Put<unsigned char>(element,0x79,1);
    }
    if(Get<short>(element,0x160)==static_cast<short>(-0x7F4A)) {
        Put<unsigned char>(element,0x115,1);
        Put(element,0x100,24); Put(element,0x108,-1);
        Put<unsigned char>(element,0x7C,1);
    }
    for(Element* child:Snapshot(element)) ImproveControls(child,buttonPart,depth+1);
}
void PrepareBodyHeight(Element* element,const Tab& tab,int bodyHeight) {
    if(tab.menuId==0x7D265C && Get<float>(element,0x11C)>0.0f) {
        // The native Audio body fills the old fixed-height parent. Under an
        // auto-height scroll pane that dependency collapses the body to zero.
        Put(element,0x11C,-1.0f);Put<unsigned char>(element,0x115,1);
        Put(element,0x100,0);Put(element,0x108,-1);Put<unsigned char>(element,0x79,1);
    }
    if(tab.menuId==0x7D33E8 && Get<short>(element,0x14)==g_ui.GlobalId(0x7D3360)) {
        // Keep the original key-list ScrollPane and its binding callbacks;
        // give its viewport a definite height inside our auto-height body.
        const int listHeight=(std::max)(180,(std::min)(360,bodyHeight*3/5));
        ModernUi::FixedSize(element,Get<int>(element,0xEC),listHeight);
        Put(element,0x118,1.0f);
    }
}
bool CommitAudioHeight(Element* body) {
    if(!Valid(body)||Get<int>(body,0xF0)>0) return false;
    const int measured=Get<int>(body,0x110);
    if(measured<=0||measured>16384) return false;
    // Native natural-size returns this extent but does not commit F0 when
    // auto-height is combined with inactive absolute alignment. A real root
    // layout has now measured every label/widget, so commit that exact value.
    Put(body,0x11C,-1.0f);Put<unsigned char>(body,0x115,0);
    Put(body,0xF0,measured);Put(body,0x100,measured);Put(body,0x108,measured);
    Put<unsigned char>(body,0x79,1);Put<unsigned char>(body,0x7B,1);
    return true;
}
void Record(const Tab& tab,int vw,int vh,int width,int height,int bodyHeight,size_t moved,int audioBodyHeight=0) {
    wchar_t path[32768]{};
    DWORD count=GetEnvironmentVariableW(L"MW_MODERN_LOG",path,32768);
    if(!count||count>=32768) return;
    wchar_t* name=nullptr;
    for(wchar_t* p=path;*p;++p) if(*p==L'\\'||*p==L'/') name=p+1;
    if(!name||size_t(name-path)+20>=32768) return;
    wcscpy_s(name,32768-(name-path),L"options-layout.log");
    HANDLE file=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return;
    char line[256]{};
    const int length=sprintf_s(line,"%s viewport=%dx%d panel=%dx%d scrollBody=%d moved=%u stickyFooter=1 audioBodyHeight=%d\r\n",tab.name,vw,vh,width,height,bodyHeight,static_cast<unsigned>(moved),audioBodyHeight);
    DWORD written=0; if(length>0) WriteFile(file,line,static_cast<DWORD>(length),&written,nullptr); CloseHandle(file);
}
void Adapt(Element* root,const Tab& tab,bool materialize=true) {
    if(!Valid(root)||Get<short>(root,0x14)!=g_ui.GlobalId(tab.menuId)) return;
    auto* world=Get<void*>(g_ui.image,0x3C67DC);
    if(!Readable(world,0x80)) return;
    const int vw=Get<int>(world,0x78),vh=Get<int>(world,0x7C);
    if(vw<640||vh<480||vw>16384||vh>16384) return;
    Element* content=g_ui.FindGlobal(root,0x7D7E50);
    Element* ok=Action(root,tab);
    if(!Valid(content)||!Valid(ok)) return;
    Element* footer=DirectAncestor(ok,content);
    auto original=Snapshot(content);
    if(!footer||original.size()<3) return;
    Element* tabs=nullptr;
    for(Element* child:original) if(child!=footer){ tabs=child;break; }
    if(!tabs) return;
    const int width=(std::min)(760,vw-64),height=(std::min)(720,vh-64);
    const int innerWidth=width-48,innerHeight=height-48;
    // Native fixed frames align to their parent's centre. Keep that relationship;
    // root extent now belongs to the logical UI viewport, with a 32px safe margin.
    ModernUi::FixedSize(root,width,height);
    Put(root,0x120,0.5f); Put(root,0x124,0.5f); Put(root,0xDC,0); Put(root,0xE0,0);
    ModernUi::NoSpacing(root);
    ModernUi::FixedSize(content,innerWidth,innerHeight);
    ModernUi::Position(content,24,24); ModernUi::NoSpacing(content);
    g_ui.Property(content,-0x7FA7,static_cast<DWORD>(static_cast<int>(-0x7F34)),0x10);
    ImproveControls(content,g_ui.Id("PartButton"));
    ModernUi::NoSpacing(tabs); ModernUi::NoSpacing(footer);
    FlowWidth(tabs,innerWidth); FlowWidth(footer,innerWidth);
    constexpr int headerHeight=48,footerHeight=52;
    Put<unsigned char>(tabs,0x115,0); Put<unsigned char>(footer,0x115,0);
    Put(tabs,0xF0,headerHeight); Put(footer,0xF0,footerHeight);
    Put(tabs,0x100,headerHeight); Put(footer,0x100,footerHeight);
    Put(tabs,0x108,headerHeight); Put(footer,0x108,footerHeight);
    // A native footer can retain a previous layout offset (Video used -42).
    // Anchor its action inside the reserved strip instead of inheriting it.
    ModernUi::FixedSize(ok,120,44);
    ModernUi::Position(ok,innerWidth-124,4);
    // Do not materialize either subtree here: the root has no scene node yet.
    // A subtree layout would attach its native NiNodes to the global UI scene,
    // leaving them there when the root scene is built later. One final root
    // layout creates the complete hierarchy in its correct ownership order.
    const int modeHeight=tab.menuId==0x7D7718?VideoModeControlsHeight:0;
    const int bodyHeight=(std::max)(96,innerHeight-headerHeight-footerHeight-24-modeHeight);
    Element* scroller=g_ui.Widget(content,g_ui.Id("MM_Options_scroll_body"),0x648FD0);
    if(!Valid(scroller)) return;
    Element* pane=g_ui.Find(scroller,g_ui.Id("PartScrollPane_pane"));
    if(!Valid(pane)) return;
    ModernUi::NoSpacing(scroller);
    ModernUi::FixedSize(scroller,innerWidth,bodyHeight);
    Put(scroller,0xC4,12); Put(scroller,0xC0,12);
    FlowWidth(pane,innerWidth-32);
    Put<unsigned char>(pane,0x115,1); Put(pane,0x11C,-1.0f); Put(pane,0x100,0); Put(pane,0x108,-1);
    g_ui.Property(pane,-0x7FA7,static_cast<DWORD>(static_cast<int>(-0x7F34)),0x10);
    Element* audioBody=nullptr;
    size_t moved=0;
    for(Element* child:original) if(child!=tabs&&child!=footer) {
        if(g_ui.MoveTo(child,pane)) {
            ++moved; if(Get<float>(child,0x118)>0.0f) FlowWidth(child,innerWidth-32);
            PrepareBodyHeight(child,tab,bodyHeight);
            if(tab.menuId==0x7D265C) audioBody=child;
        }
    }
    // Reorder the existing footer pointer; ownership and native callbacks stay intact.
    ModernUi::Children order(content);
    for(size_t i=0;i<order.Count();++i) if(order.first[i]==footer) {
        std::memmove(order.first+i,order.first+i+1,(order.Count()-i-1)*sizeof(Element*));
        order.last[-1]=footer; break;
    }
    if(tab.menuId==0x7D7718) {
        ApplyVideoModeControls(reinterpret_cast<HMODULE>(g_ui.image),root,content,innerWidth);
        Element* mode=g_ui.Find(root,g_ui.Id("MWModern_Video_DisplayModes"));
        ModernUi::Children children(content);size_t source=children.Count(),target=children.Count();
        for(size_t i=0;i<children.Count();++i){if(children.first[i]==mode)source=i;if(children.first[i]==scroller)target=i;}
        if(source<children.Count()&&target<source){std::memmove(children.first+target+1,children.first+target,(source-target)*sizeof(Element*));children.first[target]=mode;}
    }
    ApplyMenuTheme(reinterpret_cast<HMODULE>(g_ui.image),root);
    if(materialize) {
        g_ui.Layout(root,false);
        if(CommitAudioHeight(audioBody)) g_ui.Layout(root,false);
        g_ui.SyncVisuals(root);
    }
    Record(tab,vw,vh,width,height,bodyHeight,moved,Valid(audioBody)?Get<int>(audioBody,0xF0):0);
}
bool ResizeOnly(Element* root) {
    if(!Valid(root)||Get<short>(root,0x14)!=g_ui.GlobalId(kTabs[2].menuId)) return false;
    void* world=Get<void*>(g_ui.image,0x3C67DC);
    if(!Readable(world,0x80)) return false;
    const int vw=Get<int>(world,0x78),vh=Get<int>(world,0x7C);
    if(vw<640||vh<480||vw>16384||vh>16384) return false;
    Element* content=g_ui.FindGlobal(root,0x7D7E50);
    Element* ok=g_ui.FindGlobal(root,kTabs[2].okId);
    Element* scroller=g_ui.Find(root,g_ui.Id("MM_Options_scroll_body"));
    Element* pane=Valid(scroller)?g_ui.Find(scroller,g_ui.Id("PartScrollPane_pane")):nullptr;
    if(!Valid(content)||!Valid(ok)||!Valid(scroller)||!Valid(pane)) return false;
    Element* footer=DirectAncestor(ok,content);
    Element* tabs=nullptr;
    for(Element* child:Snapshot(content)) if(child!=footer&&child!=scroller) {tabs=child;break;}
    if(!Valid(tabs)||!Valid(footer)) return false;
    const int width=(std::min)(760,vw-64),height=(std::min)(720,vh-64);
    const int iw=width-48,ih=height-48,body=(std::max)(96,ih-48-52-24-VideoModeControlsHeight);
    ModernUi::FixedSize(root,width,height);
    Put(root,0x120,0.5f);Put(root,0x124,0.5f);Put(root,0xDC,0);Put(root,0xE0,0);
    ModernUi::FixedSize(content,iw,ih);ModernUi::Position(content,24,24);
    FlowWidth(tabs,iw);FlowWidth(footer,iw);
    ModernUi::FixedSize(scroller,iw,body);FlowWidth(pane,iw-32);
    for(Element* child:Snapshot(pane)) if(Get<float>(child,0x118)>0.0f) FlowWidth(child,iw-32);
    ModernUi::FixedSize(ok,120,44);ModernUi::Position(ok,iw-124,4);
    // Resize only existing decorations. This path never invokes a factory or
    // moves an element: the native mode-change callback retains the menu root.
    struct Rule {const char* name;int x,y,w,h;};
    const Rule rules[]={
        {"MWModern_Menu_Panel",0,0,width,height},{"MWModern_Menu_Top",0,0,width,1},
        {"MWModern_Menu_Bottom",0,height-1,width,1},{"MWModern_Menu_Left",0,0,1,height},
        {"MWModern_Menu_Right",width-1,0,1,height},{"MWModern_Menu_InlayL",8,8,26,1},
        {"MWModern_Menu_InlayR",width-34,8,26,1},
    };
    for(const Rule& rule:rules) if(Element* element=g_ui.Find(root,g_ui.Id(rule.name))) {
        ModernUi::FixedSize(element,rule.w,rule.h);ModernUi::Position(element,rule.x,rule.y);
    }
    if(g_ui.Find(root,g_ui.Id("MWModern_Video_DisplayModes")))
        ApplyVideoModeControls(reinterpret_cast<HMODULE>(g_ui.image),root,content,iw);
    Record(kTabs[2],vw,vh,width,height,body,0);
    return true;
}
Element* __fastcall VideoResizeLayout(Element* root,void*,bool timestamp) {
    const DWORD incoming=GetLastError();
    ResizeOnly(root);
    SetLastError(incoming);
    Element* result=g_ui.Layout(root,timestamp);
    const DWORD outgoing=GetLastError();
    g_ui.SyncVisuals(root);
    SetLastError(outgoing);
    return result;
}
Element* __fastcall VideoLayout(Element* root,void*,bool timestamp) {
    const DWORD incoming=GetLastError();
    // Video uniquely lays out the complete menu inside its constructor.
    // Adapt its tree immediately before that first native materialization.
    Adapt(root,kTabs[2],false);
    SetLastError(incoming);
    Element* result=g_ui.Layout(root,timestamp);
    const DWORD outgoing=GetLastError();
    g_ui.SyncVisuals(root);
    SetLastError(outgoing);
    return result;
}
template<size_t Index> void __cdecl Constructor(Element* root,DWORD retainedMenuId) {
    // The caller retains the preceding createMenu argument until ADD ESP,8.
    // Forward that existing second slot as well; native constructors use root.
    g_ui.At<void (__cdecl*)(Element*,DWORD)>(kTabs[Index].constructor)(root,retainedMenuId);
    const DWORD error=GetLastError();
    Adapt(root,kTabs[Index]);
    SetLastError(error);
}
struct Patch { unsigned char* address; unsigned char original[5],replacement[5]; DWORD protection; };
bool Write(Patch& patch,bool install) {
    DWORD old=0,ignored=0;
    if(!VirtualProtect(patch.address,5,PAGE_EXECUTE_READWRITE,&old)) return false;
    if(install) patch.protection=old;
    std::memcpy(patch.address,install?patch.replacement:patch.original,5);
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),patch.address,5)!=FALSE;
    const bool restored=VirtualProtect(patch.address,5,install?old:patch.protection,&ignored)!=FALSE;
    return flushed&&restored;
}
bool Guard(unsigned char* image) {
    if(!Readable(image,sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<sizeof(*dos)||dos->e_lfanew>0x100000||!Readable(image+dos->e_lfanew,sizeof(IMAGE_NT_HEADERS32))) return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(image+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.ImageBase!=0x400000||nt->OptionalHeader.SizeOfImage<0x3D0000) return false;
    for(size_t index=0;index<4;++index) {
        const Tab& tab=kTabs[index]; const DWORD rva=tab.call-0x400000;
        if(index==2) {
            if(!Readable(image+rva-4,13)||std::memcmp(image+rva-4,"\x6A\x01\x8B\xCB\xE8\x65\xBC\xF4\xFF\x8B\x44\x24\x3C",13)) return false;
        } else {
            if(!Readable(image+rva-1,9)||image[rva-1]!=0x56||image[rva]!=0xE8||std::memcmp(image+rva+5,"\x83\xC4\x08",3)) return false;
            int32_t delta=0; std::memcpy(&delta,image+rva+1,4);
            if(static_cast<DWORD>(tab.call+5+delta)!=tab.constructor) return false;
        }
    }
    struct Signature { DWORD address; const char* bytes; size_t length; };
    const Signature signatures[]={
        {0x583E60,"\x83\xEC\x28\x53\x55\x56\x8B\xF1",8},
        {0x5844FF,"\x89\xAE\x0C\x01\x00\x00\x89\x9E\x10\x01\x00\x00",12},
        {0x588980,"\x51\x57\x8B\xF9\x80\x3F\x58",7},
        {0x58AD30,"\x83\xEC\x08\x8A\x44\x24\x0C\x53\x55\x56",10},
        {0x595370,"\xA1\xDC\x67\x7C\x00\x85\xC0\x56",8},
        {0x5A1322,"\x89\x9E\x18\x01\x00\x00\x89\x9E\x1C\x01\x00\x00",12},
        {0x5BBC52,"\xB8\x00\x00\x80\x3F\x89\x83\x18\x01\x00\x00\x89\x83\x1C\x01\x00\x00",17},
        {0x6EB000,"\x8B\x54\x24\x08\x56\x8B\xF1\x8B\x4C\x24\x10",11},
        {0x587050,"\x8A\x41\x75\x83\xEC\x18\x84\xC0",8},
        {0x5BC36C,"\x33\xD2\x66\x8B\x15\xC8\x33\x7D\x00\x6A\x00\x68\x20\x94\x63\x00\x8B\xCE\x52\xE8",20},
        {0x5BC384,"\x6A\x20\x8B\xF0\x68\x70\xC5\x5B\x00\x68\x35\x80\xFF\xFF\x8B\xCE\xE8",17},
        {0x57B4A0,"\x83\xEC\x08\x53\x55\x8B\x6C\x24\x14",9},
        {0x4228A0,"\x83\xEC\x08\x53\x55\x8B\x6C\x24\x18",9},
        {0x58C3D0,"\x51\x53\x8B\xD9\x8B\x4B\x10\x85\xC9",9},
        {0x40A880,"\x8B\x09\x85\xC9\x74\x0F\xFF\x49\x04",9},
        {0x638566,"\x6A\x01\x8B\xCE\xE8\x01\xB6\xF4\xFF\x5F\xB0\x01\x5E",13},
        {0x581F30,"\x83\xEC\x18\x53\x55\x8B\x6C\x24\x28\x56\x57",11},
        {0x57B850,"\x8B\x54\x24\x04\x56\x89\x51\x28",8},
        {0x588140,"\x83\xEC\x1C\x56\x8B\xF1",6},
        {0x58DF10,"\x8B\x44\x24\x04\x85\xC0",6},
        {0x582DE0,"\x53\x55\x8B\xE9\x85\xED\x56\x57",8},
        {0x583B70,"\x83\xEC\x08\x53\x56\x8B\xF1",7},
        {0x581440,"\x83\xEC\x08\x53\x55\x56\x57\x8B\xF9",9},
        {0x648FD0,"\x6A\xFF\x68\x52\x06\x73\x00",7},
        {0x5A10D0,"\x53\x55\x8B\x6C\x24\x0C\x56\x57",8},
        {0x600FD0,"\x53\x55\x56\x8B\x74\x24\x10\x57",8},
        {0x6372A0,"\x64\xA1\x00\x00\x00\x00\x6A\xFF",8},
        {0x5BB9F0,"\x83\xEC\x08\x53\x55\x8B\x6C\x24\x14\x56",10},
        {0x588340,"\x51\x57\x8B\xF9\x80\x3F\x58",7},
        {0x588BE0,"\x83\xEC\x0C\x56\x8B\xF1",6},
    };
    for(const auto& s:signatures) if(!Readable(image+s.address-0x400000,s.length)||std::memcmp(image+s.address-0x400000,s.bytes,s.length)) return false;
    return true;
}
}
bool InstallOptionsLayoutHooks(HMODULE gameModule) {
    auto* image=reinterpret_cast<unsigned char*>(gameModule);
    if(g_ui.image) return g_ui.image==image;
    if(!Guard(image)) {g_status="Adaptive Options rejected unsupported native signatures.";return false;}
    const void* targets[]={&Constructor<0>,&Constructor<1>,&VideoLayout,&Constructor<3>,&VideoResizeLayout};
    const DWORD calls[]={kTabs[0].call,kTabs[1].call,kTabs[2].call,kTabs[3].call,kVideoResizeCall};
    Patch patches[5]{};
    for(size_t i=0;i<5;++i) {
        auto& p=patches[i];p.address=image+calls[i]-0x400000;
        std::memcpy(p.original,p.address,5);p.replacement[0]=0xE8;
        const auto delta=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targets[i])-reinterpret_cast<uintptr_t>(p.address+5));
        std::memcpy(p.replacement+1,&delta,4);
    }
    g_ui=ModernUi::Bridge(gameModule);
    for(size_t i=0;i<5;++i) if(!Write(patches[i],true)) {
        bool rolled=true;for(size_t j=0;j<=i;++j) if(std::memcmp(patches[j].address,patches[j].original,5)&&!Write(patches[j],false)) rolled=false;
        g_ui=ModernUi::Bridge();g_status=rolled?"Adaptive Options API failure; original CALLs restored.":"Adaptive Options rollback failed; suspended game must not resume.";return false;
    }
    g_status="Adaptive Audio/Preferences/Video/Controls: logical viewport bounds, scrolling body, fixed actions.";
    return true;
}
const char* OptionsLayoutHookStatus(){return g_status;}
