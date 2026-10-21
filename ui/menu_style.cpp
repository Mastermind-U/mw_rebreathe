#include "menu_style.h"
#include <cstdio>

namespace {
using namespace ModernUi;
Bridge g_ui;
Element* g_main=nullptr;
char g_status[256]="Native menu style has not run.";
void (*g_log)(const char*)=nullptr;
constexpr int kLeave=-0x7FCE,kOver=-0x7FCD,kDown=-0x7FCC;
struct Binding { Element* button=nullptr; Element* label=nullptr; Element* face=nullptr; Callback previous[3]{}; };
Binding g_buttons[7];
struct Action { DWORD id; const char* name; const char* key; };
constexpr Action kActions[]={
    {0x7D4EF8,"Return to game","Return"}, {0x7D4F3A,"New game","New"},
    {0x7D4F8C,"Load game","Load"}, {0x7D4ED8,"Save game","Save"},
    {0x7D4F44,"Options","Options"}, {0x7D4F0A,"Credits","Credits"},
    {0x7D4EBC,"Exit game","Exit"},
};
Element* Rule(Bridge ui,Element* parent,const char* name,int x,int y,int width,int height,Color color) {
    Element* rect=ui.Rect(parent,ui.Id(name)); if (!rect) return nullptr;
    // FixedFrame/ScrollPane constructors redirect new children to content.
    // Decorations belong to the requested outer element, not that content.
    if(Parent(rect)!=parent && !ui.MoveTo(rect,parent)) return nullptr;
    NoSpacing(rect); Position(rect,x,y); FixedSize(rect,width,height); Tint(rect,color);
    Put<unsigned char>(rect,0x7F,0);
    return rect;
}
void TintLabels(Element* parent,unsigned depth=0) {
    if (!parent || depth>24) return;
    if (Get<int>(parent,0x160)==-0x7F4A) Tint(parent,Parchment); // Native text content type 0x80B6.
    Children children(parent);
    for(size_t i=0;i<children.Count();++i) TintLabels(children.first[i],depth+1);
}
void HideOriginalArtwork(Element* button,const Binding& binding) {
    // Bitmap content owns a NiNode whose intrinsic geometry survives zero UI
    // dimensions. Native visibility drives its app-culling flag on layout.
    // Keep all original elements and IDs for the game's state callbacks.
    Children children(button);
    for(size_t i=0;i<children.Count();++i) {
        Element* child=children.first[i];
        if(child==binding.label || child==binding.face) continue;
        g_ui.Visible(child,false);
        Put<unsigned char>(child,0x7F,0);
    }
}
bool __cdecl StyleEvent(Element* element,int eventId,int data0,int data1,Element* source) {
    for(auto& binding:g_buttons) if(binding.button==element) {
        const int slot=eventId==kLeave?0:eventId==kOver?1:2;
        const bool result=binding.previous[slot] ? binding.previous[slot](element,eventId,data0,data1,source) : false;
        Element* current=g_ui.FindMenu(g_ui.GlobalId(0x7D4F10));
        // A previous callback may close the menu. Resolve ownership afresh
        // before touching any retained visual pointer.
        if(!current || current!=g_main || !g_ui.Find(current,g_ui.Id("MWModern_Menu_Panel")) ||
            g_ui.FindGlobal(current,kActions[&binding-g_buttons].id)!=element) return result;
        // Native mouse handlers may re-show idle/over/pressed bitmap states.
        HideOriginalArtwork(element,binding);
        const bool disabled=g_ui.Property(element,-0x7F94,1)!=0;
        if(binding.label) Tint(binding.label,disabled?Muted:eventId==kLeave?Parchment:Hover);
        if(binding.face) Tint(binding.face,eventId==kDown?Rgb(63,51,33):eventId==kOver?Rgb(49,43,31):Secondary);
        g_ui.Layout(current);
        const DWORD layoutError=GetLastError();
        g_ui.SyncVisuals(current);
        SetLastError(layoutError);
        return result;
    }
    return false;
}
void StyleButton(Element* button,size_t index,int width,int height) {
    auto& binding=g_buttons[index];
    char name[96];
    sprintf_s(name,"MWModern_Main_%s_Label",kActions[index].key);
    const int labelId=g_ui.Id(name);
    const bool fresh=g_ui.Find(button,labelId)==nullptr;
    if(fresh) {
        // Original bitmap IDs remain available to native hover handlers.
        // Collapse their layout space; HideOriginalArtwork also hides NiNodes.
        Children original(button);
        for(size_t i=0;i<original.Count();++i) {
            FixedSize(original.first[i],0,0); Position(original.first[i],0,0);
            Put<unsigned char>(original.first[i],0x7F,0);
        }
    }
    binding.button=button;
    binding.label=g_ui.Label(button,labelId,kActions[index].name);
    sprintf_s(name,"MWModern_Main_%s_Face",kActions[index].key);
    binding.face=Rule(g_ui,button,name,0,0,width,height,Secondary);
    if(binding.face) SendBack(binding.face);
    if(binding.label) {
        NoSpacing(binding.label); Position(binding.label,22,(height-24)/2);
        FixedSize(binding.label,width-44,24); Put(binding.label,0x15C,0);
        Put<unsigned char>(binding.label,0x7C,1); Put<unsigned char>(binding.label,0x7F,0);
        Tint(binding.label,g_ui.Property(button,-0x7F94,1)?Muted:Parchment);
    }
    NoSpacing(button); FixedSize(button,width,height);
    HideOriginalArtwork(button,binding);
    const int events[]={kLeave,kOver,kDown};
    for(size_t i=0;i<3;++i) {
        const auto callback=reinterpret_cast<Callback>(g_ui.Property(button,events[i],0x20));
        if(callback!=StyleEvent) binding.previous[i]=callback;
        g_ui.Event(button,events[i],StyleEvent);
    }
}
void ApplyMain(Element* root) {
    if(!root || !g_ui.image) return;
    const bool fresh=root!=g_main || !g_ui.Find(root,g_ui.Id("MWModern_Menu_Panel"));
    if(fresh) {
        g_main=root; for(auto& binding:g_buttons) binding=Binding{};
    }
    void* world=Get<void*>(g_ui.image,0x3C67DC); if(!world) return;
    if(!Get<void*>(world,0x30)) return; // Native world teardown clears the renderer first.
    const int viewWidth=Get<int>(world,0x78),viewHeight=Get<int>(world,0x7C);
    if(viewWidth<640 || viewHeight<480 || viewWidth>16384 || viewHeight>16384) return;
    Element* buttons[7]{}; int visible=0;
    for(size_t i=0;i<7;++i) {
        buttons[i]=g_ui.FindGlobal(root,kActions[i].id);
        if(buttons[i] && Get<unsigned char>(buttons[i],0x75)) ++visible;
    }
    if(!visible) return;
    const int width=(std::min)(520,viewWidth-64);
    const int buttonHeight=viewHeight<600?36:44,gap=viewHeight<600?6:10;
    const int top=88,height=top+visible*(buttonHeight+gap)+26;
    const int x=viewWidth>=1000?(std::max)(40,(viewWidth-width)/6):(viewWidth-width)/2;
    // Native top-level positions are measured from screen centre; descendants
    // use the parent's top-left content origin.
    NoSpacing(root); FixedSize(root,width,height); Position(root,x-viewWidth/2,-height/2);
    ApplyMenuTheme(reinterpret_cast<HMODULE>(g_ui.image),root);
    int row=0;
    for(size_t i=0;i<7;++i) if(buttons[i]) {
        unsigned depth=0;
        for(Element* ancestor=Parent(buttons[i]);ancestor && ancestor!=root && depth++<12;ancestor=Parent(ancestor)) {
            NoSpacing(ancestor); FixedSize(ancestor,width,height); Position(ancestor,0,0);
        }
        StyleButton(buttons[i],i,width-56,buttonHeight);
        if(Get<unsigned char>(buttons[i],0x75)) Position(buttons[i],28,top+row++*(buttonHeight+gap));
    }
    Element* title=g_ui.Label(root,g_ui.Id("MWModern_Main_Title"),"MORROWIND");
    if(title) {
        NoSpacing(title); FixedSize(title,width-56,28); Position(title,28,23);
        Tint(title,Brass); Put<unsigned char>(title,0x7F,0);
    }
    Element* subtitle=g_ui.Label(root,g_ui.Id("MWModern_Main_Subtitle"),"THE ELDER SCROLLS III");
    if(subtitle) {
        NoSpacing(subtitle); FixedSize(subtitle,width-56,24); Position(subtitle,28,53);
        Tint(subtitle,Muted); Put<unsigned char>(subtitle,0x7F,0);
    }
    Rule(g_ui,root,"MWModern_Main_HeaderRule",28,80,width-56,1,Brass);
    char status[256];
    sprintf_s(status,"Native brass menu applied: actions=%d logical=%dx%d panel=%dx%d position=%d,%d.",
              visible,viewWidth,viewHeight,width,height,Get<int>(root,0xDC),Get<int>(root,0xE0));
    if(fresh || std::strcmp(g_status,status)) LogMenuStyle(status);
    strcpy_s(g_status,status);
}
Element* __fastcall MainLayout(Element* root,void*,bool timestamp) {
    ApplyMain(root); Element* result=g_ui.Layout(root,timestamp);
    const DWORD layoutError=GetLastError();
    g_ui.SyncVisuals(root); SetLastError(layoutError); return result;
}
bool Matches(const unsigned char* image,DWORD rva,const char* bytes,size_t length) { return std::memcmp(image+rva,bytes,length)==0; }
bool WriteCall(unsigned char* image,DWORD rva,const void* target,const unsigned char* restore=nullptr) {
    unsigned char code[5]={0xE8};
    const DWORD delta=DWORD(reinterpret_cast<uintptr_t>(target)-reinterpret_cast<uintptr_t>(image+rva+5));
    std::memcpy(code+1,&delta,4);
    DWORD previous=0,ignored=0;
    if(!VirtualProtect(image+rva,5,PAGE_EXECUTE_READWRITE,&previous)) return false;
    std::memcpy(image+rva,restore?restore:code,5);
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),image+rva,5)!=FALSE;
    const bool protectedAgain=VirtualProtect(image+rva,5,previous,&ignored)!=FALSE;
    return flushed&&protectedAgain;
}
}

