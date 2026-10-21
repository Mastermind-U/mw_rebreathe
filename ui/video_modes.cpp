#include "video_modes.h"
#include "../display/display.h"

namespace {
using namespace ModernUi;
constexpr int kClick=-0x7FCB,kOver=-0x7FCD,kLeave=-0x7FCE;
struct Choice {const char* id;const char* text;DisplayMode mode;};
constexpr Choice kChoices[]={
    {"MWModern_Video_Fullscreen","Fullscreen\n(scaled)",DisplayMode::Fullscreen},
    {"MWModern_Video_Borderless","Borderless\nfullscreen",DisplayMode::Borderless},
    {"MWModern_Video_Window","Window",DisplayMode::Window},
};
HMODULE g_image=nullptr;
void ColorButton(Bridge ui,Element* button,size_t index,bool hover) {
    const bool selected=GetDisplayMode()==kChoices[index].mode;
    const char* faces[]={"MWModern_Video_Fullscreen_Face","MWModern_Video_Borderless_Face","MWModern_Video_Window_Face"};
    const char* labels[]={"MWModern_Video_Fullscreen_Label","MWModern_Video_Borderless_Label","MWModern_Video_Window_Label"};
    if(Element* face=ui.Find(button,ui.Id(faces[index]))) Tint(face,selected?Rgb(65,53,34):hover?Rgb(49,43,31):Secondary);
    if(Element* label=ui.Find(button,ui.Id(labels[index]))) Tint(label,selected||hover?Hover:Parchment);
}
bool __cdecl ModeEvent(Element* element,int eventId,int,int,Element* source) {
    if(!g_image || (!source && !element)) return false;
    Bridge ui(g_image);
    Element* root=ui.FindMenu(ui.GlobalId(0x7D7718));
    if(!root) return false;
    // Native dispatch resolves argument 1 to the nearest is_part ancestor.
    // Our action is a plain Block; argument 5 retains the actual hit source.
    // A decoration may also be the source, so resolve its owning action.
    size_t index=3;
    Element* button=nullptr;
    Element* candidate=source?source:element;
    for(unsigned depth=0;candidate && candidate!=root && !button && depth<64;++depth,candidate=Parent(candidate)) {
        for(size_t i=0;i<3;++i) if(Get<short>(candidate,0x14)==ui.Id(kChoices[i].id) && ui.Find(root,ui.Id(kChoices[i].id))==candidate) {
            index=i;button=candidate;break;
        }
    }
    if(!button) return false;
    if(eventId==kClick) {
        SetDisplayMode(kChoices[index].mode);
        // Window messages may run while the style changes. Resolve the menu
        // again instead of retaining action pointers across that operation.
        root=ui.FindMenu(ui.GlobalId(0x7D7718));if(!root) return true;
        for(size_t i=0;i<3;++i) if(Element* action=ui.Find(root,ui.Id(kChoices[i].id))) ColorButton(ui,action,i,false);
    } else ColorButton(ui,button,index,eventId==kOver);
    ui.Layout(root,true);ui.SyncVisuals(root);
    return true;
}
}

bool ApplyVideoModeControls(HMODULE game,ModernUi::Element* root,ModernUi::Element* parent,int width) {
    using namespace ModernUi;
    if(!game || !root || !parent || width<300 || width>16384) return false;
    Bridge ui(game);g_image=game;
    const int rowId=ui.Id("MWModern_Video_DisplayModes");
    Element* row=ui.Find(root,rowId);
    if(!row) row=ui.Block(parent,rowId);
    if(!row) return false;
    NoSpacing(row);FixedSize(row,width,VideoModeControlsHeight);
    Put(row,0x120,-1.0f);Put(row,0x124,-1.0f);
    Element* title=ui.Label(row,ui.Id("MWModern_Video_DisplayModes_Title"),"Display mode");
    if(title) {
        NoSpacing(title);FixedSize(title,width,24);Position(title,0,0);Tint(title,Brass);Put<unsigned char>(title,0x7F,0);
    }
    constexpr const char* faces[]={"MWModern_Video_Fullscreen_Face","MWModern_Video_Borderless_Face","MWModern_Video_Window_Face"};
    constexpr const char* labels[]={"MWModern_Video_Fullscreen_Label","MWModern_Video_Borderless_Label","MWModern_Video_Window_Label"};
    const int buttonWidth=(width-16)/3;
    for(size_t i=0;i<3;++i) {
        Element* button=ui.Find(row,ui.Id(kChoices[i].id));
        if(!button) button=ui.Block(row,ui.Id(kChoices[i].id));
        if(!button) return false;
        NoSpacing(button);FixedSize(button,buttonWidth,64);Position(button,int(i)*(buttonWidth+8),32);
        Put<unsigned char>(button,0x7F,1);
        Element* face=ui.Rect(button,ui.Id(faces[i]));
        Element* label=ui.Label(button,ui.Id(labels[i]),kChoices[i].text);
        if(face) {NoSpacing(face);FixedSize(face,buttonWidth,64);Position(face,0,0);Put<unsigned char>(face,0x7F,0);SendBack(face);}
        if(label) {NoSpacing(label);FixedSize(label,buttonWidth-16,48);Position(label,8,8);Put<unsigned char>(label,0x7F,0);Put(label,0x15C,0);}
        ui.Event(button,kClick,ModeEvent);ui.Event(button,kOver,ModeEvent);ui.Event(button,kLeave,ModeEvent);
        ColorButton(ui,button,i,false);
    }
    return true;
}
