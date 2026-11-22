// Three separate native presentation states. Fullscreen stays D3D9 exclusive.
enum class DisplayMode : LONG {Fullscreen=0,Borderless=1,Windowed=2};
static volatile LONG requestedDisplayMode=-1,appliedDisplayMode=-1;
static HWND presentationWindow;
static WNDPROC previousWindowProc;
static LONG_PTR previousBackgroundBrush;
static bool arrangingPresentationWindow;
static RECT desktopOutputRect;
static D3DPRESENT_PARAMETERS successfulPresentation;
static bool haveSuccessfulPresentation;
static volatile LONG focusReturnPending;
static DWORD focusReturnTick;
static volatile LONG avoidedBackgroundErases;
static volatile LONG hiddenInitialWindow,initialUiFrame,initialRevealQueued;
static UINT initialFrameMessage;
static RECT placedPresentationBounds;
static BYTE* activeDisplayRenderer;
static BYTE* successfulDisplayRenderer;
static DWORD successfulAntialiasing;
static IDirect3DDevice9* successfulDevice;
static volatile LONG preventedPlaceholderResizes;
static void RevealInitializedWindow(IDirect3DDevice9*,HWND);
static void (*revealInitializedWindow)(IDirect3DDevice9*,HWND)=RevealInitializedWindow;

// Native ShowCursor(FALSE) used a thread-wide display counter. Keep the
// engine's counter semantics, but hide only when WM_SETCURSOR targets our client.
static int (WINAPI* nativeShowCursor)(BOOL)=::ShowCursor;
static thread_local int logicalCursorCount=INT_MAX;
static volatile LONG clientCursorHidden;
static int WINAPI ClientShowCursor(BOOL show) {
    if(logicalCursorCount==INT_MAX) {
        logicalCursorCount=nativeShowCursor(show);
        nativeShowCursor(!show); // Undo the probe: never leave the OS count hidden.
    } else logicalCursorCount+=show?1:-1;
    InterlockedExchange(&clientCursorHidden,logicalCursorCount<0);
    POINT point={};RECT client={};
    if(gameWindow&&GetForegroundWindow()==gameWindow&&GetCursorPos(&point)&&
       WindowFromPoint(point)==gameWindow&&ScreenToClient(gameWindow,&point)&&
       GetClientRect(gameWindow,&client)&&PtInRect(&client,point))
        SetCursor(logicalCursorCount<0?nullptr:LoadCursorW(nullptr,IDC_ARROW));
    Log("cursor visibility request=%u logical count=%d; OS display counter preserved",show!=FALSE,logicalCursorCount);
    return logicalCursorCount;
}