void ApplyMenuTheme(HMODULE game,ModernUi::Element* root) {
    using namespace ModernUi;
    if(!game || !root) return;
    Bridge ui(game);
    TintLabels(root);
    const int width=Get<int>(root,0xEC),height=Get<int>(root,0xF0);
    if(width<100 || height<60 || width>16384 || height>16384) return;
    Element* back=Rule(ui,root,"MWModern_Menu_Panel",0,0,width,height,Rgb(29,27,22,0.97f));
    if(back) SendBack(back);
    Rule(ui,root,"MWModern_Menu_Top",0,0,width,1,Brass);
    Rule(ui,root,"MWModern_Menu_Bottom",0,height-1,width,1,Brass);
    Rule(ui,root,"MWModern_Menu_Left",0,0,1,height,Brass);
    Rule(ui,root,"MWModern_Menu_Right",width-1,0,1,height,Brass);
    // Small paired rules evoke the game's brass inlay without copied artwork.
    Rule(ui,root,"MWModern_Menu_InlayL",8,8,26,1,Muted);
    Rule(ui,root,"MWModern_Menu_InlayR",width-34,8,26,1,Muted);
}

bool InstallMenuStyle(HMODULE game) {
    static_assert(sizeof(void*)==4,"Native menu hooks require x86");
    auto* image=reinterpret_cast<unsigned char*>(game);
    if(!image) return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<64 || dos->e_lfanew>0x100000) return false;
    const auto* pe=reinterpret_cast<const IMAGE_NT_HEADERS32*>(image+dos->e_lfanew);
    if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386 ||
       pe->OptionalHeader.ImageBase!=0x400000 || pe->OptionalHeader.SizeOfImage<0x3D8000) return false;
    if(!Matches(image,0x1FCA28,"\xE8\x43\x71\xF8\xFF",5) ||
       !Matches(image,0x1FF250,"\xE8\x1B\x49\xF8\xFF",5) ||
       !Matches(image,0x183B70,"\x83\xEC\x08\x53\x56\x8B\xF1",7) ||
       !Matches(image,0x188340,"\x51\x57\x8B\xF9\x80\x3F\x58",7) ||
       !Matches(image,0x188BE0,"\x83\xEC\x0C\x56\x8B\xF1",6) ||
       !Matches(image,0x17F2A0,"\x8B\x15\x28\x1C\x7D\x00\x53\x55",8) ||
       !Matches(image,0x17B4A0,"\x83\xEC\x08\x53\x55\x8B\x6C\x24\x14",9) ||
       !Matches(image,0x228A0,"\x83\xEC\x08\x53\x55\x8B\x6C\x24\x18",9) ||
       !Matches(image,0x18C3D0,"\x51\x53\x8B\xD9\x8B\x4B\x10",7) ||
       !Matches(image,0xA880,"\x8B\x09\x85\xC9\x74\x0F",6) ||
       !Matches(image,0x2C8410,"\x64\xA1\x00\x00\x00\x00\x6A\xFF",8) ||
       !Matches(image,0x2C8680,"\x6A\xFF\x68\xD8\x28\x73\x00",7) ||
       !Matches(image,0x2EACC0,"\x51\x56\x8B\xF1\x8B\x4E\x18",7) ||
       !Matches(image,0x187050,"\x8A\x41\x75\x83\xEC\x18\x84\xC0",8) ||
       !Matches(image,0x2EB000,"\x8B\x54\x24\x08\x56\x8B\xF1",7)) return false;
    g_ui=Bridge(game);
    unsigned char first[5],second[5]; std::memcpy(first,image+0x1FCA28,5); std::memcpy(second,image+0x1FF250,5);
    if(!WriteCall(image,0x1FCA28,reinterpret_cast<const void*>(MainLayout)) ||
       !WriteCall(image,0x1FF250,reinterpret_cast<const void*>(MainLayout))) {
        WriteCall(image,0x1FCA28,nullptr,first); WriteCall(image,0x1FF250,nullptr,second); g_ui=Bridge(); return false;
    }
    strcpy_s(g_status,"Native brass main menu armed; original actions, visibility and disabled states retained.");
    return true;
}
const char* MenuStyleStatus() { return g_status; }
void SetMenuStyleLogCallback(void (*callback)(const char*)) { g_log=callback; }
void LogMenuStyle(const char* line) { if(g_log) { g_log(line); g_log("\r\n"); } }
