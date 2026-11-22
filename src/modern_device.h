// Original D3D9 engine -> D3D9Ex bridge. No third-party wrapper code.
// Ex preserves resources across focus changes. Managed resources need explicit
// CPU storage: merely changing MANAGED to DEFAULT breaks the engine's read locks.
#pragma once
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <new>
#include <cstring>
#include "engine_telemetry.h"
#include "gpu_diagnostics.h"
namespace pm9ex {
class Managed;
class StateBlock;
class Device;
static Device* activeDevice;
using Bindings=std::map<UINT,Managed*>;
using Guard=std::lock_guard<std::recursive_mutex>;
template<class T,class I,class... A>HRESULT Allocate(I** out,A... args) {
    try{*out=new T(args...);return S_OK;}catch(const std::bad_alloc&){*out=nullptr;return E_OUTOFMEMORY;}
}
static constexpr UINT StreamKey=0x10000,IndexKey=0x20000;
static D3DDISPLAYMODEEX FullscreenMode(const D3DPRESENT_PARAMETERS& p) {
    D3DDISPLAYMODEEX m={};m.Size=sizeof(m);m.Width=p.BackBufferWidth;m.Height=p.BackBufferHeight;
    m.RefreshRate=p.FullScreen_RefreshRateInHz;m.Format=p.BackBufferFormat;
    m.ScanLineOrdering=D3DSCANLINEORDERING_PROGRESSIVE;return m;
}
static HRESULT LegacyCooperativeResult(HRESULT state,bool resetPending,bool foreground) {
    if(state==S_PRESENT_MODE_CHANGED||state==D3DERR_DEVICEHUNG)return D3DERR_DEVICENOTRESET;
    if(state==D3DERR_DEVICELOST)return foreground?D3DERR_DEVICENOTRESET:D3DERR_DEVICELOST;
    if(state==S_OK)return resetPending?D3DERR_DEVICENOTRESET:S_OK;
    if(state==S_PRESENT_OCCLUDED)return resetPending?D3DERR_DEVICELOST:S_OK;
    return state;
}
class Device final:public IDirect3DDevice9 {
    volatile LONG refs=1;
public:
    IDirect3DDevice9Ex* real;
    IDirect3D9* factory;
    HWND window;
    std::recursive_mutex mutex;
    pmdiag::GpuDiagnostics diagnostics;
    std::map<void*,Managed*> wrappers,native;
    Bindings bound,recorded;
    std::map<DWORD,DWORD> recordedLOD;
    bool recording=false;
    // Guarded by mutex. A clean binding set needs no per-draw map traversal.
    // Every CPU write, new LOD view and live binding change invalidates it.
    bool uploadsPending=true;
    volatile LONG presentStatus=S_OK;
    IDirect3DStateBlock9* resetDefaults=nullptr;
    IDirect3DSurface9* defaultDepth=nullptr;
    D3DPRESENT_PARAMETERS presentation={};
    bool havePresentation=false,resetPending=false;
    DWORD samplerLOD[261]={};
    unsigned resetCount=0,targetRefreshCount=0,managedCreated=0;
    Device(IDirect3DDevice9Ex* r,IDirect3D9* f,HWND w):real(r),factory(f),window(w),diagnostics(pmdiag::D3D9QueryBackend(r)){
        factory->AddRef();real->CreateStateBlock(D3DSBT_ALL,&resetDefaults);activeDevice=this;
        real->GetDepthStencilSurface(&defaultDepth);
        IDirect3DSwapChain9* initial=nullptr;
        if(SUCCEEDED(real->GetSwapChain(0,&initial))) {
            havePresentation=SUCCEEDED(initial->GetPresentParameters(&presentation));initial->Release();
        }
    }
    ~Device();
    Managed* Find(void* p) {auto i=wrappers.find(p);return i==wrappers.end()?nullptr:i->second;}
    Managed* FindNative(void* p) {auto i=native.find(p);return i==native.end()?nullptr:i->second;}
    template<class T>T* Unwrap(T* p);
    template<class T>void Rewrap(T** p);
    void Bind(Bindings& b,UINT key,Managed* m);
    void ClearBindings(Bindings& b);
    void CopyBindings(Bindings& destination,const Bindings& source);
    void SyncBindings();
    HRESULT Flush();
    void RecordPresentResult(HRESULT result){InterlockedExchange(&presentStatus,result);}
    HRESULT RestoreLegacyDefaults();
    void ApplyLOD(UINT stage,bool rebindTexture=true);
    #include "modern_device_methods.h"
};
// External resource references keep the public device alive. Bindings and state
// blocks keep resources alive without introducing a device/resource ref cycle.
class Managed {
    ULONG external=1,internal=0;
public:
    Device* owner;
    IDirect3DResource9* gpu;
    IDirect3DResource9* api=nullptr;
    IDirect3DBaseTexture9* shadow=nullptr;
    std::map<DWORD,IDirect3DBaseTexture9*> lodViews;
    std::map<UINT,IUnknown*> children;
    DWORD lod=0;
    bool dirty=false;
    void MarkDirty(){dirty=true;owner->uploadsPending=true;}
    explicit Managed(Device* d,IDirect3DResource9* g):owner(d),gpu(g){}
    void Register(IDirect3DResource9* p) {
        try{owner->wrappers[p]=this;owner->native[gpu]=this;}
        catch(...){owner->wrappers.erase(p);owner->native.erase(gpu);throw;}
        api=p;owner->AddRef();++owner->managedCreated;
    }
    virtual ~Managed() {
        if(!api)return; // A throwing constructor has not taken ownership yet.
        owner->wrappers.erase(api);owner->native.erase(gpu);
        for(auto& view:lodViews){owner->native.erase(view.second);view.second->Release();}
        if(shadow)shadow->Release();gpu->Release();
    }
    ULONG Add() {Guard g(owner->mutex);if(!external)owner->AddRef();return ++external;}
    ULONG Drop() {
        Device* d=owner;ULONG result;
        {Guard g(d->mutex);result=--external;if(!result&&!internal)delete this;}
        if(!result)d->Release();return result;
    }
    void Hold(){++internal;}
    void Unhold(){if(!--internal&&!external)delete this;}
    HRESULT GetOwner(IDirect3DDevice9** p){if(!p)return D3DERR_INVALIDCALL;*p=owner;owner->AddRef();return S_OK;}
    IDirect3DBaseTexture9* SamplingTexture() {
        auto base=static_cast<IDirect3DBaseTexture9*>(gpu);
        if(!lod)return base;
        auto found=lodViews.find(lod);if(found!=lodViews.end())return found->second;
        IDirect3DBaseTexture9* view=nullptr;HRESULT hr=D3DERR_INVALIDCALL;
        if(shadow->GetType()==D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC d={};static_cast<IDirect3DTexture9*>(shadow)->GetLevelDesc(lod,&d);IDirect3DTexture9* t=nullptr;
            hr=owner->real->CreateTexture(d.Width,d.Height,shadow->GetLevelCount()-lod,0,d.Format,D3DPOOL_DEFAULT,&t,nullptr);view=t;
        } else if(shadow->GetType()==D3DRTYPE_CUBETEXTURE) {
            D3DSURFACE_DESC d={};static_cast<IDirect3DCubeTexture9*>(shadow)->GetLevelDesc(lod,&d);IDirect3DCubeTexture9* t=nullptr;
            hr=owner->real->CreateCubeTexture(d.Width,shadow->GetLevelCount()-lod,0,d.Format,D3DPOOL_DEFAULT,&t,nullptr);view=t;
        } else {
            D3DVOLUME_DESC d={};static_cast<IDirect3DVolumeTexture9*>(shadow)->GetLevelDesc(lod,&d);IDirect3DVolumeTexture9* t=nullptr;
            hr=owner->real->CreateVolumeTexture(d.Width,d.Height,d.Depth,shadow->GetLevelCount()-lod,0,d.Format,D3DPOOL_DEFAULT,&t,nullptr);view=t;
        }
        if(FAILED(hr)){Log("D3D9Ex managed LOD view failed=%08lx level=%lu",hr,lod);return base;}
        try{lodViews[lod]=view;owner->native[view]=this;}
        catch(const std::bad_alloc&){lodViews.erase(lod);owner->native.erase(view);view->Release();return base;}
        MarkDirty();return view;
    }
    void DirtyShadow() {
        switch(shadow->GetType()) {
        case D3DRTYPE_TEXTURE:static_cast<IDirect3DTexture9*>(shadow)->AddDirtyRect(nullptr);break;
        case D3DRTYPE_CUBETEXTURE:for(unsigned f=0;f<6;++f)static_cast<IDirect3DCubeTexture9*>(shadow)->AddDirtyRect(static_cast<D3DCUBEMAP_FACES>(f),nullptr);break;
        case D3DRTYPE_VOLUMETEXTURE:static_cast<IDirect3DVolumeTexture9*>(shadow)->AddDirtyBox(nullptr);break;
        default:break;
        }
    }
    HRESULT Upload() {
        if(!dirty||!shadow)return S_OK;
        // Marking mip zero dirty also transfers lower mip levels, including the
        // engine's read/modify/copy mip path and partial volume writes.
        DirtyShadow();
        auto upload=owner->diagnostics.BeginUpload();
        HRESULT hr=owner->real->UpdateTexture(shadow,static_cast<IDirect3DBaseTexture9*>(gpu));
        owner->diagnostics.EndUpload(upload);
        // Native managed SetLOD changes the resident top mip even with mip
        // filtering disabled. MAXMIPLEVEL alone does not match that behavior.
        for(auto& view:lodViews)if(SUCCEEDED(hr)){DirtyShadow();auto lodUpload=owner->diagnostics.BeginUpload();hr=owner->real->UpdateTexture(shadow,view.second);owner->diagnostics.EndUpload(lodUpload);}
        if(SUCCEEDED(hr))dirty=false;else Log("D3D9Ex managed upload failed=%08lx type=%u",hr,gpu->GetType());
        return hr;
    }
    DWORD SetTextureLOD(DWORD value) {
        Guard g(owner->mutex);DWORD previous=lod;
        lod=std::min(value,static_cast<IDirect3DBaseTexture9*>(gpu)->GetLevelCount()-1);
        for(auto& i:owner->bound)if(i.second==this&&i.first<StreamKey)owner->ApplyLOD(i.first);
        return previous;
    }
};
#define PM_RESOURCE(Type) \
    ULONG STDMETHODCALLTYPE AddRef() override{return Add();} \
    ULONG STDMETHODCALLTYPE Release() override{return Drop();} \
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** p) override{return GetOwner(p);} \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID k,const void* p,DWORD n,DWORD f) override{return gpu->SetPrivateData(k,p,n,f);} \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID k,void* p,DWORD* n) override{return gpu->GetPrivateData(k,p,n);} \
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID k) override{return gpu->FreePrivateData(k);} \
    DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override{return gpu->SetPriority(p);} \
    DWORD STDMETHODCALLTYPE GetPriority() override{return gpu->GetPriority();} \
    void STDMETHODCALLTYPE PreLoad() override{Guard g(owner->mutex);Upload();gpu->PreLoad();} \
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override{return gpu->GetType();} \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override { \
        if(!p)return E_POINTER;*p=nullptr; \
        if(id==__uuidof(IUnknown)||id==__uuidof(IDirect3DResource9)||id==__uuidof(Type)|| \
            (shadow&&id==__uuidof(IDirect3DBaseTexture9))){*p=static_cast<Type*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
#define PM_TEXTURE \
    DWORD STDMETHODCALLTYPE SetLOD(DWORD n) override{return SetTextureLOD(n);} \
    DWORD STDMETHODCALLTYPE GetLOD() override{Guard g(owner->mutex);return lod;} \
    DWORD STDMETHODCALLTYPE GetLevelCount() override{return static_cast<IDirect3DBaseTexture9*>(gpu)->GetLevelCount();} \
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE f) override{return static_cast<IDirect3DBaseTexture9*>(gpu)->SetAutoGenFilterType(f);} \
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override{return static_cast<IDirect3DBaseTexture9*>(gpu)->GetAutoGenFilterType();} \
    void STDMETHODCALLTYPE GenerateMipSubLevels() override{Guard g(owner->mutex);if(SUCCEEDED(Upload()))static_cast<IDirect3DBaseTexture9*>(gpu)->GenerateMipSubLevels();}
class Surface final:public IDirect3DSurface9 {
    volatile LONG refs=1;
public:
    Managed* parent;
    IDirect3DSurface9 *cpu,*gpu;
    UINT key;
    DWORD lockFlags=0;
    Surface(Managed* p,IDirect3DSurface9* c,IDirect3DSurface9* g,UINT k):parent(p),cpu(c),gpu(g),key(k){parent->children[key]=this;parent->Add();}
    ~Surface(){{Guard g(parent->owner->mutex);parent->children.erase(key);cpu->Release();gpu->Release();}parent->Drop();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override {
        if(!p)return E_POINTER;*p=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IDirect3DResource9)||id==__uuidof(IDirect3DSurface9)){*p=this;AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** p) override{return parent->GetOwner(p);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID k,const void* p,DWORD n,DWORD f) override{return gpu->SetPrivateData(k,p,n,f);}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID k,void* p,DWORD* n) override{return gpu->GetPrivateData(k,p,n);}
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID k) override{return gpu->FreePrivateData(k);}
    DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override{return gpu->SetPriority(p);}
    DWORD STDMETHODCALLTYPE GetPriority() override{return gpu->GetPriority();}
    void STDMETHODCALLTYPE PreLoad() override{Guard g(parent->owner->mutex);parent->Upload();}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override{return D3DRTYPE_SURFACE;}
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID id,void** p) override{return parent->api->QueryInterface(id,p);}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* p) override{HRESULT hr=gpu->GetDesc(p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* p,const RECT* r,DWORD f) override{Guard g(parent->owner->mutex);HRESULT hr=cpu->LockRect(p,r,f);if(SUCCEEDED(hr))lockFlags=f;return hr;}
    HRESULT STDMETHODCALLTYPE UnlockRect() override{Guard g(parent->owner->mutex);HRESULT hr=cpu->UnlockRect();if(SUCCEEDED(hr)&&!(lockFlags&(D3DLOCK_READONLY|D3DLOCK_NO_DIRTY_UPDATE)))parent->MarkDirty();return hr;}
    HRESULT STDMETHODCALLTYPE GetDC(HDC* p) override{return cpu->GetDC(p);}
    HRESULT STDMETHODCALLTYPE ReleaseDC(HDC p) override{Guard g(parent->owner->mutex);HRESULT hr=cpu->ReleaseDC(p);if(SUCCEEDED(hr))parent->MarkDirty();return hr;}
};
class Volume final:public IDirect3DVolume9 {
    volatile LONG refs=1;
    Managed* parent;IDirect3DVolume9 *cpu,*gpu;DWORD lockFlags=0;UINT key;
public:
    Volume(Managed* p,IDirect3DVolume9* c,IDirect3DVolume9* g,UINT k):parent(p),cpu(c),gpu(g),key(k){parent->children[key]=this;parent->Add();}
    ~Volume(){{Guard g(parent->owner->mutex);parent->children.erase(key);cpu->Release();gpu->Release();}parent->Drop();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override{if(!p)return E_POINTER;*p=nullptr;if(id==__uuidof(IUnknown)||id==__uuidof(IDirect3DVolume9)){*p=this;AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** p) override{return parent->GetOwner(p);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID k,const void* p,DWORD n,DWORD f) override{return gpu->SetPrivateData(k,p,n,f);}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID k,void* p,DWORD* n) override{return gpu->GetPrivateData(k,p,n);}
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID k) override{return gpu->FreePrivateData(k);}
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID id,void** p) override{return parent->api->QueryInterface(id,p);}
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC* p) override{HRESULT hr=gpu->GetDesc(p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX* p,const D3DBOX* b,DWORD f) override{Guard g(parent->owner->mutex);HRESULT hr=cpu->LockBox(p,b,f);if(SUCCEEDED(hr))lockFlags=f;return hr;}
    HRESULT STDMETHODCALLTYPE UnlockBox() override{Guard g(parent->owner->mutex);HRESULT hr=cpu->UnlockBox();if(SUCCEEDED(hr)&&!(lockFlags&(D3DLOCK_READONLY|D3DLOCK_NO_DIRTY_UPDATE)))parent->MarkDirty();return hr;}
};
class Texture final:public IDirect3DTexture9,public Managed {
    std::vector<DWORD> locks;
    IDirect3DTexture9* Cpu(){return static_cast<IDirect3DTexture9*>(shadow);}
    IDirect3DTexture9* Gpu(){return static_cast<IDirect3DTexture9*>(gpu);}
public:
    Texture(Device* d,IDirect3DTexture9* c,IDirect3DTexture9* g):Managed(d,g),locks(c->GetLevelCount()) {shadow=c;Register(this);}
    PM_RESOURCE(IDirect3DTexture9)
    PM_TEXTURE
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l,D3DSURFACE_DESC* p) override{HRESULT hr=Gpu()->GetLevelDesc(l,p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT l,IDirect3DSurface9** p) override {
        if(!p)return D3DERR_INVALIDCALL;*p=nullptr;Guard guard(owner->mutex);
        auto found=children.find(l);if(found!=children.end())return found->second->QueryInterface(__uuidof(IDirect3DSurface9),reinterpret_cast<void**>(p));
        IDirect3DSurface9 *c=nullptr,*g=nullptr;HRESULT hr=Cpu()->GetSurfaceLevel(l,&c);
        if(SUCCEEDED(hr))hr=Gpu()->GetSurfaceLevel(l,&g);
        if(SUCCEEDED(hr)){hr=Allocate<Surface>(p,this,c,g,l);if(SUCCEEDED(hr))return hr;}
        if(c)c->Release();if(g)g->Release();return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT l,D3DLOCKED_RECT* p,const RECT* r,DWORD f) override{Guard g(owner->mutex);HRESULT hr=Cpu()->LockRect(l,p,r,f);if(SUCCEEDED(hr))locks[l]=f;return hr;}
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT l) override{Guard g(owner->mutex);HRESULT hr=Cpu()->UnlockRect(l);if(SUCCEEDED(hr)&&!(locks[l]&(D3DLOCK_READONLY|D3DLOCK_NO_DIRTY_UPDATE)))MarkDirty();return hr;}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT* r) override{Guard g(owner->mutex);HRESULT hr=Cpu()->AddDirtyRect(r);if(SUCCEEDED(hr))MarkDirty();return hr;}
};
class CubeTexture final:public IDirect3DCubeTexture9,public Managed {
    std::vector<DWORD> locks;
    IDirect3DCubeTexture9* Cpu(){return static_cast<IDirect3DCubeTexture9*>(shadow);}
    IDirect3DCubeTexture9* Gpu(){return static_cast<IDirect3DCubeTexture9*>(gpu);}
public:
    CubeTexture(Device* d,IDirect3DCubeTexture9* c,IDirect3DCubeTexture9* g):Managed(d,g),locks(c->GetLevelCount()*6){shadow=c;Register(this);}
    PM_RESOURCE(IDirect3DCubeTexture9)
    PM_TEXTURE
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l,D3DSURFACE_DESC* p) override{HRESULT hr=Gpu()->GetLevelDesc(l,p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE GetCubeMapSurface(D3DCUBEMAP_FACES f,UINT l,IDirect3DSurface9** p) override {
        if(!p)return D3DERR_INVALIDCALL;*p=nullptr;Guard guard(owner->mutex);
        if(f<0||f>=6||l>=GetLevelCount())return D3DERR_INVALIDCALL;
        UINT key=l*6+f;auto found=children.find(key);if(found!=children.end())return found->second->QueryInterface(__uuidof(IDirect3DSurface9),reinterpret_cast<void**>(p));
        IDirect3DSurface9 *c=nullptr,*g=nullptr;HRESULT hr=Cpu()->GetCubeMapSurface(f,l,&c);
        if(SUCCEEDED(hr))hr=Gpu()->GetCubeMapSurface(f,l,&g);
        if(SUCCEEDED(hr)){hr=Allocate<Surface>(p,this,c,g,key);if(SUCCEEDED(hr))return hr;}
        if(c)c->Release();if(g)g->Release();return hr;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES f,UINT l,D3DLOCKED_RECT* p,const RECT* r,DWORD flags) override{Guard g(owner->mutex);HRESULT hr=Cpu()->LockRect(f,l,p,r,flags);if(SUCCEEDED(hr))locks[l*6+f]=flags;return hr;}
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES f,UINT l) override{Guard g(owner->mutex);HRESULT hr=Cpu()->UnlockRect(f,l);if(SUCCEEDED(hr)&&!(locks[l*6+f]&(D3DLOCK_READONLY|D3DLOCK_NO_DIRTY_UPDATE)))MarkDirty();return hr;}
    HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES f,const RECT* r) override{Guard g(owner->mutex);HRESULT hr=Cpu()->AddDirtyRect(f,r);if(SUCCEEDED(hr))MarkDirty();return hr;}
};
class VolumeTexture final:public IDirect3DVolumeTexture9,public Managed {
    std::vector<DWORD> locks;
    IDirect3DVolumeTexture9* Cpu(){return static_cast<IDirect3DVolumeTexture9*>(shadow);}
    IDirect3DVolumeTexture9* Gpu(){return static_cast<IDirect3DVolumeTexture9*>(gpu);}
public:
    VolumeTexture(Device* d,IDirect3DVolumeTexture9* c,IDirect3DVolumeTexture9* g):Managed(d,g),locks(c->GetLevelCount()){shadow=c;Register(this);}
    PM_RESOURCE(IDirect3DVolumeTexture9)
    PM_TEXTURE
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT l,D3DVOLUME_DESC* p) override{HRESULT hr=Gpu()->GetLevelDesc(l,p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT l,IDirect3DVolume9** p) override {
        if(!p)return D3DERR_INVALIDCALL;*p=nullptr;Guard guard(owner->mutex);
        auto found=children.find(l);if(found!=children.end())return found->second->QueryInterface(__uuidof(IDirect3DVolume9),reinterpret_cast<void**>(p));
        IDirect3DVolume9 *c=nullptr,*g=nullptr;HRESULT hr=Cpu()->GetVolumeLevel(l,&c);
        if(SUCCEEDED(hr))hr=Gpu()->GetVolumeLevel(l,&g);
        if(SUCCEEDED(hr)){hr=Allocate<Volume>(p,this,c,g,l);if(SUCCEEDED(hr))return hr;}
        if(c)c->Release();if(g)g->Release();return hr;
    }
    HRESULT STDMETHODCALLTYPE LockBox(UINT l,D3DLOCKED_BOX* p,const D3DBOX* b,DWORD f) override{Guard g(owner->mutex);HRESULT hr=Cpu()->LockBox(l,p,b,f);if(SUCCEEDED(hr))locks[l]=f;return hr;}
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT l) override{Guard g(owner->mutex);HRESULT hr=Cpu()->UnlockBox(l);if(SUCCEEDED(hr)&&!(locks[l]&(D3DLOCK_READONLY|D3DLOCK_NO_DIRTY_UPDATE)))MarkDirty();return hr;}
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX* b) override{Guard g(owner->mutex);HRESULT hr=Cpu()->AddDirtyBox(b);if(SUCCEEDED(hr))MarkDirty();return hr;}
};
template<class Interface,class Description>class Buffer final:public Interface,public Managed {
    std::vector<BYTE> bytes;
    UINT offset=0,length=0;DWORD flags=0;bool locked=false;
    Interface* Gpu(){return static_cast<Interface*>(gpu);}
public:
    Buffer(Device* d,Interface* g,UINT size):Managed(d,g),bytes(size){Register(this);}
    HRESULT ReadGpu() {Description desc={};HRESULT hr=Gpu()->GetDesc(&desc);if(FAILED(hr)||(desc.Usage&D3DUSAGE_WRITEONLY))return hr;void* p=nullptr;hr=Gpu()->Lock(0,0,&p,D3DLOCK_READONLY);if(SUCCEEDED(hr)){memcpy(bytes.data(),p,bytes.size());hr=Gpu()->Unlock();}return hr;}
    PM_RESOURCE(Interface)
    HRESULT STDMETHODCALLTYPE GetDesc(Description* p) override{HRESULT hr=Gpu()->GetDesc(p);if(SUCCEEDED(hr))p->Pool=D3DPOOL_MANAGED;return hr;}
    HRESULT STDMETHODCALLTYPE Lock(UINT o,UINT n,void** p,DWORD f) override {
        Guard g(owner->mutex);if(!p||locked||o>bytes.size()||n>bytes.size()-o||(f&(D3DLOCK_DISCARD|D3DLOCK_NOOVERWRITE)))return D3DERR_INVALIDCALL;
        Description desc={};if((f&D3DLOCK_READONLY)&&SUCCEEDED(Gpu()->GetDesc(&desc))&&(desc.Usage&D3DUSAGE_WRITEONLY))return D3DERR_INVALIDCALL;
        offset=o;length=n?n:static_cast<UINT>(bytes.size())-o;flags=f;locked=true;*p=bytes.data()+o;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override {
        Guard g(owner->mutex);if(!locked)return D3DERR_INVALIDCALL;locked=false;
        if(flags&D3DLOCK_READONLY)return S_OK;
        auto upload=owner->diagnostics.BeginUpload();
        void* p=nullptr;HRESULT hr=Gpu()->Lock(offset,length,&p,0);
        if(SUCCEEDED(hr)){memcpy(p,bytes.data()+offset,length);hr=Gpu()->Unlock();}
        owner->diagnostics.EndUpload(upload);return hr;
    }
};
using VertexBuffer=Buffer<IDirect3DVertexBuffer9,D3DVERTEXBUFFER_DESC>;
using IndexBuffer=Buffer<IDirect3DIndexBuffer9,D3DINDEXBUFFER_DESC>;
#undef PM_RESOURCE
#undef PM_TEXTURE
template<class T>T* Device::Unwrap(T* p){auto m=Find(p);return m?static_cast<T*>(m->gpu):p;}
template<class T>void Device::Rewrap(T** p){if(p&&*p){auto m=FindNative(*p);if(m){(*p)->Release();m->Add();*p=static_cast<T*>(m->api);}}}
inline void Device::Bind(Bindings& b,UINT key,Managed* m) {
    auto old=b.find(key);if(old!=b.end()&&old->second==m)return;
    if(&b==&bound)uploadsPending=true;
    if(m)m->Hold();if(old!=b.end()&&old->second)old->second->Unhold();b[key]=m;
}
inline void Device::ClearBindings(Bindings& b){for(auto& i:b)if(i.second)i.second->Unhold();b.clear();}
inline void Device::CopyBindings(Bindings& destination,const Bindings& source){Bindings copy=source;for(auto& i:copy)if(i.second)i.second->Hold();ClearBindings(destination);destination.swap(copy);}
inline Device::~Device(){diagnostics.Reset();if(activeDevice==this)activeDevice=nullptr;ClearBindings(recorded);ClearBindings(bound);if(defaultDepth)defaultDepth->Release();if(resetDefaults)resetDefaults->Release();real->Release();factory->Release();}
inline ULONG Device::AddRef(){return InterlockedIncrement(&refs);}
inline ULONG Device::Release(){ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
inline HRESULT Device::QueryInterface(REFIID id,void** p){if(!p)return E_POINTER;*p=nullptr;if(id==__uuidof(IUnknown)||id==__uuidof(IDirect3DDevice9)){*p=this;AddRef();return S_OK;}return E_NOINTERFACE;}
inline HRESULT Device::GetDirect3D(IDirect3D9** p){if(!p)return D3DERR_INVALIDCALL;*p=factory;factory->AddRef();return S_OK;}
inline HRESULT Device::TestCooperativeLevel() {
    LONG last=InterlockedCompareExchange(&presentStatus,0,0);
    if(!resetPending&&(last==S_OK||last==S_PRESENT_OCCLUDED))return S_OK;
    HRESULT state=real->CheckDeviceState(window);
    HRESULT result=LegacyCooperativeResult(state,resetPending,GetForegroundWindow()==window);
    if(result==S_OK)InterlockedExchange(&presentStatus,state);
    return result;
}
inline HRESULT Device::RestoreLegacyDefaults() {
    // State blocks do not restore render targets or the depth surface. Bind
    // the actual implicit surfaces before the engine rebuilds its AA/HDR ones.
    IDirect3DSurface9* back=nullptr;HRESULT hr=real->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
    if(FAILED(hr))return hr;
    D3DSURFACE_DESC desc={};hr=back->GetDesc(&desc);
    if(SUCCEEDED(hr))hr=real->SetRenderTarget(0,back);back->Release();
    if(FAILED(hr))return hr;
    D3DCAPS9 caps={};hr=real->GetDeviceCaps(&caps);if(FAILED(hr))return hr;
    for(DWORD i=1;i<caps.NumSimultaneousRTs;++i){hr=real->SetRenderTarget(i,nullptr);if(FAILED(hr))return hr;}
    hr=real->SetDepthStencilSurface(defaultDepth);if(FAILED(hr))return hr;
    if(resetDefaults){hr=resetDefaults->Apply();if(FAILED(hr))return hr;}
    D3DVIEWPORT9 viewport={0,0,desc.Width,desc.Height,0.0f,1.0f};
    hr=real->SetViewport(&viewport);if(FAILED(hr))return hr;
    RECT rect={0,0,static_cast<LONG>(desc.Width),static_cast<LONG>(desc.Height)};
    hr=real->SetScissorRect(&rect);if(FAILED(hr))return hr;
    memset(samplerLOD,0,sizeof(samplerLOD));SyncBindings();return S_OK;
}
inline HRESULT Device::Reset(D3DPRESENT_PARAMETERS* p) {
    if(!p)return D3DERR_INVALIDCALL;Guard g(mutex);auto requested=*p;
    diagnostics.Reset();
    // A changed engine AA level requires new offscreen targets, not a display
    // mode switch. Preserve the native resource-rebuild path without ResetEx.
    if(havePresentation&&!memcmp(p,&presentation,sizeof(*p))&&TestCooperativeLevel()==D3D_OK&&
       SUCCEEDED(RestoreLegacyDefaults())) {
        ++targetRefreshCount;
        Log("D3D9Ex engine target refresh without display reset; retained resources=%u",static_cast<unsigned>(wrappers.size()));
        return S_OK;
    }
    if(defaultDepth){defaultDepth->Release();defaultDepth=nullptr;}
    auto mode=FullscreenMode(*p);HRESULT hr=real->ResetEx(p,p->Windowed?nullptr:&mode);++resetCount;
    if(SUCCEEDED(hr)) {
        presentation=requested;havePresentation=true;resetPending=false;real->GetDepthStencilSurface(&defaultDepth);
        HRESULT restored=RestoreLegacyDefaults();
        if(FAILED(restored))Log("D3D9Ex legacy defaults restoration failed=%08lx",restored);
        InterlockedExchange(&presentStatus,S_OK);
    } else {
        // A later healthy CheckDeviceState does not prove that a rejected
        // Reset installed the requested swap chain. Native recovery must retry
        // before rebuilding targets from those dimensions.
        havePresentation=false;resetPending=true;InterlockedExchange(&presentStatus,hr);
    }
    Log("D3D9Ex ResetEx result=%08lx resource objects retained=%u",hr,static_cast<unsigned>(wrappers.size()));return hr;
}
inline HRESULT Device::Present(const RECT* s,const RECT* d,HWND w,const RGNDATA* dirty) {
    if(diagnostics.HasPresentWork()){Guard g(mutex);diagnostics.BeforePresent();}
    HRESULT hr=real->PresentEx(s,d,w,dirty,0);
    // Never extend the bridge mutex across the driver's Present/synchronization.
    if(diagnostics.HasPresentWork()){Guard g(mutex);diagnostics.AfterPresent(hr);}
    LONG previous=InterlockedExchange(&presentStatus,hr);
    if(hr!=previous&&hr!=S_OK)Log("D3D9Ex Present status=%08lx (occlusion does not rebuild resources)",hr);
    return hr;
}
inline HRESULT Device::CreateTexture(UINT w,UINT h,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DTexture9** p,HANDLE* shared) {
    if(pool!=D3DPOOL_MANAGED)return real->CreateTexture(w,h,levels,usage,format,pool,p,shared);
    if(!p)return D3DERR_INVALIDCALL;*p=nullptr;if(shared||(usage&(D3DUSAGE_DYNAMIC|D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return D3DERR_INVALIDCALL;
    Guard guard(mutex);IDirect3DTexture9 *c=nullptr,*g=nullptr;
    HRESULT hr=real->CreateTexture(w,h,levels,usage,format,D3DPOOL_DEFAULT,&g,nullptr);
    if(SUCCEEDED(hr))hr=real->CreateTexture(w,h,(usage&D3DUSAGE_AUTOGENMIPMAP)?1:levels,0,format,D3DPOOL_SYSTEMMEM,&c,nullptr);
    if(SUCCEEDED(hr)){hr=Allocate<Texture>(p,this,c,g);if(SUCCEEDED(hr))return hr;}
    if(c)c->Release();if(g)g->Release();Log("D3D9Ex managed texture failed=%08lx format=%u %ux%u",hr,format,w,h);return hr;
}
inline HRESULT Device::CreateCubeTexture(UINT size,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DCubeTexture9** p,HANDLE* shared) {
    if(pool!=D3DPOOL_MANAGED)return real->CreateCubeTexture(size,levels,usage,format,pool,p,shared);
    if(!p)return D3DERR_INVALIDCALL;*p=nullptr;if(shared||(usage&(D3DUSAGE_DYNAMIC|D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return D3DERR_INVALIDCALL;
    Guard guard(mutex);IDirect3DCubeTexture9 *c=nullptr,*g=nullptr;
    HRESULT hr=real->CreateCubeTexture(size,levels,usage,format,D3DPOOL_DEFAULT,&g,nullptr);
    if(SUCCEEDED(hr))hr=real->CreateCubeTexture(size,(usage&D3DUSAGE_AUTOGENMIPMAP)?1:levels,0,format,D3DPOOL_SYSTEMMEM,&c,nullptr);
    if(SUCCEEDED(hr)){hr=Allocate<CubeTexture>(p,this,c,g);if(SUCCEEDED(hr))return hr;}
    if(c)c->Release();if(g)g->Release();return hr;
}
inline HRESULT Device::CreateVolumeTexture(UINT w,UINT h,UINT depth,UINT levels,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DVolumeTexture9** p,HANDLE* shared) {
    if(pool!=D3DPOOL_MANAGED)return real->CreateVolumeTexture(w,h,depth,levels,usage,format,pool,p,shared);
    if(!p)return D3DERR_INVALIDCALL;*p=nullptr;if(shared||usage)return D3DERR_INVALIDCALL;
    Guard guard(mutex);IDirect3DVolumeTexture9 *c=nullptr,*g=nullptr;
    HRESULT hr=real->CreateVolumeTexture(w,h,depth,levels,usage,format,D3DPOOL_DEFAULT,&g,nullptr);
    if(SUCCEEDED(hr))hr=real->CreateVolumeTexture(w,h,depth,levels,0,format,D3DPOOL_SYSTEMMEM,&c,nullptr);
    if(SUCCEEDED(hr)){hr=Allocate<VolumeTexture>(p,this,c,g);if(SUCCEEDED(hr))return hr;}
    if(c)c->Release();if(g)g->Release();return hr;
}
inline HRESULT Device::CreateVertexBuffer(UINT size,DWORD usage,DWORD fvf,D3DPOOL pool,IDirect3DVertexBuffer9** p,HANDLE* shared) {
    if(pool!=D3DPOOL_MANAGED)return real->CreateVertexBuffer(size,usage,fvf,pool,p,shared);
    if(!p)return D3DERR_INVALIDCALL;*p=nullptr;if(shared||(usage&D3DUSAGE_DYNAMIC))return D3DERR_INVALIDCALL;
    Guard guard(mutex);IDirect3DVertexBuffer9* g=nullptr;HRESULT hr=real->CreateVertexBuffer(size,usage,fvf,D3DPOOL_DEFAULT,&g,nullptr);
    if(SUCCEEDED(hr)){hr=Allocate<VertexBuffer>(p,this,g,size);if(FAILED(hr))g->Release();}return hr;
}
inline HRESULT Device::CreateIndexBuffer(UINT size,DWORD usage,D3DFORMAT format,D3DPOOL pool,IDirect3DIndexBuffer9** p,HANDLE* shared) {
    if(pool!=D3DPOOL_MANAGED)return real->CreateIndexBuffer(size,usage,format,pool,p,shared);
    if(!p)return D3DERR_INVALIDCALL;*p=nullptr;if(shared||(usage&D3DUSAGE_DYNAMIC))return D3DERR_INVALIDCALL;
    Guard guard(mutex);IDirect3DIndexBuffer9* g=nullptr;HRESULT hr=real->CreateIndexBuffer(size,usage,format,D3DPOOL_DEFAULT,&g,nullptr);
    if(SUCCEEDED(hr)){hr=Allocate<IndexBuffer>(p,this,g,size);if(FAILED(hr))g->Release();}return hr;
}
// Managed surfaces cannot be update/render targets in the D3D9 contract. They
// are used only through their texture's locks. Reject them before native calls.
inline bool IsManagedSurface(IDirect3DSurface9* s){D3DSURFACE_DESC d={};return s&&SUCCEEDED(s->GetDesc(&d))&&d.Pool==D3DPOOL_MANAGED;}
inline HRESULT Device::UpdateSurface(IDirect3DSurface9* s,const RECT* r,IDirect3DSurface9* d,const POINT* p){if(IsManagedSurface(s)||IsManagedSurface(d))return D3DERR_INVALIDCALL;return real->UpdateSurface(s,r,d,p);}
inline HRESULT Device::UpdateTexture(IDirect3DBaseTexture9* s,IDirect3DBaseTexture9* d){Guard g(mutex);if(Find(s)||Find(d))return D3DERR_INVALIDCALL;auto upload=diagnostics.BeginUpload();HRESULT hr=real->UpdateTexture(s,d);diagnostics.EndUpload(upload);return hr;}
inline HRESULT Device::GetRenderTargetData(IDirect3DSurface9* s,IDirect3DSurface9* d){if(IsManagedSurface(s)||IsManagedSurface(d))return D3DERR_INVALIDCALL;return real->GetRenderTargetData(s,d);}
inline HRESULT Device::StretchRect(IDirect3DSurface9* s,const RECT* sr,IDirect3DSurface9* d,const RECT* dr,D3DTEXTUREFILTERTYPE f){if(IsManagedSurface(s)||IsManagedSurface(d))return D3DERR_INVALIDCALL;return real->StretchRect(s,sr,d,dr,f);}
inline HRESULT Device::ColorFill(IDirect3DSurface9* s,const RECT* r,D3DCOLOR c){if(IsManagedSurface(s))return D3DERR_INVALIDCALL;return real->ColorFill(s,r,c);}
inline HRESULT Device::SetRenderTarget(DWORD i,IDirect3DSurface9* s){if(IsManagedSurface(s))return D3DERR_INVALIDCALL;return real->SetRenderTarget(i,s);}
inline HRESULT Device::SetDepthStencilSurface(IDirect3DSurface9* s){if(IsManagedSurface(s))return D3DERR_INVALIDCALL;return real->SetDepthStencilSurface(s);}
inline void Device::ApplyLOD(UINT stage,bool rebindTexture){
    auto i=bound.find(stage);DWORD lod=0;
    if(i!=bound.end()&&i->second){
        if(rebindTexture)real->SetTexture(stage,i->second->SamplingTexture());
        lod=i->second->lod;
    }
    if(stage<_countof(samplerLOD))real->SetSamplerState(stage,D3DSAMP_MAXMIPLEVEL,samplerLOD[stage]>lod?samplerLOD[stage]-lod:0);
}
inline HRESULT Device::SetTexture(DWORD stage,IDirect3DBaseTexture9* t){
    Guard g(mutex);auto m=Find(t);
    HRESULT hr=real->SetTexture(stage,m?m->SamplingTexture():t);
    if(SUCCEEDED(hr)){
        Bind(recording?recorded:bound,stage,m);
        // The native texture is already bound. Only the sampler LOD needs
        // adjustment here; SetLOD and state-block restoration still rebind.
        if(!recording)ApplyLOD(stage,false);
    }
    return hr;
}
inline HRESULT Device::GetTexture(DWORD stage,IDirect3DBaseTexture9** t){Guard g(mutex);HRESULT hr=real->GetTexture(stage,t);if(SUCCEEDED(hr))Rewrap(t);return hr;}
inline HRESULT Device::SetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD v){Guard g(mutex);DWORD effective=v;if(!recording&&t==D3DSAMP_MAXMIPLEVEL){auto b=bound.find(s);DWORD lod=b!=bound.end()&&b->second?b->second->lod:0;effective=v>lod?v-lod:0;}HRESULT hr=real->SetSamplerState(s,t,effective);if(SUCCEEDED(hr)&&t==D3DSAMP_MAXMIPLEVEL&&s<_countof(samplerLOD)){if(recording)recordedLOD[s]=v;else samplerLOD[s]=v;}return hr;}
inline HRESULT Device::GetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD* p){Guard g(mutex);HRESULT hr=real->GetSamplerState(s,t,p);if(SUCCEEDED(hr)&&t==D3DSAMP_MAXMIPLEVEL&&s<_countof(samplerLOD))*p=samplerLOD[s];return hr;}
inline HRESULT Device::SetStreamSource(UINT s,IDirect3DVertexBuffer9* b,UINT o,UINT stride){
    Guard g(mutex);auto managed=Find(b);
    auto buffer=managed?static_cast<IDirect3DVertexBuffer9*>(managed->gpu):b;
    HRESULT hr=real->SetStreamSource(s,buffer,o,stride);
    if(SUCCEEDED(hr))Bind(recording?recorded:bound,StreamKey+s,managed);return hr;
}
inline HRESULT Device::GetStreamSource(UINT s,IDirect3DVertexBuffer9** b,UINT* o,UINT* stride){Guard g(mutex);HRESULT hr=real->GetStreamSource(s,b,o,stride);if(SUCCEEDED(hr))Rewrap(b);return hr;}
inline HRESULT Device::SetIndices(IDirect3DIndexBuffer9* b){
    Guard g(mutex);auto managed=Find(b);
    auto buffer=managed?static_cast<IDirect3DIndexBuffer9*>(managed->gpu):b;
    HRESULT hr=real->SetIndices(buffer);
    if(SUCCEEDED(hr))Bind(recording?recorded:bound,IndexKey,managed);return hr;
}
inline HRESULT Device::GetIndices(IDirect3DIndexBuffer9** b){Guard g(mutex);HRESULT hr=real->GetIndices(b);if(SUCCEEDED(hr))Rewrap(b);return hr;}
inline HRESULT Device::Flush(){
    if(!uploadsPending)return S_OK;
    for(auto& b:bound)if(b.second&&b.second->dirty){HRESULT hr=b.second->Upload();if(FAILED(hr))return hr;}
    // A failed upload above leaves the scan pending for the next draw.
    uploadsPending=false;return S_OK;
}
inline HRESULT Device::DrawPrimitive(D3DPRIMITIVETYPE t,UINT start,UINT n){Guard g(mutex);HRESULT hr=Flush();if(FAILED(hr))return hr;diagnostics.Draw();return real->DrawPrimitive(t,start,n);}
inline HRESULT Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE t,INT base,UINT min,UINT count,UINT start,UINT n){Guard g(mutex);HRESULT hr=Flush();if(FAILED(hr))return hr;diagnostics.Draw();return real->DrawIndexedPrimitive(t,base,min,count,start,n);}
inline HRESULT Device::DrawPrimitiveUP(D3DPRIMITIVETYPE t,UINT n,const void* p,UINT stride){Guard g(mutex);HRESULT hr=Flush();if(FAILED(hr))return hr;diagnostics.Draw();hr=real->DrawPrimitiveUP(t,n,p,stride);Bind(bound,StreamKey,nullptr);return hr;}
inline HRESULT Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t,UINT min,UINT count,UINT n,const void* indices,D3DFORMAT f,const void* vertices,UINT stride){Guard g(mutex);HRESULT hr=Flush();if(FAILED(hr))return hr;diagnostics.Draw();hr=real->DrawIndexedPrimitiveUP(t,min,count,n,indices,f,vertices,stride);Bind(bound,StreamKey,nullptr);Bind(bound,IndexKey,nullptr);return hr;}
inline HRESULT Device::ProcessVertices(UINT start,UINT dest,UINT count,IDirect3DVertexBuffer9* b,IDirect3DVertexDeclaration9* d,DWORD f){Guard g(mutex);auto m=Find(b);HRESULT hr=real->ProcessVertices(start,dest,count,Unwrap(b),d,f);if(SUCCEEDED(hr)&&m)hr=static_cast<VertexBuffer*>(m)->ReadGpu();return hr;}
inline void Device::SyncBindings() {
    Bindings fresh;
    for(UINT s=0;s<=260;++s)if(s<16||s>=256){IDirect3DBaseTexture9* t=nullptr;if(SUCCEEDED(real->GetTexture(s,&t))){Bind(fresh,s,FindNative(t));if(t)t->Release();}}
    for(UINT s=0;s<16;++s){IDirect3DVertexBuffer9* b=nullptr;UINT o=0,stride=0;if(SUCCEEDED(real->GetStreamSource(s,&b,&o,&stride))){Bind(fresh,StreamKey+s,FindNative(b));if(b)b->Release();}}
    IDirect3DIndexBuffer9* indices=nullptr;if(SUCCEEDED(real->GetIndices(&indices))){Bind(fresh,IndexKey,FindNative(indices));if(indices)indices->Release();}
    ClearBindings(bound);bound.swap(fresh);uploadsPending=true;
}
class StateBlock final:public IDirect3DStateBlock9 {
    volatile LONG refs=1;
    Device* owner;IDirect3DStateBlock9* real;Bindings held;
    std::map<DWORD,DWORD> samplerValues;
public:
    StateBlock(Device* d,IDirect3DStateBlock9* r,const Bindings& b,D3DSTATEBLOCKTYPE type):owner(d),real(r){
        owner->AddRef();owner->CopyBindings(held,b);
        if(type==0)samplerValues=d->recordedLOD;
        else for(DWORD s=0;s<=260;++s)if((s<16&&type!=D3DSBT_VERTEXSTATE)||(s>=256&&type!=D3DSBT_PIXELSTATE))samplerValues[s]=d->samplerLOD[s];
    }
    ~StateBlock(){Device* d=owner;{Guard g(d->mutex);d->ClearBindings(held);real->Release();}d->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** p) override{if(!p)return E_POINTER;*p=nullptr;if(id==__uuidof(IUnknown)||id==__uuidof(IDirect3DStateBlock9)){*p=this;AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
    ULONG STDMETHODCALLTYPE Release() override{ULONG n=InterlockedDecrement(&refs);if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** p) override{if(!p)return D3DERR_INVALIDCALL;*p=owner;owner->AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE Capture() override{Guard g(owner->mutex);HRESULT hr=real->Capture();if(SUCCEEDED(hr)){owner->CopyBindings(held,owner->bound);for(auto& s:samplerValues)s.second=owner->samplerLOD[s.first];}return hr;}
    HRESULT STDMETHODCALLTYPE Apply() override{Guard g(owner->mutex);HRESULT hr=real->Apply();if(SUCCEEDED(hr)){for(auto& s:samplerValues)owner->samplerLOD[s.first]=s.second;owner->SyncBindings();for(auto& b:owner->bound)if(b.first<StreamKey)owner->ApplyLOD(b.first);}return hr;}
};
inline HRESULT Device::CreateStateBlock(D3DSTATEBLOCKTYPE t,IDirect3DStateBlock9** p){if(!p)return D3DERR_INVALIDCALL;*p=nullptr;Guard g(mutex);IDirect3DStateBlock9* b=nullptr;HRESULT hr=real->CreateStateBlock(t,&b);if(SUCCEEDED(hr)){*p=new(std::nothrow) StateBlock(this,b,bound,t);if(!*p){b->Release();hr=E_OUTOFMEMORY;}}return hr;}
inline HRESULT Device::BeginStateBlock(){Guard g(mutex);HRESULT hr=real->BeginStateBlock();if(SUCCEEDED(hr)){ClearBindings(recorded);recordedLOD.clear();recording=true;}return hr;}
inline HRESULT Device::EndStateBlock(IDirect3DStateBlock9** p){if(!p)return D3DERR_INVALIDCALL;*p=nullptr;Guard g(mutex);IDirect3DStateBlock9* b=nullptr;HRESULT hr=real->EndStateBlock(&b);if(SUCCEEDED(hr)){recording=false;*p=new(std::nothrow) StateBlock(this,b,recorded,static_cast<D3DSTATEBLOCKTYPE>(0));ClearBindings(recorded);recordedLOD.clear();if(!*p){b->Release();hr=E_OUTOFMEMORY;}}return hr;}
} // namespace pm9ex
