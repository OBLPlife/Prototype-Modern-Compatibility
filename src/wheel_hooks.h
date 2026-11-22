#include "wheel_cursor.h"
static pmwheel::Remainder wheelRemainder;
static BYTE* wheelClampReturn;
static void (*nativeCenterWheelCursor)();
static void __cdecl ProjectWheelCursor(float dx,float dy,int width,int height) {
    auto* point=reinterpret_cast<int*>(engineBase+0x1315c6c);
    const float radius=*reinterpret_cast<float*>(engineBase+0x11cef44);
    wheelRemainder.Project(point[0],point[1],static_cast<int>(dx),static_cast<int>(dy),width,height,radius);
}
static void __cdecl CenterWheelCursor() {
    wheelRemainder.Reset();
    nativeCenterWheelCursor();
}
__declspec(naked) static void WheelCircleClamp() {
    __asm {
        // The original block overwrites XMM0-5 and volatile GPRs. Preserve
        // XMM6/7 as well as the native nonvolatile GPRs preserved by C++.
        sub esp,20h
        movups [esp],xmm6
        movups [esp+10h],xmm7
        push edx
        push edi
        push dword ptr [esp+38h]
        push dword ptr [esp+38h]
        call ProjectWheelCursor
        add esp,10h
        movups xmm6,[esp]
        movups xmm7,[esp+10h]
        add esp,20h
        jmp dword ptr [wheelClampReturn]
    }
}
static bool PrepareWheelEdits(std::vector<Edit>& edits) {
    const BYTE start[]={0x8b,0x0d};
    const BYTE convertX[]={0xf3,0x0f,0x2c,0x4c,0x24,0x0c};
    const BYTE convertY[]={0xf3,0x0f,0x2c,0x44,0x24,0x10};
    const BYTE finish[]={0x0f,0x57,0xc0,0x6a,0x17};
    auto centerTarget=engineBase+0x4c6792+5+*reinterpret_cast<int*>(engineBase+0x4c6793);
    if(memcmp(engineBase+0x864779,start,sizeof(start))||
       *reinterpret_cast<BYTE**>(engineBase+0x86477b)!=engineBase+0x127f35c||
       memcmp(engineBase+0x8646f7,convertX,sizeof(convertX))||
       memcmp(engineBase+0x864720,convertY,sizeof(convertY))||
       memcmp(engineBase+0x864838,finish,sizeof(finish))||
       engineBase[0x4c6792]!=0xe8||centerTarget!=engineBase+0x864440) {
        Log("wheel cursor instruction conflict; no hooks applied");return false;
    }
    nativeCenterWheelCursor=reinterpret_cast<void(*)()>(engineBase+0x864440);
    wheelClampReturn=engineBase+0x864838;
    edits.push_back(Branch(engineBase+0x864779,0xe9,&WheelCircleClamp,6));
    edits.push_back(Branch(engineBase+0x4c6792,0xe8,&CenterWheelCursor,5));
    Log("wheel cursor retains circular-clamp subpixel motion; native 300-pixel radius, input counts, sensitivity and selection rules unchanged");
    return true;
}
