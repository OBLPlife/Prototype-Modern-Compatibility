// The original loading screen calls fe_setIsLoadingSceenActive at entry/exit.
// Preserve its result and read the accepted state, including native autotester checks.
typedef bool (__thiscall* NativeLoadingActive)(BYTE*,bool);
typedef void (__thiscall* NativeBeginUi)(BYTE*);
static NativeLoadingActive nativeLoadingActive;
static NativeBeginUi nativeBeginUi;
static volatile LONG loadingScreenActive,loadingClearFrames;
static volatile LONG loadingClearReported;

static bool __fastcall SetLoadingActive(BYTE* manager,void*,bool active) {
    bool result=nativeLoadingActive(manager,active);
    LONG accepted=manager[0xe]!=0;
    LONG before=InterlockedExchange(&loadingScreenActive,accepted);
    if(before!=accepted) {
        LONG frames=InterlockedExchange(&loadingClearFrames,0);
        InterlockedExchange(&loadingClearReported,0);
        Log("loading screen active=%ld accepted=%u previous full-target clears=%ld",accepted,result,frames);
    }
    return result;
}
static HRESULT ClearLoadingTarget(IDirect3DDevice9* device) {
    // Clear once before the complete UI list, never between movies/layers.
    // Operate on its current target and restore viewport/scissor; no resource swap.
    IDirect3DSurface9* target=nullptr;
    HRESULT result=device->GetRenderTarget(0,&target);
    if(FAILED(result)||!target)return FAILED(result)?result:E_FAIL;
    D3DSURFACE_DESC desc={};result=target->GetDesc(&desc);target->Release();
    if(FAILED(result))return result;
    D3DVIEWPORT9 previous={};result=device->GetViewport(&previous);
    if(FAILED(result))return result;
    DWORD scissor=0;result=device->GetRenderState(D3DRS_SCISSORTESTENABLE,&scissor);
    if(FAILED(result))return result;
    D3DVIEWPORT9 full={0,0,desc.Width,desc.Height,0.0f,1.0f};
    result=device->SetViewport(&full);
    if(FAILED(result))return result;
    HRESULT state=device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    if(SUCCEEDED(state))result=device->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1.0f,0);
    else result=state;
    device->SetRenderState(D3DRS_SCISSORTESTENABLE,scissor);
    device->SetViewport(&previous);
    return result;
}
static void __fastcall BeginUiWithLoadingBackground(BYTE* renderer,void*) {
    nativeBeginUi(renderer);
    InterlockedExchange(&initialUiFrame,1);
    if(!InterlockedCompareExchange(&loadingScreenActive,0,0))return;
    auto device=*reinterpret_cast<IDirect3DDevice9**>(engineBase+0x1276d38);
    if(!device)return;
    HRESULT result=ClearLoadingTarget(device);
    if(SUCCEEDED(result))InterlockedIncrement(&loadingClearFrames);
    if(!InterlockedExchange(&loadingClearReported,1))
        Log("loading full-target background result=%08lx",static_cast<unsigned long>(result));
}
