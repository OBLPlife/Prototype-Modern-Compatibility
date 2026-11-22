// PrototypeModern 0.3.3 development build: original compatibility work.
// Game-specific evidence: ../evidence/input-memory-decompile.c.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <mmreg.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>
#include <dxgi1_4.h>
#include <dxcore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <algorithm>
#include <new>
#include <vector>
#include <string>

struct TraceSite { uint32_t site, target; };
struct Profile {
    uint32_t magic, version, inputRva, memoryRva, d3dCreateRva, traceCount;
    TraceSite traces[1];
};
static const uint32_t kMagic = 0x314d4350; // PCM1
static HMODULE selfModule;
static BYTE* engineBase;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static SRWLOCK logLock = SRWLOCK_INIT;
static SRWLOCK inputLock = SRWLOCK_INIT;
static ULONGLONG started, inputRefresh;
static bool inputCacheValid;
static bool inputRetryPending;
static volatile LONG inputDevicesChanged=1;
static std::vector<DWORD> inputProducts;
static HWND gameWindow;
static HMONITOR selectedOutput;
static D3DDISPLAYMODE outputDesktopMode;
static volatile LONG traceDeviceEpoch;
static void InvalidateInputProducts() { InterlockedExchange(&inputDevicesChanged,1);InterlockedIncrement(&traceDeviceEpoch); }
typedef IDirect3D9* (WINAPI* Create9Function)(UINT);
static Create9Function originalCreate9;
static volatile LONG testStep=1;
static unsigned long long logSequence;

static void Log(const char* format, ...) {
    DWORD savedError = GetLastError();
    char buffer[1024];
    AcquireSRWLockExclusive(&logLock);
    int prefix = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "[%llu ms tid=%lu] [seq=%llu step=T%02ld] ", GetTickCount64() - started,
        GetCurrentThreadId(),++logSequence,InterlockedCompareExchange(&testStep,0,0));
    if (prefix < 0) prefix = 0;
    va_list args; va_start(args, format);
    _vsnprintf_s(buffer + prefix, sizeof(buffer) - prefix, _TRUNCATE, format, args);
    va_end(args);
    size_t length = strnlen_s(buffer, sizeof(buffer) - 3);
    buffer[length++] = '\r'; buffer[length++] = '\n';
    if (logFile != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(logFile, buffer, static_cast<DWORD>(length), &written, nullptr);
        // WriteFile makes the record available to a live reader. Forcing a disk
        // cache flush here stalls the render/window thread for every record.
    }
    ReleaseSRWLockExclusive(&logLock);
    SetLastError(savedError);
}

#include "operation_log.h"
static void TraceWindowEvent(HWND,UINT,WPARAM,LPARAM);

static void LogDisplayParameters(const char* phase,const D3DPRESENT_PARAMETERS* parameters) {
    if(!parameters)return;
    HWND window=parameters->hDeviceWindow?parameters->hDeviceWindow:gameWindow;
    RECT client={},outer={};GetClientRect(window,&client);GetWindowRect(window,&outer);
    Log("display %s request=%ux%u fullscreen=%u refresh=%u client=%ldx%ld outer=%ld,%ld,%ld,%ld style=%08lx minimized=%u foreground=%u",
        phase,parameters->BackBufferWidth,parameters->BackBufferHeight,!parameters->Windowed,
        parameters->FullScreen_RefreshRateInHz,client.right-client.left,client.bottom-client.top,
        outer.left,outer.top,outer.right,outer.bottom,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),
        IsIconic(window)!=FALSE,GetForegroundWindow()==window);
}
static void LogBackBuffer(IDirect3DDevice9* device) {
    DWORD savedError=GetLastError();
    if(device) {
        IDirect3DSurface9* surface=nullptr;
        if(SUCCEEDED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&surface))&&surface) {
            D3DSURFACE_DESC desc={};
            if(SUCCEEDED(surface->GetDesc(&desc)))Log("display actual backbuffer=%ux%u format=%u",desc.Width,desc.Height,desc.Format);
            surface->Release();
        }
    }
    SetLastError(savedError);
}

#include "windowed_present.h"
#include "presentation.h"

static HMODULE LoadSystem(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, _countof(path));
    if (!n || n >= _countof(path) - 2) return nullptr;
    wcscat_s(path, L"\\");
    if (wcscat_s(path, name)) return nullptr;
    return LoadLibraryW(path);
}

