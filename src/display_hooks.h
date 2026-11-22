// Included by compat.cpp after its shared edit helpers. All patch locations are
// from the user's original engine and guarded before the transaction commits.
#include "display_modes.h"
#include "display_guards.h"
#include "menu_hooks.h"
#include "loading_hooks.h"
#include "display_menu_bridge.h"
static std::vector<pm::Mode> displayModes;
static unsigned displayModeCount;
static BYTE* displayClampResume;
static BYTE* displayDefaultResume;
static const D3DFORMAT displayFormats[]={D3DFMT_A1R5G5B5,D3DFMT_A2R10G10B10,D3DFMT_A8R8G8B8,D3DFMT_R5G6B5,D3DFMT_X1R5G5B5,D3DFMT_X8R8G8B8};
typedef void* (__cdecl* EngineAllocate)(size_t);
typedef void (__cdecl* EngineFree)(void*);
static EngineAllocate displayAllocate;
static EngineFree displayFree;
typedef void (__thiscall* NativeSelectMode)(BYTE*,int*,int*,int*,int);
static NativeSelectMode nativeSelectMode;
struct NativeModeBuffer {D3DDISPLAYMODE* begin;D3DDISPLAYMODE* end;D3DDISPLAYMODE* capacity;DWORD reserved;};
typedef unsigned (__thiscall* NativeModeSupport)(BYTE*,unsigned,unsigned,unsigned);
static NativeModeSupport nativeModeSupport;
typedef bool (__thiscall* NativeNeedsReset)(BYTE*);
typedef void (__thiscall* NativeRecovery)(BYTE*);
static NativeNeedsReset nativeNeedsReset;
static NativeRecovery nativeRecovery;
typedef LRESULT (__thiscall* NativeClientResize)(BYTE*,HWND,UINT,WPARAM,LPARAM);
static NativeClientResize nativeClientResize;
static LRESULT __fastcall PreserveRenderResolution(BYTE* renderer,void*,HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_SIZE&&window==gameWindow&&requestedDisplayMode>=0) {
        Log("client size event=%lu %ux%u; selected render remains %ux%u",static_cast<DWORD>(w),
            LOWORD(l),HIWORD(l),*reinterpret_cast<DWORD*>(renderer+0x748),*reinterpret_cast<DWORD*>(renderer+0x74c));
        return 1; // Original handler only writes dimensions/dirty flag and returns 1.
    }
    return nativeClientResize(renderer,window,message,w,l);
}
static unsigned __fastcall TraceMenuModeSupport(BYTE* renderer,void*,unsigned width,unsigned height,unsigned format) {
    unsigned result=nativeModeSupport(renderer,width,height,format);
    auto buffers=reinterpret_cast<NativeModeBuffer*>(renderer+0x79c);
    unsigned matches=0;
    for(unsigned f=0;f<_countof(displayFormats);++f)if(static_cast<unsigned>(displayFormats[f])==format)
        for(auto mode=buffers[f].begin;mode!=buffers[f].end;++mode)if(mode->Width==width&&mode->Height==height)++matches;
    Log("menu mode support %ux%u format=%u cache matches=%u native result=%08x",width,height,format,matches,result);
    return result;
}
static HRESULT WINAPI ResetForDiagnostics(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters) {
    LogDisplayParameters("reset",parameters);
    PreparePresentation(parameters,gameWindow,false);
    // Reset may zero its in/out dimensions and change a windowed format to
    // UNKNOWN. The native recovery immediately reads these engine-owned fields
    // to rebuild targets/viewport, so give the driver a separate parameter copy.
    D3DPRESENT_PARAMETERS driverParameters=*parameters;
    windowedPresentation.Release();
    ULONGLONG begin=GetTickCount64();
    bool arranging=arrangingPresentationWindow;arrangingPresentationWindow=true;
    HRESULT result=device->Reset(&driverParameters);
    arrangingPresentationWindow=arranging;
    Log("D3D9 Reset result=%08lx elapsed=%llu ms windowed=%u refresh=%u",static_cast<unsigned long>(result),
        GetTickCount64()-begin,parameters?parameters->Windowed:0,parameters?parameters->FullScreen_RefreshRateInHz:0);
    if(SUCCEEDED(result)){
        PlacePresentationWindow(parameters->hDeviceWindow?parameters->hDeviceWindow:gameWindow,*parameters,requestedDisplayMode);
        PresentationSucceeded(parameters,device);LogBackBuffer(device);
    }
    else if((result==D3DERR_INVALIDCALL||result==D3DERR_NOTAVAILABLE||result==D3DERR_OUTOFVIDEOMEMORY||result==E_OUTOFMEMORY)&&haveSuccessfulPresentation) {
        // A rejected resolution can fail within the same display mode too.
        // Restore both the original request and its window geometry.
        InterlockedExchange(&requestedDisplayMode,appliedDisplayMode);
        *parameters=successfulPresentation;
        PlacePresentationWindow(parameters->hDeviceWindow?parameters->hDeviceWindow:gameWindow,*parameters,appliedDisplayMode);
        Log("display mode rejected; queued return to %s",DisplayModeName(appliedDisplayMode));
    }
    return result;
}
static void __fastcall RecoverDeviceWithTiming(BYTE* renderer,void*) {
    ULONGLONG begin=GetTickCount64();
    activeDisplayRenderer=renderer;
    Log("device recovery begin requested=%s",DisplayModeName(requestedDisplayMode));
    auto p=reinterpret_cast<D3DPRESENT_PARAMETERS*>(renderer+0x748);
    Log("recovery parameters render=%ux%u interval=%08x count=%u format=%u depth=%u/%u msaa=%u/%u engine AA=%lu flags=%08lx",
        p->BackBufferWidth,p->BackBufferHeight,p->PresentationInterval,p->BackBufferCount,p->BackBufferFormat,
        p->EnableAutoDepthStencil,p->AutoDepthStencilFormat,p->MultiSampleType,p->MultiSampleQuality,
        *reinterpret_cast<DWORD*>(renderer+0x28),p->Flags);
    nativeRecovery(renderer);
    Log("device recovery complete elapsed=%llu ms pending=%u",GetTickCount64()-begin,renderer[0x78e]);
}
static HRESULT WINAPI PresentWithTiming(IDirect3DDevice9* device,const RECT* source,const RECT* destination,HWND window,const RGNDATA* dirty) {
    ULONGLONG begin=GetTickCount64();
    if(InterlockedCompareExchange(&hiddenInitialWindow,0,0)&&!InterlockedCompareExchange(&initialUiFrame,0,0))return S_PRESENT_OCCLUDED;
    if(InterlockedCompareExchange(&hiddenInitialWindow,0,0))RequestInitialReveal(device,gameWindow);
    HRESULT result=haveSuccessfulPresentation&&successfulPresentation.Windowed&&!source&&!destination&&!window&&!dirty?
        windowedPresentation.Present(device,successfulPresentation,gameWindow):device->Present(source,destination,window,dirty);
    if(device==pm9ex::activeDevice)pm9ex::activeDevice->RecordPresentResult(result);
    if(result==D3D_OK&&GetForegroundWindow()==gameWindow&&InterlockedExchange(&focusReturnPending,0))
        Log("focus return first frame=%lu ms present=%llu ms result=%08lx mode=%s",
            GetTickCount()-focusReturnTick,GetTickCount64()-begin,static_cast<unsigned long>(result),DisplayModeName(appliedDisplayMode));
    pmmetrics::RecordPresent(result);
    return result;
}
static __declspec(naked) void PresentTimingCall() {
    __asm {
        push 0
        push 0
        push 0
        push 0
        push eax
        call PresentWithTiming
        ret
    }
}
static __declspec(naked) void RecoveryResetCall() {
    __asm {
        lea edi,[esi+748h] // original recovery path also leaves EDI pointing at these parameters
        push edi
        push eax
        call ResetForDiagnostics
        ret
    }
}
static __declspec(naked) void ResetDiagnosticsCall() {
    __asm {
        push ebp // original presentation parameters
        push eax // original native device
        call ResetForDiagnostics
        ret
    }
}
static int WINAPI OutputScreenMetric(int metric) {
    if(metric==SM_CXSCREEN&&outputDesktopMode.Width)return static_cast<int>(outputDesktopMode.Width);
    if(metric==SM_CYSCREEN&&outputDesktopMode.Height)return static_cast<int>(outputDesktopMode.Height);
    return GetSystemMetrics(metric);
}
static float __cdecl OutputAspect() {
    return static_cast<float>(OutputScreenMetric(SM_CXSCREEN))/OutputScreenMetric(SM_CYSCREEN);
}
static unsigned PreferredRefresh(const NativeModeBuffer& buffer,unsigned width,unsigned height,unsigned preferred) {
    unsigned selected=0,distance=UINT_MAX;
    for(auto mode=buffer.begin;mode!=buffer.end;++mode) {
        if(mode->Width!=width||mode->Height!=height||!mode->RefreshRate)continue;
        unsigned delta=mode->RefreshRate>preferred?mode->RefreshRate-preferred:preferred-mode->RefreshRate;
        if(delta<distance||(delta==distance&&mode->RefreshRate>selected)){distance=delta;selected=mode->RefreshRate;}
    }
    return selected;
}
static void __fastcall SelectDisplayMode(BYTE* renderer,void*,int* width,int* height,int* refresh,int format) {
    activeDisplayRenderer=renderer;
    nativeSelectMode(renderer,width,height,refresh,format);
    RefreshSystemDesktop();
    auto d3d=*reinterpret_cast<IDirect3D9**>(renderer+0x138);
    // Keep the user's desktop choice across exclusive-fullscreen mode changes.
    // Querying the active mode after a switch would report the game's last Hz.
    D3DDISPLAYMODE desktop=outputDesktopMode;
    if(!desktop.RefreshRate&&(FAILED(d3d->GetAdapterDisplayMode(0,&desktop))||!desktop.RefreshRate))return;
    auto buffers=reinterpret_cast<NativeModeBuffer*>(renderer+0x79c);
    for(unsigned f=0;f<_countof(displayFormats);++f)if(displayFormats[f]==format) {
        unsigned selected=PreferredRefresh(buffers[f],*width,*height,desktop.RefreshRate);
        if(selected){*refresh=static_cast<int>(selected);Log("selected display mode %dx%d@%u (system desktop=%u Hz)",*width,*height,selected,desktop.RefreshRate);}
        return;
    }
}
static bool __fastcall CheckPresentationReset(BYTE* renderer,void*) {
    activeDisplayRenderer=renderer;
    LONG wanted=InterlockedCompareExchange(&requestedDisplayMode,0,0);
    if(wanted>=0&&wanted<=2) {
        *reinterpret_cast<DWORD*>(renderer+0x20)=wanted==0?0:1;
        if(wanted!=appliedDisplayMode) {
            auto p=reinterpret_cast<D3DPRESENT_PARAMETERS*>(renderer+0x748);
            int width=p->BackBufferWidth,height=p->BackBufferHeight,refresh=outputDesktopMode.RefreshRate;
            SelectDisplayMode(renderer,nullptr,&width,&height,&refresh,p->BackBufferFormat);
            p->BackBufferWidth=width;p->BackBufferHeight=height;
            SetPresentationMode(*p,wanted,refresh);
            renderer[0x78e]=1;
        }
    }
    auto p=reinterpret_cast<D3DPRESENT_PARAMETERS*>(renderer+0x748);
    if(wanted>=0&&wanted<=2)SetPresentationMode(*p,wanted,p->FullScreen_RefreshRateInHz);
    bool needed=nativeNeedsReset(renderer);
    // Dirty also means "the setter was called" in the original engine. AA is
    // an engine-owned offscreen setting, so matching D3D parameters alone is
    // insufficient. Preserve actual AA/VSync/mode changes and device recovery.
    if(needed&&renderer[0x78e]&&haveSuccessfulPresentation&&wanted==appliedDisplayMode&&
       successfulDevice&&successfulDisplayRenderer==renderer&&
       successfulAntialiasing==*reinterpret_cast<DWORD*>(renderer+0x28)&&
       !memcmp(p,&successfulPresentation,sizeof(*p))&&successfulDevice->TestCooperativeLevel()==D3D_OK) {
        renderer[0x78e]=0;
        Log("skipped redundant device recovery; mode, resolution, interval, formats and AA unchanged");
        return nativeNeedsReset(renderer);
    }
    return needed;
}
static const pm::Mode legacyDisplayModes[]={
    {640,480},{800,480},{800,600},{1024,768},{1152,720},{1152,864},
    {1280,720},{1280,800},{1280,1024},{1440,900},{1600,1200},{1680,1050},
    {1920,1080},{1920,1200},{2048,1152},{2048,1536},{2560,1600}
};
static std::wstring DisplayCatalogPath() {
    wchar_t path[32768];DWORD length=GetModuleFileNameW(selfModule,path,_countof(path));
    wchar_t* slash=length&&length<_countof(path)?wcsrchr(path,L'\\'):nullptr;
    return slash?std::wstring(path,slash-path)+L"\\PrototypeModern.Backup\\display-modes.bin":L"";
}
static std::vector<pm::Mode> ReadDisplayCatalog(const std::wstring& path) {
    std::vector<pm::Mode> legacy(std::begin(legacyDisplayModes),std::end(legacyDisplayModes));
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return legacy;
    LARGE_INTEGER size={};DWORD read=0,magic=0;
    bool valid=GetFileSizeEx(file,&size)&&size.QuadPart>=4+sizeof(legacyDisplayModes)&&
        size.QuadPart<INT_MAX&&(size.QuadPart-4)%sizeof(pm::Mode)==0;
    std::vector<pm::Mode> modes;
    if(valid) {
        modes.resize(static_cast<size_t>((size.QuadPart-4)/sizeof(pm::Mode)));
        valid=ReadFile(file,&magic,4,&read,nullptr)&&read==4&&magic==0x31444d50&&
            ReadFile(file,modes.data(),static_cast<DWORD>(size.QuadPart-4),&read,nullptr)&&read==size.QuadPart-4&&
            !memcmp(modes.data(),legacyDisplayModes,sizeof(legacyDisplayModes));
    }
    CloseHandle(file);
    if(valid)return modes;
    Log("display catalog invalid; rebuilding from driver and legacy indices");return legacy;
}
static void SaveDisplayCatalog(const std::wstring& path) {
    if(path.empty())return;
    std::wstring temporary=path+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){Log("display catalog could not be saved error=%lu",GetLastError());return;}
    DWORD magic=0x31444d50,written=0,bytes=static_cast<DWORD>(displayModes.size()*sizeof(pm::Mode));
    bool ok=WriteFile(file,&magic,4,&written,nullptr)&&written==4&&
        WriteFile(file,displayModes.data(),bytes,&written,nullptr)&&written==bytes&&FlushFileBuffers(file);
    CloseHandle(file);
    if(!ok||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        Log("display catalog commit failed error=%lu",GetLastError());
}
static int __stdcall DisplayWidth(unsigned index){int value=index<displayModes.size()?displayModes[index].width:0;Log("menu mode width index=%u value=%d",index,value);return value;}
static int __stdcall DisplayHeight(unsigned index){int value=index<displayModes.size()?displayModes[index].height:0;Log("menu mode height index=%u value=%d",index,value);return value;}
static int __cdecl DisplayCount(){Log("menu mode count=%u",static_cast<unsigned>(displayModes.size()));return static_cast<int>(displayModes.size());}
static int __fastcall FindDisplayIndex(unsigned width,unsigned height) {
    int found=pm::FindMode(displayModes,width,height);return found>=0?found:0;
}
static int __fastcall CurrentDisplayIndex(void*,void*) {
    BYTE* renderer=*reinterpret_cast<BYTE**>(engineBase+0x127f35c);
    if(!renderer)return 0;
    return FindDisplayIndex(*reinterpret_cast<unsigned*>(renderer+4),*reinterpret_cast<unsigned*>(renderer+8));
}
static bool __fastcall DisplayAllowed(void*,void*,unsigned width,unsigned height) {
    // Calls originate from native D3D mode enumeration or after an exact match
    // in the native enumerated-mode cache. Retain those native format checks.
    return pm::FindMode(displayModes,width,height)>=0;
}
static void __fastcall EnumerateDisplayModes(BYTE* renderer,void*) {
    auto d3d=*reinterpret_cast<IDirect3D9**>(renderer+0x138);
    auto buffers=reinterpret_cast<NativeModeBuffer*>(renderer+0x79c);
    for(unsigned f=0;f<_countof(displayFormats);++f) {
        auto& buffer=buffers[f];
        UINT count=d3d->GetAdapterModeCount(0,displayFormats[f]);
        size_t capacity=buffer.begin?buffer.capacity-buffer.begin:0;
        if(count>capacity&&count<=SIZE_MAX/sizeof(D3DDISPLAYMODE)) {
            auto larger=static_cast<D3DDISPLAYMODE*>(displayAllocate(count*sizeof(D3DDISPLAYMODE)));
            if(larger) {
                if(buffer.begin)displayFree(buffer.begin);
                buffer.begin=larger;buffer.capacity=larger+count;capacity=count;
            }
        }
        buffer.end=buffer.begin;
        for(UINT i=0;i<count;++i) {
            D3DDISPLAYMODE mode={};
            if(SUCCEEDED(d3d->EnumAdapterModes(0,displayFormats[f],i,&mode))&&
                pm::FindMode(displayModes,mode.Width,mode.Height)>=0&&buffer.end!=buffer.capacity)
                *buffer.end++=mode;
        }
        Log("native modes format=%u reported=%u retained=%u capacity=%u",displayFormats[f],count,
            buffer.begin?static_cast<unsigned>(buffer.end-buffer.begin):0,static_cast<unsigned>(capacity));
    }
}
static __declspec(naked) void DisplayClamp() {
    __asm {
        test eax,eax
        jns nonnegative
        xor eax,eax
        jmp finished
    nonnegative:
        cmp eax,dword ptr [displayModeCount]
        jb finished
        mov eax,dword ptr [displayModeCount]
        dec eax
    finished:
        jmp dword ptr [displayClampResume]
    }
}
static __declspec(naked) void DisplayDefaultIndex() {
    __asm {
        pushfd
        pushad
        mov ecx,dword ptr [esp+60] // incoming ESP+0x18: desktop/requested width
        mov edx,dword ptr [esp+56] // incoming ESP+0x14: desktop/requested height
        call FindDisplayIndex
        mov dword ptr [esp+52],eax // incoming ESP+0x10: selected option index
        popad
        popfd
        jmp dword ptr [displayDefaultResume]
    }
}
static bool CheckDisplayGuards() {
    DWORD delta=reinterpret_cast<DWORD>(engineBase)-0x10000000u;
    for(const auto& guard:displayGuards) {
        std::vector<BYTE> expected(guard.bytes,guard.bytes+guard.length);
        for(unsigned i=0;i<guard.relocationCount;++i) {
            DWORD value;memcpy(&value,expected.data()+guard.relocations[i],4);value+=delta;
            memcpy(expected.data()+guard.relocations[i],&value,4);
        }
        if(memcmp(engineBase+guard.rva,expected.data(),expected.size())) {
            Log("display hook conflict RVA=%08lx; no hooks applied",guard.rva);return false;
        }
    }
    return !memcmp(engineBase+0x1075bc8,legacyDisplayModes,sizeof(legacyDisplayModes));
}
static bool PrepareDisplayEdits(std::vector<Edit>& edits) {
    if(!CheckDisplayGuards())return false;
    IDirect3D9* d3d=originalCreate9(D3D_SDK_VERSION);
    if(!d3d){Log("display enumeration factory failed");return false;}
    UINT adapter=SelectAdapter(d3d);
    d3d->GetAdapterDisplayMode(adapter,&outputDesktopMode);
    RefreshSystemDesktop();
    std::vector<pm::Mode> actual;
    for(D3DFORMAT format:displayFormats) {
        UINT count=d3d->GetAdapterModeCount(adapter,format);
        for(UINT i=0;i<count;++i){D3DDISPLAYMODE mode={};if(SUCCEEDED(d3d->EnumAdapterModes(adapter,format,i,&mode)))actual.push_back({mode.Width,mode.Height});}
    }
    d3d->Release();
    if(actual.empty()){Log("no native display modes enumerated; no display hooks applied");return false;}
    auto catalogPath=DisplayCatalogPath();
    displayModes=pm::MergeModes(ReadDisplayCatalog(catalogPath),actual);
    displayModeCount=static_cast<unsigned>(displayModes.size());
    SaveDisplayCatalog(catalogPath);
    displayAllocate=reinterpret_cast<EngineAllocate>(engineBase+0x7def70);
    displayFree=reinterpret_cast<EngineFree>(engineBase+0x7defe0);
    nativeSelectMode=reinterpret_cast<NativeSelectMode>(engineBase+0x6180b0);
    nativeReadAction=reinterpret_cast<NativeReadAction>(engineBase+0x9d73c0);
    nativeResizeAction=reinterpret_cast<NativeResizeAction>(engineBase+0x980be0);
    nativeModeSupport=reinterpret_cast<NativeModeSupport>(engineBase+0x616f20);
    nativeLoadingActive=reinterpret_cast<NativeLoadingActive>(engineBase+0x4aa7b0);
    nativeBeginUi=reinterpret_cast<NativeBeginUi>(engineBase+0x95d080);
    nativeUiExternal=reinterpret_cast<NativeUiExternal>(engineBase+0x873100);
    nativeTextQuery=reinterpret_cast<NativeTextQuery>(engineBase+0x873f30);
    nativeNeedsReset=reinterpret_cast<NativeNeedsReset>(engineBase+0x616540);
    nativeRecovery=reinterpret_cast<NativeRecovery>(engineBase+0x616bb0);
    nativeClientResize=reinterpret_cast<NativeClientResize>(engineBase+0x615fa0);
    if(!ObserveImport(edits,"USER32.dll","ShowCursor",0,&ClientShowCursor,nativeShowCursor))return false;
    displayClampResume=engineBase+0x2e3d0f;displayDefaultResume=engineBase+0x4dfc78;
    edits.push_back(Branch(engineBase+0x2e0240,0xe9,&DisplayWidth,11));
    edits.push_back(Branch(engineBase+0x9d763c,0xe8,&ReadMenuAction,5));
    edits.push_back(Branch(engineBase+0x618413,0xe8,&ResetDiagnosticsCall,7));
    edits.push_back(Branch(engineBase+0x616bc7,0xe8,&RecoveryResetCall,13));
    edits.push_back(Branch(engineBase+0x885e76,0xe8,&BeginUiWithLoadingBackground,5));
    edits.push_back(Branch(engineBase+0x622986,0xe8,&CheckPresentationReset,5));
    edits.push_back(Branch(engineBase+0x622992,0xe8,&RecoverDeviceWithTiming,5));
    edits.push_back(Branch(engineBase+0x70ed74,0xe8,&PreserveRenderResolution,5));
    // Inside the original FormatText path, return our English resources from
    // the text query before the native formatter constructs the GFx result.
    edits.push_back(Branch(engineBase+0x883993,0xe8,&QueryDisplayText,5));
    edits.push_back(Branch(engineBase+0x6162f6,0xe8,&PresentTimingCall,14));
    Edit menuBridge;menuBridge.site=engineBase+0xe74404;
    menuBridge.before.assign(menuBridge.site,menuBridge.site+4);menuBridge.after.resize(4);
    DWORD externalCallback=reinterpret_cast<DWORD>(&DisplayMenuExternal);
    memcpy(menuBridge.after.data(),&externalCallback,4);edits.push_back(menuBridge);
    Edit loading;loading.site=engineBase+0x4abf4d;
    loading.before.assign(loading.site,loading.site+4);loading.after.resize(4);
    DWORD loadingCallback=reinterpret_cast<DWORD>(&SetLoadingActive);
    memcpy(loading.after.data(),&loadingCallback,4);edits.push_back(loading);
    edits.push_back(Branch(engineBase+0x2e02db,0xe8,&TraceMenuModeSupport,5));
    edits.push_back(Branch(engineBase+0x2e0260,0xe9,&DisplayHeight,11));
    edits.push_back(Branch(engineBase+0x2e0280,0xe9,&DisplayCount,5));
    edits.push_back(Branch(engineBase+0x2e0200,0xe9,&OutputAspect,10));
    edits.push_back(Branch(engineBase+0x2e1050,0xe9,&CurrentDisplayIndex,8));
    edits.push_back(Branch(engineBase+0x616ee0,0xe9,&DisplayAllowed,8));
    edits.push_back(Branch(engineBase+0x617560,0xe9,&EnumerateDisplayModes,8));
    edits.push_back(Branch(engineBase+0x618288,0xe8,&SelectDisplayMode,5));
    edits.push_back(Branch(engineBase+0x61837f,0xe8,&SelectDisplayMode,5));
    edits.push_back(Branch(engineBase+0x2e3cf6,0xe9,&DisplayClamp,25));
    edits.push_back(Branch(engineBase+0x4dfc50,0xe9,&DisplayDefaultIndex,40));
    for(DWORD offset:{0x2e3d12u,0x2e3d19u,0x2e3d20u}) {
        Edit edit;edit.site=engineBase+offset;edit.before.assign(edit.site,edit.site+4);
        DWORD pointer=reinterpret_cast<DWORD>(displayModes.data())+(offset==0x2e3d20?0:4);
        edit.after.resize(4);memcpy(edit.after.data(),&pointer,4);edits.push_back(edit);
    }
    Edit rating;rating.site=engineBase+0x2e02b1;rating.before.assign(rating.site,rating.site+2);
    rating.after={0xeb,0x1d};edits.push_back(rating); // keep the actual D3D support check at +0x2e02d0
    Edit aa;aa.site=engineBase+0x2e10ec;aa.before.assign(aa.site,aa.site+2);
    aa.after={0xeb,0x13};edits.push_back(aa); // retain both A8R8G8B8 and float-HDR multisample capability checks
    Edit dimensions;dimensions.site=engineBase+0x4dfba4;dimensions.before.assign(dimensions.site,dimensions.site+6);
    dimensions.after={0xbf,0,0,0,0,0x90}; // mov edi,OutputScreenMetric; original stdcall calls and command-line overrides remain
    auto screenMetric=reinterpret_cast<DWORD>(&OutputScreenMetric);memcpy(dimensions.after.data()+1,&screenMetric,4);edits.push_back(dimensions);
    Log("display modes: legacy indices=17 additional=%u adapter=%u",displayModeCount-17,adapter);
    for(unsigned i=17;i<displayModeCount;++i)Log("display mode index=%u %ux%u",i,displayModes[i].width,displayModes[i].height);
    return true;
}
