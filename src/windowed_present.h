// Preserve the selected render size and aspect when a window has a different
// shape. The engine keeps its original backbuffer; only presentation is fitted.
// No 3D/UI target, viewport, input coordinates, or display mode is replaced.
class WindowedPresentation {
    IDirect3DSwapChain9* chain=nullptr;
    IDirect3DSurface9* target=nullptr;
    UINT width=0,height=0;
    HWND window=nullptr;
public:
    ~WindowedPresentation(){Release();}
    void Release() {
        if(target){target->Release();target=nullptr;}
        if(chain){chain->Release();chain=nullptr;}
        width=height=0;window=nullptr;
    }
    static RECT Fit(UINT sourceWidth,UINT sourceHeight,UINT clientWidth,UINT clientHeight) {
        UINT w=clientWidth,h=clientHeight;
        if(uint64_t(sourceWidth)*clientHeight>uint64_t(clientWidth)*sourceHeight)
            h=static_cast<UINT>((uint64_t(clientWidth)*sourceHeight+sourceWidth/2)/sourceWidth);
        else w=static_cast<UINT>((uint64_t(clientHeight)*sourceWidth+sourceHeight/2)/sourceHeight);
        LONG x=static_cast<LONG>((clientWidth-w)/2),y=static_cast<LONG>((clientHeight-h)/2);
        return {x,y,x+static_cast<LONG>(w),y+static_cast<LONG>(h)};
    }
    HRESULT Compose(IDirect3DDevice9* device,const D3DPRESENT_PARAMETERS& p,HWND destination,bool& fitted) {
        fitted=false;
        RECT client={};
        if(!p.Windowed||!GetClientRect(destination,&client)||client.right<=0||client.bottom<=0) {
            Release();return S_OK;
        }
        UINT clientWidth=client.right,clientHeight=client.bottom;
        if(uint64_t(p.BackBufferWidth)*clientHeight==uint64_t(clientWidth)*p.BackBufferHeight) {
            Release();return S_OK;
        }
        if(!p.BackBufferWidth||!p.BackBufferHeight)return D3DERR_INVALIDCALL;
        if(chain&&(width!=clientWidth||height!=clientHeight||window!=destination))Release();
        if(!chain) {
            D3DPRESENT_PARAMETERS display=p;
            display.BackBufferWidth=clientWidth;display.BackBufferHeight=clientHeight;
            display.BackBufferCount=1;display.MultiSampleType=D3DMULTISAMPLE_NONE;display.MultiSampleQuality=0;
            display.SwapEffect=D3DSWAPEFFECT_DISCARD;display.hDeviceWindow=destination;
            display.EnableAutoDepthStencil=FALSE;display.Flags=0;display.FullScreen_RefreshRateInHz=0;
            HRESULT hr=device->CreateAdditionalSwapChain(&display,&chain);
            if(FAILED(hr))return hr;
            hr=chain->GetBackBuffer(0,D3DBACKBUFFER_TYPE_MONO,&target);
            if(FAILED(hr)){Release();return hr;}
            width=clientWidth;height=clientHeight;window=destination;
            Log("window presentation fit render=%ux%u client=%ux%u; original aspect preserved",
                p.BackBufferWidth,p.BackBufferHeight,width,height);
        }
        IDirect3DSurface9* source=nullptr;
        HRESULT hr=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source);
        if(FAILED(hr))return hr;
        RECT fit=Fit(p.BackBufferWidth,p.BackBufferHeight,width,height);
        hr=device->ColorFill(target,nullptr,0xff000000);
        if(SUCCEEDED(hr))hr=device->StretchRect(source,nullptr,target,&fit,D3DTEXF_LINEAR);
        source->Release();fitted=SUCCEEDED(hr);return hr;
    }
    HRESULT Present(IDirect3DDevice9* device,const D3DPRESENT_PARAMETERS& p,HWND destination) {
        bool fitted=false;HRESULT hr=Compose(device,p,destination,fitted);
        if(FAILED(hr))return hr;
        return fitted?chain->Present(nullptr,nullptr,nullptr,nullptr,0):device->Present(nullptr,nullptr,nullptr,nullptr);
    }
    IDirect3DSurface9* TargetForReadback() const {return target;}
};
static WindowedPresentation windowedPresentation;