// ENUM_REGISTRY_SETTINGS is the user's saved Windows display configuration,
// not the transient mode selected by an exclusive D3D swap chain. Query it via
// the display API; no global display setting or registry key is modified.
static bool RefreshSystemDesktop() {
    MONITORINFOEXW monitor={};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(selectedOutput,&monitor))return false;
    DEVMODEW mode={};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(monitor.szDevice,ENUM_REGISTRY_SETTINGS,&mode,0)||
       !mode.dmPelsWidth||!mode.dmPelsHeight||mode.dmDisplayFrequency<=1)return false;
    bool changed=outputDesktopMode.Width!=mode.dmPelsWidth||outputDesktopMode.Height!=mode.dmPelsHeight||
        outputDesktopMode.RefreshRate!=mode.dmDisplayFrequency;
    outputDesktopMode.Width=mode.dmPelsWidth;outputDesktopMode.Height=mode.dmPelsHeight;
    outputDesktopMode.RefreshRate=mode.dmDisplayFrequency;
    desktopOutputRect={monitor.rcMonitor.left,monitor.rcMonitor.top,
        monitor.rcMonitor.left+static_cast<LONG>(mode.dmPelsWidth),
        monitor.rcMonitor.top+static_cast<LONG>(mode.dmPelsHeight)};
    if(changed)Log("system desktop preference refreshed=%ux%u@%u",mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency);
    return true;
}
static LONG_PTR PresentationStyle(LONG_PTR style,LONG mode) {
    return (style&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW|WS_POPUP))|(mode==2?WS_OVERLAPPEDWINDOW:WS_POPUP);
}
static bool KeepPresentationBounds(HWND window,WINDOWPOS* position) {
    LONG mode=InterlockedCompareExchange(&appliedDisplayMode,0,0);
    if(!position||!haveSuccessfulPresentation||arrangingPresentationWindow||mode<0||mode>2||
       (mode==2&&!hiddenInitialWindow)||
       IsIconic(window)||(position->flags&SWP_HIDEWINDOW))return false;
    MONITORINFO monitor={};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(selectedOutput,&monitor))return false;
    LONG x=mode==2?placedPresentationBounds.left:monitor.rcMonitor.left;
    LONG y=mode==2?placedPresentationBounds.top:monitor.rcMonitor.top;
    LONG width=mode==2?placedPresentationBounds.right-x:
        mode==1?desktopOutputRect.right-desktopOutputRect.left:successfulPresentation.BackBufferWidth;
    LONG height=mode==2?placedPresentationBounds.bottom-y:
        mode==1?desktopOutputRect.bottom-desktopOutputRect.top:successfulPresentation.BackBufferHeight;
    bool changed=false;
    if(!(position->flags&SWP_NOMOVE)&&(position->x!=x||position->y!=y)) {
        position->x=x;position->y=y;changed=true;
    }
    if(!(position->flags&SWP_NOSIZE)&&width>0&&height>0&&(position->cx!=width||position->cy!=height)) {
        position->cx=width;position->cy=height;changed=true;
    }
    return changed;
}