static bool Hex4(const wchar_t* text, DWORD& value) {
    if (!text) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        wchar_t c = text[i];
        unsigned n = c >= L'0' && c <= L'9' ? c - L'0' :
            c >= L'A' && c <= L'F' ? c - L'A' + 10 : 16;
        if (n >= 16) return false;
        value = (value << 4) | n;
    }
    return true;
}

// Same VID/PID + IG_ classification as the original function, using local PnP
// instance IDs instead of starting a synchronous WMI enumeration per device.
static void RefreshInputProducts() {
    ULONGLONG begin = GetTickCount64();
    std::vector<DWORD> products;
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) {
        Log("input enumeration failed error=%lu; retain previous cache", GetLastError());
        inputRefresh = begin;
        inputRetryPending=true;
        return;
    }
    bool complete = true;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA info = {}; info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInfo(devices, index, &info)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) complete = false;
            break;
        }
        wchar_t idBuffer[512]; DWORD needed = 0;
        const wchar_t* id = idBuffer;
        std::vector<wchar_t> large;
        if (!SetupDiGetDeviceInstanceIdW(devices, &info, idBuffer, _countof(idBuffer), &needed)) {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !needed) continue;
            large.resize(needed);
            if (!SetupDiGetDeviceInstanceIdW(devices, &info, large.data(), needed, nullptr)) continue;
            id = large.data();
        }
        std::vector<wchar_t> upper(id, id + wcslen(id) + 1);
        CharUpperBuffW(upper.data(), static_cast<DWORD>(upper.size() - 1));
        if (!wcsstr(upper.data(), L"IG_")) continue;
        const wchar_t* vendor = wcsstr(upper.data(), L"VID_");
        const wchar_t* product = wcsstr(upper.data(), L"PID_");
        DWORD vid, pid;
        if (Hex4(vendor ? vendor + 4 : nullptr, vid) && Hex4(product ? product + 4 : nullptr, pid))
            products.push_back((pid << 16) | vid);
    }
    SetupDiDestroyDeviceInfoList(devices);
    if (complete) {
        std::sort(products.begin(), products.end());
        products.erase(std::unique(products.begin(), products.end()), products.end());
        inputProducts.swap(products);
        inputCacheValid = true;
    }
    inputRetryPending=!complete;
    inputRefresh = GetTickCount64();
    Log("input local enumeration complete=%u products=%u elapsed=%llu ms",
        complete, static_cast<unsigned>(inputProducts.size()), inputRefresh - begin);
}

static BOOL __stdcall IsXInputDevice(const GUID* product) {
    if (!product) return FALSE;
    AcquireSRWLockExclusive(&inputLock);
    bool changed=InterlockedExchange(&inputDevicesChanged,0)!=0;
    if(changed || ((!inputCacheValid||inputRetryPending) && GetTickCount64()-inputRefresh>=2000)) RefreshInputProducts();
    BOOL result = std::binary_search(inputProducts.begin(), inputProducts.end(), product->Data1);
    ReleaseSRWLockExclusive(&inputLock);
    // Do not log device instance IDs, serial numbers or other device identifiers.
    return result;
}

// A display/output is selected first. Adapter identity comes from the driver's
// monitor association, not from a model name, generation or vendor allowlist.
static UINT SelectAdapter(IDirect3D9* d3d) {
    UINT count = d3d->GetAdapterCount();
    for (UINT i = 0; i < count; ++i)
        if (d3d->GetAdapterMonitor(i) == selectedOutput) return i;
    Log("selected output is not exposed by D3D9; retain driver's primary adapter");
    return D3DADAPTER_DEFAULT;
}

#include "modern_device.h"

