// Original movie quads remain unchanged. This adds only generated peripheral
// artwork after the native movie flush and before subsequent native UI layers.
#include "movie_canvas_art.h"
namespace pm {
struct MovieMargins {float left,right;};
static bool MovieCanvasMargins(unsigned width,unsigned height,const float* rect,MovieMargins& out) {
    // rect is the original renderer's left/right/top/bottom in target pixels.
    if(!width||!height||!rect||!(rect[0]>=0&&rect[1]>rect[0]&&rect[1]<=width+1.0f&&
       rect[2]>=-1.0f&&rect[2]<=1.0f&&rect[3]>=height-1.0f&&rect[3]<=height+1.0f))return false;
    const float movieWidth=rect[1]-rect[0];
    if(movieWidth<height*1.77f||movieWidth>height*1.79f)return false;
    // Integral outer bounds: no draw touches a pixel of the original quad.
    out.left=static_cast<float>(static_cast<unsigned>(rect[0]));
    out.right=static_cast<float>(static_cast<unsigned>(rect[1]+0.9999f));
    return out.left>=1&&out.right<width;
}
static const unsigned char* MovieLight(unsigned width,unsigned height,unsigned frames,unsigned& count) {
    count=0;if(width!=1280||height!=720)return nullptr;
    if(frames==427){count=427;return movieLight_logos;}
    if(frames==612){count=612;return movieLight_title;}
    if(frames==1808){count=1808;return movieLight_ambient;}
    return nullptr;
}
}
typedef void (__thiscall* NativeMovieFlush)(void*);
static NativeMovieFlush nativeMovieFlush;
static IDirect3DDevice9* movieCanvasDevice;
static IDirect3DTexture9* movieCanvasTexture;
static IDirect3DStateBlock9* movieCanvasState;
static unsigned movieCanvasReportedFrames,movieCanvasReportedWidth,movieCanvasReportedHeight;
static void ReleaseMovieCanvas() {
    if(movieCanvasState){movieCanvasState->Release();movieCanvasState=nullptr;}
    if(movieCanvasTexture){movieCanvasTexture->Release();movieCanvasTexture=nullptr;}
    movieCanvasDevice=nullptr;
}
static bool PrepareMovieCanvas(IDirect3DDevice9* device) {
    if(movieCanvasDevice!=device)ReleaseMovieCanvas();
    movieCanvasDevice=device;
    if(!movieCanvasTexture) {
        if(FAILED(device->CreateTexture(256,512,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&movieCanvasTexture,nullptr)))return false;
        D3DLOCKED_RECT lock={};
        if(FAILED(movieCanvasTexture->LockRect(0,&lock,nullptr,0))){ReleaseMovieCanvas();return false;}
        unsigned pixel=0;
        for(const auto& run:movieCanvasRuns) {
            const unsigned rgb=run[1],r=(rgb>>11)&31,g=(rgb>>5)&63,b=rgb&31;
            const DWORD color=0xff000000|((r*255/31)<<16)|((g*255/63)<<8)|(b*255/31);
            for(unsigned n=0;n<run[0];++n,++pixel)
                reinterpret_cast<DWORD*>(static_cast<BYTE*>(lock.pBits)+(pixel/256)*lock.Pitch)[pixel%256]=color;
        }
        movieCanvasTexture->UnlockRect(0);
    }
    if(!movieCanvasState&&FAILED(device->CreateStateBlock(D3DSBT_ALL,&movieCanvasState)))return false;
    return true;
}
struct MovieCanvasVertex {float x,y,z,rhw;DWORD color;float u,v;};
static HRESULT MovieCanvasStrip(IDirect3DDevice9* device,float x0,float x1,unsigned height,float u0,float u1,DWORD color) {
    MovieCanvasVertex v[]={
        {x0-.5f,-.5f,0,1,color,u0,0},{x1-.5f,-.5f,0,1,color,u1,0},
        {x0-.5f,height-.5f,0,1,color,u0,1},{x1-.5f,height-.5f,0,1,color,u1,1}};
    return device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0]));
}
static void DrawMovieCanvas(BYTE* movie) {
    BYTE* data=*reinterpret_cast<BYTE**>(movie+0x10);
    const unsigned* bink=data?*reinterpret_cast<unsigned**>(data+0xc):nullptr;
    if(!bink)return;
    unsigned count=0;const auto light=pm::MovieLight(bink[0],bink[1],bink[2],count);
    if(!light)return;
    auto device=*reinterpret_cast<IDirect3DDevice9**>(engineBase+0x1276d38);
    if(!device)return;
    IDirect3DSurface9* target=nullptr;
    if(FAILED(device->GetRenderTarget(0,&target))||!target)return;
    D3DSURFACE_DESC desc={};HRESULT hr=target->GetDesc(&desc);target->Release();
    pm::MovieMargins margins={};
    if(FAILED(hr)||!pm::MovieCanvasMargins(desc.Width,desc.Height,reinterpret_cast<float*>(movie+0x244),margins)||
       !PrepareMovieCanvas(device)||FAILED(movieCanvasState->Capture()))return;
    D3DVIEWPORT9 oldViewport={};
    if(FAILED(device->GetViewport(&oldViewport)))return;
    IDirect3DVertexBuffer9* stream=nullptr;UINT offset=0,stride=0;
    if(FAILED(device->GetStreamSource(0,&stream,&offset,&stride)))return;
    D3DVIEWPORT9 full={0,0,desc.Width,desc.Height,0.0f,1.0f};
    bool ok=SUCCEEDED(device->SetViewport(&full));
    auto rs=[&](D3DRENDERSTATETYPE key,DWORD value){if(FAILED(device->SetRenderState(key,value)))ok=false;};
    rs(D3DRS_ZENABLE,FALSE);rs(D3DRS_ZWRITEENABLE,FALSE);rs(D3DRS_STENCILENABLE,FALSE);
    rs(D3DRS_ALPHATESTENABLE,FALSE);rs(D3DRS_ALPHABLENDENABLE,FALSE);rs(D3DRS_FOGENABLE,FALSE);
    rs(D3DRS_LIGHTING,FALSE);rs(D3DRS_SCISSORTESTENABLE,FALSE);rs(D3DRS_CLIPPLANEENABLE,0);
    rs(D3DRS_CULLMODE,D3DCULL_NONE);rs(D3DRS_FILLMODE,D3DFILL_SOLID);rs(D3DRS_SRGBWRITEENABLE,FALSE);
    rs(D3DRS_COLORWRITEENABLE,0xf);
    if(FAILED(device->SetVertexShader(nullptr))||FAILED(device->SetPixelShader(nullptr))||
       FAILED(device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1))||
       FAILED(device->SetTexture(0,movieCanvasTexture)))ok=false;
    auto ts=[&](DWORD stage,D3DTEXTURESTAGESTATETYPE key,DWORD value){if(FAILED(device->SetTextureStageState(stage,key,value)))ok=false;};
    ts(0,D3DTSS_COLOROP,D3DTOP_MODULATE);ts(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);ts(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    ts(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG2);ts(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    ts(0,D3DTSS_TEXCOORDINDEX,0);ts(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);ts(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    auto ss=[&](D3DSAMPLERSTATETYPE key,DWORD value){if(FAILED(device->SetSamplerState(0,key,value)))ok=false;};
    ss(D3DSAMP_MINFILTER,D3DTEXF_LINEAR);ss(D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);ss(D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    ss(D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);ss(D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);ss(D3DSAMP_SRGBTEXTURE,0);
    // Native FrameNum advances after decode; use the last decoded frame.
    unsigned frame=bink[3]?bink[3]-1:0;frame=std::min(frame,count-1);
    DWORD color=0xff000000|light[frame]*0x010101;
    hr=ok?MovieCanvasStrip(device,0,margins.left,desc.Height,.5f/256,127.5f/256,color):E_FAIL;
    if(SUCCEEDED(hr))hr=MovieCanvasStrip(device,margins.right,static_cast<float>(desc.Width),desc.Height,128.5f/256,255.5f/256,color);
    movieCanvasState->Apply();device->SetViewport(&oldViewport);
    // DrawPrimitiveUP clears stream 0; restore it explicitly as well as the
    // complete state block, so native engine state caches remain accurate.
    device->SetStreamSource(0,stream,offset,stride);if(stream)stream->Release();
    if(movieCanvasReportedFrames!=count||movieCanvasReportedWidth!=desc.Width||movieCanvasReportedHeight!=desc.Height) {
        Log("movie peripheral canvas frames=%u target=%ux%u original=%.1f..%.1f result=%08lx; original quad unchanged",
            count,desc.Width,desc.Height,margins.left,margins.right,static_cast<unsigned long>(hr));
        movieCanvasReportedFrames=count;movieCanvasReportedWidth=desc.Width;movieCanvasReportedHeight=desc.Height;
    }
}
static void __stdcall MovieCanvasAfterFlush(void* renderer,BYTE* movie) {
    nativeMovieFlush(renderer);DrawMovieCanvas(movie);
}
static __declspec(naked) void MovieCanvasFlushCall() {
    __asm {
        push ebx // original movie owner in the fingerprinted draw function
        push ecx // native flush this pointer
        call MovieCanvasAfterFlush
        ret
    }
}
