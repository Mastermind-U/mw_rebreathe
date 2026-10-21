#pragma once
// Independent x86 bridge to the UI functions in the verified original EXE.
// Layout facts cross-checked against MWSE's public structure documentation.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ModernUi {
struct Element;
using Callback = bool (__cdecl*)(Element*, int, int, int, Element*);
struct Color { float red, green, blue, alpha; };
constexpr Color Rgb(unsigned r, unsigned g, unsigned b, float a = 1.0f) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}
constexpr Color Background = Rgb(18,19,15), Panel = Rgb(29,27,22), Secondary = Rgb(41,37,29);
constexpr Color Brass = Rgb(169,138,82), Parchment = Rgb(229,218,196), Muted = Rgb(165,156,135);
constexpr Color Hover = Rgb(210,179,116), Red = Rgb(138,62,48);
template<class T> inline T Get(const void* object, size_t offset) {
    T value{}; std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value)); return value;
}
template<class T> inline void Put(void* object, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}
inline Element* Parent(Element* element) { return Get<Element*>(element, 0x28); }
inline void Tint(Element* element, Color color) {
    Put(element, 0x14C, color); Put<unsigned char>(element, 0x77, 1);
}
inline void FixedSize(Element* element, int width, int height) {
    Put<unsigned char>(element,0x114,0); Put<unsigned char>(element,0x115,0);
    Put(element,0x118,-1.0f); Put(element,0x11C,-1.0f);
    Put(element,0xEC,width); Put(element,0xF0,height);
    Put(element,0xFC,width); Put(element,0x100,height);
    Put(element,0x104,width); Put(element,0x108,height);
    Put<unsigned char>(element,0x79,1); Put<unsigned char>(element,0x7B,1);
}
inline void Position(Element* element, int x, int y) {
    // Callers use screen coordinates with Y increasing downwards. Native UI
    // elements store Y upwards (the native absolute-align path negates Y).
    const int nativeY=-y;
    // Mirror native setPositionX/Y: cached screen coordinates track the same
    // delta before the next layout traversal refreshes descendants.
    Put(element,0x94,Get<int>(element,0x94)+x-Get<int>(element,0xDC));
    Put(element,0x98,Get<int>(element,0x98)+nativeY-Get<int>(element,0xE0));
    Put(element,0xDC,x); Put(element,0xE0,nativeY);
    Put(element,0x120,-2.0f); Put(element,0x124,-2.0f);
    Put<unsigned char>(element,0x78,1);
}
inline void NoSpacing(Element* element) {
    for (size_t offset=0xB4;offset<=0xD8;offset+=4) Put(element,offset,0);
}
struct Children {
    Element** first; Element** last;
    explicit Children(Element* parent) : first(Get<Element**>(parent,0x1C)), last(Get<Element**>(parent,0x20)) {}
    size_t Count() const { return first && last >= first && last-first <= 4096 ? size_t(last-first) : 0; }
};
inline void SendBack(Element* element) {
    Element* parent=Parent(element); if (!parent) return;
    Children children(parent); const size_t count=children.Count();
    for (size_t i=0;i<count;++i) if (children.first[i]==element) {
        std::memmove(children.first+1,children.first,i*sizeof(Element*)); children.first[0]=element; return;
    }
}
struct Bridge {
    unsigned char* image=nullptr;
    explicit Bridge(HMODULE module=nullptr) : image(reinterpret_cast<unsigned char*>(module)) {}
    template<class T> T At(DWORD address) const { return reinterpret_cast<T>(image+address-0x400000); }
    int Id(const char* name) const { return At<short (__cdecl*)(const char*)>(0x58DF10)(name); }
    int GlobalId(DWORD address) const { return Get<short>(image,address-0x400000); }
    Element* FindMenu(int id) const { return At<Element* (__cdecl*)(int)>(0x595370)(id); }
    Element* Find(Element* root,int id) const {
        return root ? At<Element* (__thiscall*)(Element*,int)>(0x582DE0)(root,id) : nullptr;
    }
    Element* FindGlobal(Element* root,DWORD address) const { return Find(root,GlobalId(address)); }
    Element* Rect(Element* parent,int id) const {
        Element* element=Find(parent,id);
        return element ? element : At<Element* (__thiscall*)(Element*,int,bool,bool)>(0x588340)(parent,id,false,false);
    }
    Element* Label(Element* parent,int id,const char* text) const {
        Element* element=Find(parent,id);
        if (element) At<void (__thiscall*)(Element*,const char*)>(0x58AD30)(element,text);
        else element=At<Element* (__thiscall*)(Element*,int,const char*,bool,bool)>(0x588BE0)(parent,id,text,false,false);
        return element;
    }
    Element* Block(Element* parent,int id) const { return At<Element* (__thiscall*)(Element*,int,bool)>(0x588980)(parent,id,false); }
    Element* Widget(Element* parent,int id,DWORD factory) const {
        return At<Element* (__thiscall*)(Element*,int,void*,bool)>(0x588140)(parent,id,image+factory-0x400000,false);
    }
    void Reattach(Element* element,Element* parent) const { At<void (__thiscall*)(Element*,Element*)>(0x57B850)(element,parent); }
    bool MoveTo(Element* element,Element* parent) const {
        if(!element || !parent) return false;
        Element* previous=Parent(element); if(previous==parent) return true;
        if(!previous) return false;
        const bool wasLast=Get<Element*>(previous,0x3C)==element;
        if(At<short (__thiscall*)(Element*,Element*)>(0x57B4A0)(previous,element)!=short(-0x7FFF)) return false;
        if(wasLast) {
            Children remaining(previous);
            Put<Element*>(previous,0x3C,remaining.Count()?remaining.last[-1]:nullptr);
        }
        // Use the game's vector allocator directly: AddChild/Reattach would
        // redirect a FixedFrame's outer decorations back through shunt_children.
        At<void (__thiscall*)(void*,Element**,unsigned,Element* const*)>(0x4228A0)(
            reinterpret_cast<unsigned char*>(parent)+0x18,Get<Element**>(parent,0x20),1,&element);
        Put(parent,0x3C,element); Put(element,0x28,parent);
        Put<unsigned char>(previous,0x79,1); Put<unsigned char>(parent,0x79,1);
        Put<unsigned char>(element,0x78,1);
        // A newly created scroll pane may materialize its NiNode only during
        // layout. SyncVisuals must run after that final layout, not here.
        return true;
    }
    // Synchronize only UI-owned scene roots. A widget's own NIF geometry and
    // all NiNode identities stay intact; node-less UI containers are flattened.
    bool SyncVisuals(Element* root) const {
        if(!root) return false;
        Element* owner=root;
        for(unsigned depth=0;owner && !Get<void*>(owner,0x88) && depth<64;++depth) owner=Parent(owner);
        if(!owner || !Get<void*>(owner,0x88) || !SyncOwner(owner,0)) return false;
        // Match native Layout's final geometry pass after changing Ni parents
        // and local transforms. Controllers remain disabled: no animation step.
        At<void (__thiscall*)(void*,float,bool,bool)>(0x6EB000)(Get<void*>(owner,0x88),0.0f,false,true);
        return true;
    }
    void Visible(Element* element,bool visible) const { At<void (__thiscall*)(Element*,bool)>(0x57F2A0)(element,visible); }
    Element* Layout(Element* element,bool timestamp=true) const { return At<Element* (__thiscall*)(Element*,bool)>(0x583B70)(element,timestamp); }
    void Property(Element* element,int key,DWORD value,int type) const { At<void (__thiscall*)(Element*,int,DWORD,int)>(0x581F30)(element,key,value,type); }
    DWORD Property(Element* element,int key,int type) const {
        DWORD value=0;
        At<void* (__thiscall*)(Element*,DWORD*,int,int,const Element*,bool)>(0x581440)(element,&value,key,type,nullptr,false);
        return value;
    }
    void Event(Element* element,int key,Callback callback) const { Property(element,key,DWORD(reinterpret_cast<uintptr_t>(callback)),0x20); }
private:
    bool CollectFrontier(Element* parent,std::vector<Element*>& out,unsigned depth) const {
        if(depth>64) return false;
        Children children(parent);
        for(size_t i=0;i<children.Count();++i) {
            Element* child=children.first[i]; if(!child || child==parent) return false;
            if(Get<void*>(child,0x88)) {
                if(out.size()>=4096) return false;
                out.push_back(child);
            } else if(!CollectFrontier(child,out,depth+1)) return false;
        }
        return true;
    }
    bool SyncOwner(Element* owner,unsigned depth) const {
        if(depth>64) return false;
        void* scene=Get<void*>(owner,0x88);
        std::vector<Element*> frontier;
        if(!scene || !CollectFrontier(owner,frontier,0)) return false;
        // Native UI ordering uses detach, compact, then first-vacancy attach.
        // Appending without compaction grows the Ni array's high-water count.
        for(Element* child:frontier) {
            void* node=Get<void*>(child,0x88); if(node==scene) return false;
            void* detached=nullptr;
            auto table=Get<void**>(scene,0);
            reinterpret_cast<void* (__thiscall*)(void*,void**,void*)>(table[0x98/4])(scene,&detached,node);
            At<void (__thiscall*)(void*)>(0x58C3D0)(reinterpret_cast<unsigned char*>(scene)+0x90);
            reinterpret_cast<void (__thiscall*)(void*,void*,bool)>(table[0x94/4])(scene,node,true);
            // Keep DetachChild's temporary strong reference until reattachment.
            At<void (__thiscall*)(void**)>(0x40A880)(&detached);
            if(Get<void*>(node,0x18)!=scene) return false;
            // Reparenting can preserve global UI coordinates while changing
            // the nearest scene owner. In that case layout's dirty test may
            // skip this local translation, so refresh it through native code.
            At<void (__thiscall*)(Element*)>(0x587050)(child);
        }
        for(Element* child:frontier) if(!SyncOwner(child,depth+1)) return false;
        return true;
    }
};
}