// Keep enumeration/capability queries and device creation on the selected
// output. Ex changes resource-loss behavior, not the requested display mode.
class OutputDirect3D final : public IDirect3D9 {
    IDirect3D9* real;
    IDirect3D9Ex* extended;
    UINT selected;
    UINT selectedEx;
    volatile LONG refs;
    UINT Map(UINT adapter) const {
        if (adapter == 0) return selected;
        return adapter == selected ? 0 : adapter;
    }
public:
    OutputDirect3D(IDirect3D9* object, UINT adapter,IDirect3D9Ex* ex=nullptr) :
        real(object),extended(ex),selected(adapter),selectedEx(ex?SelectAdapter(ex):0),refs(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDirect3D9)) {
            *result = static_cast<IDirect3D9*>(this); AddRef(); return S_OK;
        }
        *result = nullptr;
        return E_NOINTERFACE; // Direct3DCreate9 exposes the IDirect3D9 contract.
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&refs);
        if (!count) { if(extended)extended->Release();real->Release(); delete this; }
        return count;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* init) override { return real->RegisterSoftwareDevice(init); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override {
        return real->GetAdapterIdentifier(Map(a), flags, id);
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a, D3DFORMAT f) override { return real->GetAdapterModeCount(Map(a), f); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* mode) override {
        return real->EnumAdapterModes(Map(a), f, m, mode);
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* mode) override { return real->GetAdapterDisplayMode(Map(a), mode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT b, BOOL w) override {
        return real->CheckDeviceType(Map(a), t, f, b, w);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT f, DWORD u, D3DRESOURCETYPE r, D3DFORMAT c) override {
        return real->CheckDeviceFormat(Map(a), t, f, u, r, c);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL w, D3DMULTISAMPLE_TYPE m, DWORD* q) override {
        return real->CheckDeviceMultiSampleType(Map(a), t, f, w, m, q);
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT r, D3DFORMAT d) override {
        return real->CheckDepthStencilMatch(Map(a), t, f, r, d);
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d) override {
        return real->CheckDeviceFormatConversion(Map(a), t, s, d);
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS9* caps) override {
        HRESULT hr = real->GetDeviceCaps(Map(a), t, caps);
        if (SUCCEEDED(hr) && caps) {
            caps->AdapterOrdinal = a;
            caps->MasterAdapterOrdinal = Map(caps->MasterAdapterOrdinal);
        }
        return hr;
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return real->GetAdapterMonitor(Map(a)); }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT a, D3DDEVTYPE t, HWND window, DWORD flags,
            D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** result) override {
        Log("D3D9 CreateDevice logical=%u physical=%u output=%p", a, Map(a), selectedOutput);
        LogDisplayParameters("create",parameters);
        windowedPresentation.Release();
        PreparePresentation(parameters,window,true);
        LogDisplayParameters("create effective",parameters);
        if(!parameters||!result)return D3DERR_INVALIDCALL;
        *result=nullptr;
        D3DPRESENT_PARAMETERS driver=*parameters;
        HRESULT hr=E_FAIL;
        if(extended&&a==0) {
            IDirect3DDevice9Ex* device=nullptr;
            auto mode=pm9ex::FullscreenMode(driver);
            hr=extended->CreateDeviceEx(selectedEx,t,window,flags,&driver,driver.Windowed?nullptr:&mode,&device);
            if(SUCCEEDED(hr)&&device) {
                auto bridge=new(std::nothrow) pm9ex::Device(device,this,window);
                if(bridge&&bridge->resetDefaults){*result=bridge;Log("D3D9Ex resource-preserving device enabled; exclusive=%u",!parameters->Windowed);}
                else {if(bridge)bridge->Release();else device->Release();hr=E_OUTOFMEMORY;}
            }
            if(FAILED(hr))Log("D3D9Ex creation unavailable=%08lx; using native D3D9 fallback",static_cast<unsigned long>(hr));
        }
        if(!*result){driver=*parameters;hr=real->CreateDevice(Map(a), t, window, flags, &driver, result);}
        Log("D3D9 CreateDevice result=%08lx", static_cast<unsigned long>(hr));
        if(SUCCEEDED(hr)&&result){
            PlacePresentationWindow(parameters->hDeviceWindow?parameters->hDeviceWindow:window,*parameters,requestedDisplayMode);
            PresentationSucceeded(parameters,*result);LogBackBuffer(*result);
            if(hiddenInitialWindow)Log("initial window reveal deferred until first UI frame");
        }
        return hr;
    }
};