static const char* DisplayModeName(LONG mode) {
    return mode==0?"fullscreen":mode==1?"borderless":mode==2?"windowed":"uninitialized";
}
static LRESULT CALLBACK PresentationWindowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    TraceWindowEvent(window,message,w,l);
    if(initialFrameMessage&&message==initialFrameMessage) {
        if(InterlockedCompareExchange(&hiddenInitialWindow,0,0)&&InterlockedCompareExchange(&initialUiFrame,0,0))
            revealInitializedWindow(successfulDevice,window);
        return 0;
    }
    if(message==WM_SETCURSOR&&LOWORD(l)==HTCLIENT&&InterlockedCompareExchange(&clientCursorHidden,0,0)) {
        SetCursor(nullptr);return TRUE;
    }
    // The launcher also calls ShowWindow/changes WS_VISIBLE after CreateDevice.
    // Those early calls must not reveal its empty 800x450 initialization frame.
    // Our first-frame reveal clears hiddenInitialWindow before ShowWindow.
    if(InterlockedCompareExchange(&hiddenInitialWindow,0,0)&&l) {
        if(message==WM_WINDOWPOSCHANGING)
            reinterpret_cast<WINDOWPOS*>(l)->flags&=~SWP_SHOWWINDOW;
        if(message==WM_STYLECHANGING&&w==static_cast<WPARAM>(GWL_STYLE))
            reinterpret_cast<STYLESTRUCT*>(l)->styleNew&=~WS_VISIBLE;
    }
    if(message==WM_DEVICECHANGE)InvalidateInputProducts();
    if(message==WM_WINDOWPOSCHANGING&&KeepPresentationBounds(window,reinterpret_cast<WINDOWPOS*>(l))&&
       InterlockedIncrement(&preventedPlaceholderResizes)<=8)
        Log("prevented launcher placeholder resize after graphics initialization");
    if(message==WM_STYLECHANGING&&w==static_cast<WPARAM>(GWL_STYLE)&&l&&
       haveSuccessfulPresentation&&!arrangingPresentationWindow&&appliedDisplayMode>=0) {
        auto change=reinterpret_cast<STYLESTRUCT*>(l);
        change->styleNew=static_cast<DWORD>(PresentationStyle(change->styleNew,appliedDisplayMode));
    }
    // The original launcher registers DKGRAY_BRUSH (GetStockObject(3)). Once
    // D3D owns the client, let its next Present repaint it instead of GDI gray.
    if(message==WM_ERASEBKGND&&haveSuccessfulPresentation) {
        if(InterlockedIncrement(&avoidedBackgroundErases)<=8)
            Log("suppressed legacy gray background erase mode=%s",DisplayModeName(appliedDisplayMode));
        return 1;
    }
    // Our placement must not feed desktop client size into render resolution.
    if(message==WM_SIZE&&arrangingPresentationWindow)return DefWindowProcA(window,message,w,l);
    if(message==WM_NCDESTROY&&previousBackgroundBrush&&GetClassLongPtrA(window,GCLP_HBRBACKGROUND)==0)
        SetClassLongPtrA(window,GCLP_HBRBACKGROUND,previousBackgroundBrush);
    bool record=message==WM_ACTIVATEAPP||message==WM_ACTIVATE||message==WM_DISPLAYCHANGE||
        (message==WM_SIZE&&w==SIZE_MINIMIZED);
    if(message==WM_ACTIVATEAPP&&w){focusReturnTick=GetTickCount();InterlockedExchange(&focusReturnPending,1);}
    ULONGLONG begin=GetTickCount64();
    LRESULT result=CallWindowProcA(previousWindowProc,window,message,w,l);
    if(record)Log("window event=%04x value=%lu native handler=%llu ms mode=%s minimized=%u",
        message,static_cast<DWORD>(w),GetTickCount64()-begin,DisplayModeName(appliedDisplayMode),IsIconic(window)!=FALSE);
    if(message==WM_NCDESTROY){windowedPresentation.Release();presentationWindow=nullptr;previousWindowProc=nullptr;previousBackgroundBrush=0;}
    return result;
}
static void ObservePresentationWindow(HWND window) {
    if(!window||presentationWindow==window)return;
    SetLastError(0);
    auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(window,GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&PresentationWindowProc)));
    if(!previous){Log("window focus observation unavailable error=%lu",GetLastError());return;}
    previousWindowProc=previous;presentationWindow=window;
    initialFrameMessage=RegisterWindowMessageW(L"PrototypeModern.InitialRenderedFrame.v1");
    previousBackgroundBrush=GetClassLongPtrA(window,GCLP_HBRBACKGROUND);
    if(previousBackgroundBrush) {
        SetClassLongPtrA(window,GCLP_HBRBACKGROUND,0);
        Log("removed native window class background brush; D3D owns client painting");
    }
}
static std::wstring DisplayPreferencePath() {
    wchar_t path[32768];DWORD length=GetModuleFileNameW(selfModule,path,_countof(path));
    wchar_t* slash=length&&length<_countof(path)?wcsrchr(path,L'\\'):nullptr;
    return slash?std::wstring(path,slash-path)+L"\\PrototypeModern.Backup\\display-mode.bin":L"";
}
static LONG ReadDisplayPreference() {
    auto path=DisplayPreferencePath();if(path.empty())return -1;
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return -1;
    DWORD record[2]={},read=0;
    bool ok=GetFileSize(file,nullptr)==sizeof(record)&&ReadFile(file,record,sizeof(record),&read,nullptr)&&read==sizeof(record);
    CloseHandle(file);return ok&&record[0]==0x314d4450&&record[1]<=2?static_cast<LONG>(record[1]):-1;
}
static bool CommitDisplayPreference() {
    LONG mode=InterlockedCompareExchange(&appliedDisplayMode,0,0);
    if(mode<0||mode>2||mode!=requestedDisplayMode)return false;
    auto path=DisplayPreferencePath();if(path.empty())return false;
    auto temporary=path+L".tmp";
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD record[]={0x314d4450,static_cast<DWORD>(mode)},written=0;
    bool ok=WriteFile(file,record,sizeof(record),&written,nullptr)&&written==sizeof(record)&&FlushFileBuffers(file);
    CloseHandle(file);
    ok=ok&&MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    Log("display mode confirmation saved=%u mode=%s",ok,DisplayModeName(mode));return ok;
}
static void SetPresentationMode(D3DPRESENT_PARAMETERS& p,LONG mode,UINT refresh) {
    p.Windowed=mode!=static_cast<LONG>(DisplayMode::Fullscreen);
    p.FullScreen_RefreshRateInHz=p.Windowed?0:refresh;
}
static bool PlacePresentationWindow(HWND window,const D3DPRESENT_PARAMETERS& p,LONG mode) {
    if(!window||!p.BackBufferWidth||!p.BackBufferHeight)return false;
    MONITORINFO monitor={};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(selectedOutput,&monitor))return false;
    LONG_PTR oldStyle=GetWindowLongPtrW(window,GWL_STYLE),oldExStyle=GetWindowLongPtrW(window,GWL_EXSTYLE);
    LONG_PTR style=PresentationStyle(oldStyle,mode);
    LONG_PTR exStyle=oldExStyle&~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE|WS_EX_CLIENTEDGE);
    if(mode==2)exStyle|=WS_EX_WINDOWEDGE;
    RECT bounds={monitor.rcMonitor.left,monitor.rcMonitor.top,
        monitor.rcMonitor.left+static_cast<LONG>(p.BackBufferWidth),monitor.rcMonitor.top+static_cast<LONG>(p.BackBufferHeight)};
    if(mode==1) {
        bounds.right=bounds.left+(desktopOutputRect.right-desktopOutputRect.left);
        bounds.bottom=bounds.top+(desktopOutputRect.bottom-desktopOutputRect.top);
    } else if(mode==2) {
        RECT frame={};AdjustWindowRectEx(&frame,static_cast<DWORD>(style),FALSE,static_cast<DWORD>(exStyle));
        LONG availableWidth=monitor.rcWork.right-monitor.rcWork.left-(frame.right-frame.left);
        LONG availableHeight=monitor.rcWork.bottom-monitor.rcWork.top-(frame.bottom-frame.top);
        double scale=std::min(1.0,std::min(double(availableWidth)/p.BackBufferWidth,double(availableHeight)/p.BackBufferHeight));
        LONG width=static_cast<LONG>(p.BackBufferWidth*scale)+frame.right-frame.left;
        LONG height=static_cast<LONG>(p.BackBufferHeight*scale)+frame.bottom-frame.top;
        bounds.left=monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2;
        bounds.top=monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2;
        bounds.right=bounds.left+width;bounds.bottom=bounds.top+height;
    }
    placedPresentationBounds=bounds;
    RECT current={};GetWindowRect(window,&current);
    if(oldStyle==style&&oldExStyle==exStyle&&!(oldExStyle&WS_EX_TOPMOST)&&EqualRect(&current,&bounds))return true;
    bool previous=arrangingPresentationWindow;arrangingPresentationWindow=true;
    if(oldStyle!=style)SetWindowLongPtrW(window,GWL_STYLE,style);
    if(oldExStyle!=exStyle)SetWindowLongPtrW(window,GWL_EXSTYLE,exStyle);
    bool ok=SetWindowPos(window,HWND_NOTOPMOST,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,
        SWP_NOACTIVATE|SWP_FRAMECHANGED)!=FALSE;
    arrangingPresentationWindow=previous;
    if(!ok)Log("display window placement failed mode=%s error=%lu",DisplayModeName(mode),GetLastError());
    return ok;
}
static void PreparePresentation(D3DPRESENT_PARAMETERS* parameters,HWND focus,bool creating) {
    if(!parameters)return;
    HWND window=parameters->hDeviceWindow?parameters->hDeviceWindow:focus;
    if(creating) {
        hiddenInitialWindow=window&&!IsWindowVisible(window);
        initialUiFrame=false;
        initialRevealQueued=0;
        ObservePresentationWindow(window);
        MONITORINFO monitor={};monitor.cbSize=sizeof(monitor);
        if(GetMonitorInfoW(selectedOutput,&monitor))desktopOutputRect=monitor.rcMonitor;
        LONG saved=ReadDisplayPreference();
        LONG native=parameters->Windowed?((GetWindowLongPtrW(window,GWL_STYLE)&WS_OVERLAPPEDWINDOW)?2:1):0;
        InterlockedExchange(&requestedDisplayMode,saved>=0?saved:native);
    }
    LONG mode=InterlockedCompareExchange(&requestedDisplayMode,0,0);
    if(mode<0||mode>2)return;
    RefreshSystemDesktop();
    // SetWindowPos synchronously dispatches WM_SIZE through the outer native
    // GFx procedure. Never let a reentrant message mutate this selected mode.
    D3DPRESENT_PARAMETERS effective=*parameters;
    SetPresentationMode(effective,mode,effective.FullScreen_RefreshRateInHz);
    PlacePresentationWindow(window,effective,mode);
    *parameters=effective;
}
static void RevealInitializedWindow(IDirect3DDevice9* device,HWND window) {
    if(!hiddenInitialWindow||!device||!window)return;
    InterlockedExchange(&hiddenInitialWindow,0);
    // Reveal immediately before presenting the first actual UI frame. Creating
    // a device is not evidence that the engine has finished drawing a frame.
    bool previous=arrangingPresentationWindow;arrangingPresentationWindow=true;
    ShowWindow(window,SW_SHOW);
    // STARTF_USESHOWWINDOW/SW_HIDE can override the first ShowWindow on an
    // overlapped window. That call consumes the startup hint; a second explicit
    // show is needed only when the real window is still hidden.
    if(!IsWindowVisible(window))ShowWindow(window,SW_SHOW);
    bool visible=IsWindowVisible(window)!=FALSE;
    arrangingPresentationWindow=previous;
    if(!visible) {
        InterlockedExchange(&hiddenInitialWindow,1);
        InterlockedExchange(&initialRevealQueued,0);
    }
    Log("initial window reveal with first UI frame at final geometry; visible=%u",visible);
}
static void RequestInitialReveal(IDirect3DDevice9* device,HWND window) {
    // Present can run on the engine's render thread. Do not synchronously send
    // ShowWindow to the window thread while that thread waits for rendering.
    if(GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId())revealInitializedWindow(device,window);
    else if(initialFrameMessage&&!InterlockedExchange(&initialRevealQueued,1)) {
        if(!PostMessageW(window,initialFrameMessage,0,0))InterlockedExchange(&initialRevealQueued,0);
    }
}
static void PresentationSucceeded(const D3DPRESENT_PARAMETERS* parameters,IDirect3DDevice9* device=nullptr) {
    if(!parameters)return;
    successfulPresentation=*parameters;haveSuccessfulPresentation=true;
    successfulDevice=device;successfulDisplayRenderer=activeDisplayRenderer;
    if(activeDisplayRenderer)successfulAntialiasing=*reinterpret_cast<DWORD*>(activeDisplayRenderer+0x28);
    LONG mode=InterlockedCompareExchange(&requestedDisplayMode,0,0);
    InterlockedExchange(&appliedDisplayMode,mode);
    Log("display mode applied=%s windowed=%u render=%ux%u refresh=%u",DisplayModeName(mode),
        parameters->Windowed,parameters->BackBufferWidth,parameters->BackBufferHeight,parameters->FullScreen_RefreshRateInHz);
}
