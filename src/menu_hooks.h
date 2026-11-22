// The native DoAction loader owns a complete AVM1 buffer before enqueuing it.
// One verified call site is wrapped; no archive or global file I/O hooks.
#include "menu_actions.h"
#include "ui_actions.h"
typedef void (__thiscall* NativeReadAction)(BYTE*,void*);
typedef void (__thiscall* NativeResizeAction)(BYTE*,unsigned);
static NativeReadAction nativeReadAction;
static NativeResizeAction nativeResizeAction;
static void __fastcall ReadMenuAction(BYTE* action,void*,void* stream) {
    nativeReadAction(action,stream);
    BYTE* data=*reinterpret_cast<BYTE**>(action+4);
    if(!data)return;
    BYTE* bytes=*reinterpret_cast<BYTE**>(data+0x10);
    unsigned length=*reinterpret_cast<unsigned*>(data+0x14);
    bool videoMenu=pm::menu::IsVideoMenu(bytes,length);
    auto repair=pm::menu::RepairResolutionList(bytes,length);
    if(repair.result==pm::menu::Result::Patched)
        Log("native resolution menu repaired: action offset=%u span=%u; sorted by width/height, values retain native mode indices",
            static_cast<unsigned>(repair.offset),static_cast<unsigned>(repair.length));
    else if(repair.result==pm::menu::Result::Unrecognized)
        Log("native resolution menu layout unrecognized; action bytes unchanged");
    if(videoMenu) {
        nativeResizeAction(data+0x10,length-1+sizeof(displayMenuActions));
        bytes=*reinterpret_cast<BYTE**>(data+0x10);
        memcpy(bytes+length-1,displayMenuActions,sizeof(displayMenuActions));
        Log("native Display Mode row installed above Resolution; four English resources via native text query; mode values 0/1/2");
        return;
    }
    if(pm::menu::IsLayoutRoot(bytes,length)) {
        // The original GArray owns the allocation. Use its own resize operation
        // before enqueuing/executing the timeline, preserving its allocator/flags.
        unsigned expanded=length-1+sizeof(uiLayoutActions);
        nativeResizeAction(data+0x10,expanded);
        bytes=*reinterpret_cast<BYTE**>(data+0x10);
        memcpy(bytes+length-1,uiLayoutActions,sizeof(uiLayoutActions));
        Log("native UI layout extension installed: root=%u own actions=%u; edge-anchored pause controls, unchanged map scale, expanded backgrounds",
            length,static_cast<unsigned>(sizeof(uiLayoutActions)));
    }
}