static IDirect3D9* WINAPI CreateForOutput(UINT sdk) {
    Log("D3D9 factory begin SDK=%u", sdk);
    IDirect3D9* real = originalCreate9(sdk);
    if (!real) { Log("D3D9 factory failed"); return nullptr; }
    UINT selected = SelectAdapter(real);
    D3DADAPTER_IDENTIFIER9 id = {};
    if (SUCCEEDED(real->GetAdapterIdentifier(selected, 0, &id)))
        Log("selected GPU=%s vendor=%04lx device=%04lx adapter=%u output=%p",
            id.Description, id.VendorId, id.DeviceId, selected, selectedOutput);
    IDirect3D9Ex* extended=nullptr;
    auto createEx=reinterpret_cast<HRESULT(WINAPI*)(UINT,IDirect3D9Ex**)>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"),"Direct3DCreate9Ex"));
    if(createEx)createEx(sdk,&extended);
    auto wrapper = new(std::nothrow) OutputDirect3D(real, selected,extended);
    if (!wrapper) { if(extended)extended->Release();real->Release(); Log("output adapter mapping allocation failed"); return nullptr; }
    return wrapper;
}

static bool GetAdapterLuid(LUID& luid) {
    HMODULE d3d = GetModuleHandleW(L"d3d9.dll");
    if (!d3d) return false;
    typedef HRESULT (WINAPI* Create9Ex)(UINT, IDirect3D9Ex**);
    auto create = reinterpret_cast<Create9Ex>(GetProcAddress(d3d, "Direct3DCreate9Ex"));
    if (!create) return false;
    IDirect3D9Ex* object = nullptr;
    HRESULT hr = create(D3D_SDK_VERSION, &object);
    if (FAILED(hr) || !object) return false;
    UINT adapter = SelectAdapter(object);
    hr = object->GetAdapterLUID(adapter, &luid);
    object->Release();
    return SUCCEEDED(hr);
}

static bool QueryDXCore(const LUID& luid, uint64_t& bytes, bool& shared) {
    HMODULE library = LoadSystem(L"dxcore.dll");
    if (!library) return false;
    typedef HRESULT (WINAPI* CreateFactory)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory>(GetProcAddress(library, "DXCoreCreateAdapterFactory"));
    IDXCoreAdapterFactory* factory = nullptr;
    IDXCoreAdapter* adapter = nullptr;
    bool ok = false;
    if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) && factory &&
        SUCCEEDED(factory->GetAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter) {
        ok = SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DedicatedAdapterMemory, sizeof(bytes), &bytes));
        if (ok && !bytes) {
            shared = true;
            ok = SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::SharedSystemMemory, sizeof(bytes), &bytes));
        }
    }
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    FreeLibrary(library);
    return ok && bytes != 0;
}

static bool QueryDXGIBudget(const LUID& luid, uint64_t& bytes) {
    HMODULE library = LoadSystem(L"dxgi.dll");
    if (!library) return false;
    typedef HRESULT (WINAPI* CreateFactory)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory>(GetProcAddress(library, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    bool ok = false;
    if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) && factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (FAILED(factory->EnumAdapters1(i, &adapter))) break;
            DXGI_ADAPTER_DESC1 desc = {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
                desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
                IDXGIAdapter3* modern = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&modern)))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
                    if (SUCCEEDED(modern->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                        bytes = info.Budget;
                        ok = bytes != 0;
                    }
                    modern->Release();
                }
                // The x86 SIZE_T field is intentionally not used for >4 GiB.
                adapter->Release();
                break;
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(library);
    return ok;
}

// Original function returns signed MiB, not bytes. Keep the original ABI while
// performing the byte query and conversion with 64-bit arithmetic.
static int __cdecl VideoMemoryMiB() {
    Log("video memory query begin");
    LUID luid = {}; uint64_t bytes = 0; bool shared = false;
    if (!GetAdapterLuid(luid)) { Log("video memory: selected-output D3D9 adapter unavailable, return unknown (0)"); return 0; }
    const char* source = "DXCore dedicated";
    if (QueryDXCore(luid, bytes, shared)) {
        if (shared) source = "DXCore shared (integrated GPU)";
    } else {
        source = "DXGI current budget (estimate, not installed VRAM)";
        if (!QueryDXGIBudget(luid, bytes)) { Log("video memory query failed, return unknown (0)"); return 0; }
    }
    uint64_t mib = bytes / (1024ull * 1024ull);
    int result = static_cast<int>(std::min<uint64_t>(mib, INT_MAX));
    Log("video memory source=%s bytes=%llu MiB=%d", source, bytes, result);
    return result;
}

static bool WriteCode(BYTE* location, const void* replacement, size_t size) {
    DWORD oldProtect;
    if (!VirtualProtect(location, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    memcpy(location, replacement, size);
    FlushInstructionCache(GetCurrentProcess(), location, size);
    DWORD ignored;
    if (!VirtualProtect(location, size, oldProtect, &ignored)) {
        Log("cannot restore code-page protection error=%lu", GetLastError());
        return false;
    }
    return true;
}

struct Edit { BYTE* site; std::vector<BYTE> before, after; };
static Edit Branch(BYTE* site, BYTE opcode, void* target, size_t size) {
    Edit edit; edit.site = site;
    edit.before.assign(site, site + size);
    edit.after.assign(size, 0x90);
    edit.after[0] = opcode;
    uint32_t relative = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(site + 5);
    memcpy(edit.after.data() + 1, &relative, 4);
    return edit;
}

#include "frame_metrics.h"
#include "operation_trace.h"
#include "display_hooks.h"
#include "timing_hooks.h"
#include "system_hooks.h"
#include "physics_hooks.h"
#include "vehicle_hooks.h"
#include "tendril_retirement_clock.h"
#include "tendril_hooks.h"
#include "wheel_hooks.h"
#include "woi_trace_hooks.h"
#include "audio_queue_hooks.h"
#include "dns_lifetime.h"
#include "wait_diagnostics.h"

static void __stdcall Trace(uint32_t callsite) {
    Log("enter engine call RVA=%08lx", static_cast<unsigned long>(callsite));
}

// Save flags, all integer registers, x87 state and SSE registers. Tail-jump to
// the original callee, preserving its original stack and calling convention.
static BYTE* TraceThunk(BYTE* destination, uint32_t callsite, BYTE* target) {
    BYTE* out = destination;
    auto emit = [&](BYTE b) { *out++ = b; };
    auto dword = [&](uint32_t n) { memcpy(out, &n, 4); out += 4; };
    const BYTE start[] = {0x9c,0x60,0x8b,0xec,0x81,0xec,0x20,0x02,0,0,
        0x83,0xe4,0xf0,0x0f,0xae,0x04,0x24,0x68};
    memcpy(out, start, sizeof(start)); out += sizeof(start); dword(callsite);
    emit(0xe8); dword(reinterpret_cast<uintptr_t>(&Trace) - reinterpret_cast<uintptr_t>(out + 4));
    const BYTE end[] = {0x0f,0xae,0x0c,0x24,0x8b,0xe5,0x61,0x9d,0xe9};
    memcpy(out, end, sizeof(end)); out += sizeof(end);
    dword(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(out + 4));
    return out;
}

extern "C" __declspec(dllexport) BOOL __stdcall PM_Initialize(HMODULE engine, const Profile* profile, const uintptr_t* engineArguments) {
    started = GetTickCount64(); engineBase = reinterpret_cast<BYTE*>(engine);
    OpenLog();
    Log("Prototype Modern Compatibility Patch 0.3.3 + A5 audio queue polling + E1 sampled engine diagnostics; A4 wheel/logo, A3 vehicle/input and A2 resource submission retained; English display labels via native text query");
    LogTestSession();
    if (!profile || profile->magic != kMagic || profile->version != 2 || profile->traceCount > 128) {
        Log("profile invalid; no hooks applied"); return FALSE;
    }
    const BYTE inputExpected[] = {0x81,0xec,0x88,0,0,0};
    const BYTE memoryExpected[] = {0x83,0xec,0x24,0x55,0x56};
    if (memcmp(engineBase + profile->inputRva, inputExpected, sizeof(inputExpected)) ||
        memcmp(engineBase + profile->memoryRva, memoryExpected, sizeof(memoryExpected))) {
        Log("function entry conflict; no hooks applied"); return FALSE;
    }
    for (uint32_t i = 0; i < profile->traceCount; ++i) {
        BYTE* p = engineBase + profile->traces[i].site;
        int32_t delta; memcpy(&delta, p + 1, 4);
        if (*p != 0xe8 || p + 5 + delta != engineBase + profile->traces[i].target) {
            Log("callsite conflict RVA=%08lx; no hooks applied", profile->traces[i].site); return FALSE;
        }
    }
    gameWindow = engineArguments ? reinterpret_cast<HWND>(engineArguments[1]) : nullptr;
    DWORD owner = 0;
    if (!IsWindow(gameWindow) || !GetWindowThreadProcessId(gameWindow, &owner) || owner != GetCurrentProcessId()) gameWindow = nullptr;
    selectedOutput = gameWindow ? MonitorFromWindow(gameWindow, MONITOR_DEFAULTTOPRIMARY) :
        MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    Log("output preference: %s monitor=%p", gameWindow ? "game window display" : "Windows primary display", selectedOutput);
    originalCreate9 = *reinterpret_cast<Create9Function*>(engineBase + profile->d3dCreateRva);
    if (!originalCreate9) { Log("D3D9 import is null; no hooks applied"); return FALSE; }
    BYTE* thunks = static_cast<BYTE*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!thunks) { Log("trace allocation failed error=%lu", GetLastError()); return FALSE; }
    std::vector<Edit> edits;
    if(!PrepareDisplayEdits(edits)||!PrepareTimingEdits(edits)||!PrepareSystemEdits(edits)||!PreparePhysicsEdits(edits)||
       !PrepareVehicleEdits(edits)||!PrepareTendrilClockEdits(edits)||!PrepareTendrilEdits(edits)||
       !PrepareWheelEdits(edits)||!PrepareWoiTraceEdits(edits)||!PrepareAudioQueueEdits(edits)||
       !PrepareOperationTraceEdits(edits)||!PrepareDnsLifetimeEdits(edits)) {VirtualFree(thunks,0,MEM_RELEASE);return FALSE;}
    bool waitDiagnostics=false;
    if(pmdiag::Initialize(selfModule,engineBase))waitDiagnostics=pmwaitdiag::PrepareEdits(edits);
    edits.push_back(Branch(engineBase + profile->inputRva, 0xe9, &IsXInputDevice, sizeof(inputExpected)));
    edits.push_back(Branch(engineBase + profile->memoryRva, 0xe9, &VideoMemoryMiB, sizeof(memoryExpected)));
    Edit importEdit; importEdit.site = engineBase + profile->d3dCreateRva;
    importEdit.before.assign(importEdit.site, importEdit.site + sizeof(void*));
    auto replacementCreate = &CreateForOutput;
    BYTE* pointerBytes = reinterpret_cast<BYTE*>(&replacementCreate);
    importEdit.after.assign(pointerBytes, pointerBytes + sizeof(void*)); edits.push_back(importEdit);
    BYTE* next = thunks;
    for (uint32_t i = 0; i < profile->traceCount; ++i) {
        BYTE* start = next;
        next = TraceThunk(next, profile->traces[i].site, engineBase + profile->traces[i].target);
        edits.push_back(Branch(engineBase + profile->traces[i].site, 0xe8, start, 5));
    }
    DWORD previous;
    if (!VirtualProtect(thunks, 8192, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(thunks, 0, MEM_RELEASE);pmdiag::CloseChannel();Log("trace protection failed");return FALSE;
    }
    FlushInstructionCache(GetCurrentProcess(), thunks, next - thunks);
    for (size_t i = 0; i < edits.size(); ++i) {
        if (!WriteCode(edits[i].site, edits[i].after.data(), edits[i].after.size())) {
            Log("hook write failed index=%u; rolling back", static_cast<unsigned>(i));
            bool restored = true;
            for (size_t j = 0; j <= i; ++j)
                restored = WriteCode(edits[j].site, edits[j].before.data(), edits[j].before.size()) && restored;
            if (restored) {VirtualFree(thunks, 0, MEM_RELEASE);pmdiag::CloseChannel();}
            else Log("hook rollback incomplete; quit and restore before retrying");
            return FALSE;
        }
    }
    DWORD_PTR processMask = 0, systemMask = 0;
    pmdiag::SetWaitHookMask(waitDiagnostics?15:0);
    GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    Log("hooks installed input=%08lx memory=%08lx traces=%lu affinity=%08lx (unchanged)",
        profile->inputRva, profile->memoryRva, profile->traceCount, static_cast<DWORD>(processMask));
    Log("EngineInitialize original body resumes");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        selfModule = instance;
        // No COM, PnP, graphics or hooks under the loader lock.
    }
    return TRUE;
}
