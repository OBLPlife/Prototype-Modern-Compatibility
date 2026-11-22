// WOI diagnosis only: preserve the two original movie requests, the FMV mode
// entry and the original preparation check. No playback, timing or queue edits.
// The loading check has no clock/log/formatting work until it reports completion.
struct WoiSavedRegisters {
    DWORD edi,esi,ebp,esp,ebx,edx,ecx,eax,flags,returnAddress,args[8];
};
static_assert(offsetof(WoiSavedRegisters,args)==40,"x86 pushfd/pushad trace frame");
static void* woiNativeRequest;
static void* woiNativeModeEnter;
static void* woiNativeReady;
static SRWLOCK woiTraceLock=SRWLOCK_INIT;
static volatile LONG woiTraceReadyPending;
struct WoiTraceState {
    unsigned sequence=0,node=0;
    ULONGLONG enterMs=0,requestMs=0;
    bool entered=false,limitReported=false;
};
static WoiTraceState woiTraceState;
static const unsigned woiTraceMovieLimit=32;
static int WoiTraceNode(const char* path) {
    static const char prefix[]="movies/woi/woi_node_";
    if(!path||strncmp(path,prefix,sizeof(prefix)-1))return -1;
    const char* p=path+sizeof(prefix)-1;
    unsigned value=0,digits=0;
    while(*p>='0'&&*p<='9'&&digits<6){value=value*10+(*p++-'0');++digits;}
    return digits&&!strcmp(p,".bik")?static_cast<int>(value):-1;
}
enum WoiTraceEvent {WoiModeEnter=0,WoiMenuRequest=1,WoiModeRequest=2,WoiReady=3};
static void __stdcall ObserveWoiEvent(unsigned event,const WoiSavedRegisters* saved) {
    DWORD error=GetLastError();
    // All pointers below belong to the original, fingerprinted call. The mode
    // path is owned by the live action; the queued request owns its own copy.
    const BYTE* object=nullptr;
    const char* path=nullptr;
    if(event==WoiModeEnter){
        object=reinterpret_cast<const BYTE*>(saved->ecx);
        path=*reinterpret_cast<const char* const*>(object+0x58);
    } else if(event==WoiReady) {
        // Ready stub saves the incoming ECX below its return address before
        // invoking native code. At this observer frame it occupies this slot.
        object=reinterpret_cast<const BYTE*>(saved->returnAddress);
        const BYTE* request=*reinterpret_cast<BYTE**>(engineBase+0x130d3f0);
        if(request)path=*reinterpret_cast<const char* const*>(request+8);
    } else path=reinterpret_cast<const char*>(saved->args[0]);
    int node=WoiTraceNode(path);
    AcquireSRWLockExclusive(&woiTraceLock);
    auto& state=woiTraceState;
    if(node<0||(event==WoiReady&&state.node!=static_cast<unsigned>(node))) {
        state.entered=false;InterlockedExchange(&woiTraceReadyPending,0);
        ReleaseSRWLockExclusive(&woiTraceLock);SetLastError(error);return;
    }
    bool matchingEnter=event==WoiModeRequest&&state.entered&&state.node==static_cast<unsigned>(node);
    bool newMovie=event==WoiModeEnter||((event==WoiMenuRequest||event==WoiModeRequest)&&!matchingEnter);
    if(newMovie&&state.sequence>=woiTraceMovieLimit) {
        if(!state.limitReported){Log("WOI transition trace event limit reached: first %u movies only; playback unchanged",woiTraceMovieLimit);state.limitReported=true;}
        state.entered=false;InterlockedExchange(&woiTraceReadyPending,0);
        ReleaseSRWLockExclusive(&woiTraceLock);SetLastError(error);return;
    }
    if(event==WoiModeEnter) {
        ++state.sequence;state.node=static_cast<unsigned>(node);
        state.enterMs=GetTickCount64();state.requestMs=0;state.entered=true;
        InterlockedExchange(&woiTraceReadyPending,0);
        Log("WOI transition seq=%u node=%u source=consume-fmv phase=mode-enter",state.sequence,state.node);
    } else if(event==WoiMenuRequest||event==WoiModeRequest) {
        ULONGLONG now=GetTickCount64();
        if(!matchingEnter){++state.sequence;state.node=static_cast<unsigned>(node);state.enterMs=0;}
        state.requestMs=now;state.entered=false;
        // These are the exact original 1082f990 admission guards. Report them;
        // never change them or claim that a blocked request was queued.
        bool blocked=*reinterpret_cast<DWORD*>(engineBase+0x130d3cc)!=0||engineBase[0x130d3e0]!=0;
        InterlockedExchange(&woiTraceReadyPending,blocked?0:1);
        Log("WOI transition seq=%u node=%u source=%s phase=request prequeue-ms=%llu mode-entry-seen=%u blocked=%u flags=%08lx,%08lx,%08lx,%08lx arg3=%08lx arg4=%08lx arg7=%08lx",
            state.sequence,state.node,event==WoiMenuRequest?"pause-preview":"consume-fmv",
            matchingEnter?now-state.enterMs:0,matchingEnter,blocked,
            saved->args[1],saved->args[4],saved->args[5],saved->args[7],
            saved->args[2],saved->args[3],saved->args[6]);
    } else if(woiTraceReadyPending&&state.node==static_cast<unsigned>(node)) {
        InterlockedExchange(&woiTraceReadyPending,0);
        unsigned nativeState=*reinterpret_cast<const unsigned*>(object+0xc);
        unsigned frame=0,total=0;
        if(nativeState==2) {
            // Native 1082f300 holds its movie lock around this ready call;
            // 105a0e30 publishes state 2 only after the Bink handle is ready.
            const BYTE* data=*reinterpret_cast<const BYTE* const*>(object+0x10);
            const unsigned* bink=data?*reinterpret_cast<const unsigned* const*>(data+0xc):nullptr;
            if(bink){total=bink[2];frame=bink[3];}
        }
        Log("WOI transition seq=%u node=%u phase=prepare-finished request-ms=%llu native-state=%u frame=%u total=%u; Bink clock unchanged",
            state.sequence,state.node,GetTickCount64()-state.requestMs,nativeState,frame,total);
    }
    ReleaseSRWLockExclusive(&woiTraceLock);SetLastError(error);
}

// These are observation thunks, not C++ forwarding calls. Before a request the
// original stack, GPRs, flags, x87 and SSE state are restored, then we tail-jump
// to the original function. Native return values and caller cleanup are exact.
__declspec(naked) static void WoiMenuRequestTrace() {
    __asm {
        pushfd
        pushad
        mov ebp,esp
        and esp,0fffffff0h
        sub esp,200h
        fxsave [esp]
        push ebp
        push 1
        call ObserveWoiEvent
        fxrstor [esp]
        mov esp,ebp
        popad
        popfd
        jmp dword ptr [woiNativeRequest]
    }
}
__declspec(naked) static void WoiModeRequestTrace() {
    __asm {
        pushfd
        pushad
        mov ebp,esp
        and esp,0fffffff0h
        sub esp,200h
        fxsave [esp]
        push ebp
        push 2
        call ObserveWoiEvent
        fxrstor [esp]
        mov esp,ebp
        popad
        popfd
        jmp dword ptr [woiNativeRequest]
    }
}
__declspec(naked) static void WoiModeEnterTrace() {
    __asm {
        pushfd
        pushad
        mov ebp,esp
        and esp,0fffffff0h
        sub esp,200h
        fxsave [esp]
        push ebp
        push 0
        call ObserveWoiEvent
        fxrstor [esp]
        mov esp,ebp
        popad
        popfd
        jmp dword ptr [woiNativeModeEnter]
    }
}
__declspec(naked) static void WoiReadyTrace() {
    __asm {
        push ecx
        call dword ptr [woiNativeReady]
        pushfd
        cmp dword ptr [woiTraceReadyPending],0
        je done
        test al,al
        jnz done
        pushad
        mov ebp,esp
        and esp,0fffffff0h
        sub esp,200h
        fxsave [esp]
        push ebp
        push 3
        call ObserveWoiEvent
        fxrstor [esp]
        mov esp,ebp
        popad
    done:
        popfd
        lea esp,[esp+4]
        ret
    }
}
static bool WoiTraceCallMatches(DWORD offset,DWORD target,const BYTE (&expected)[5]) {
    BYTE* site=engineBase+offset;int displacement=0;
    memcpy(&displacement,site+1,sizeof(displacement));
    return !memcmp(site,expected,5)&&site+5+displacement==engineBase+target;
}
static bool PrepareWoiTraceEdits(std::vector<Edit>& edits) {
    const BYTE menuCall[]={0xe8,0x6e,0x37,0x2d,0x00};
    const BYTE modeCall[]={0xe8,0x26,0xf2,0xf8,0xff};
    const BYTE readyCall[]={0xe8,0xa8,0x1a,0xd7,0xff};
    const BYTE enterStart[]={0x56,0x8b,0xf1,0xc6,0x46,0x64,0x00,0xe8,0x04,0xc1,0xf8,0xff};
    const BYTE requestStart[]={0x83,0xec,0x34,0xe8,0x28,0xf5,0xff,0xff};
    const BYTE menuCleanup[]={0x83,0xc4,0x60,0xc2,0x04,0x00};
    const BYTE modeCleanup[]={0x83,0xc4,0x20,0x5e,0xc3};
    const BYTE readyResult[]={0x84,0xc0};
    if(!WoiTraceCallMatches(0x55c21d,0x82f990,menuCall)||
       !WoiTraceCallMatches(0x8a0765,0x82f990,modeCall)||
       !WoiTraceCallMatches(0x82f383,0x5a0e30,readyCall)||
       memcmp(engineBase+0x8a06a0,enterStart,sizeof(enterStart))||
       memcmp(engineBase+0x82f990,requestStart,sizeof(requestStart))||
       memcmp(engineBase+0x55c222,menuCleanup,sizeof(menuCleanup))||
       memcmp(engineBase+0x8a076a,modeCleanup,sizeof(modeCleanup))||
       memcmp(engineBase+0x82f388,readyResult,sizeof(readyResult))||
       *reinterpret_cast<BYTE**>(engineBase+0xe787fc)!=engineBase+0x8a06a0) {
        Log("WOI trace instruction/ABI conflict; no WOI hooks prepared");return false;
    }
    woiNativeRequest=engineBase+0x82f990;woiNativeModeEnter=engineBase+0x8a06a0;
    woiNativeReady=engineBase+0x5a0e30;
    std::vector<Edit> prepared;
    prepared.push_back(Branch(engineBase+0x55c21d,0xe8,&WoiMenuRequestTrace,5));
    prepared.push_back(Branch(engineBase+0x8a0765,0xe8,&WoiModeRequestTrace,5));
    prepared.push_back(Branch(engineBase+0x82f383,0xe8,&WoiReadyTrace,5));
    Edit slot;slot.site=engineBase+0xe787fc;slot.before.assign(slot.site,slot.site+4);
    void* replacement=reinterpret_cast<void*>(&WoiModeEnterTrace);
    BYTE* pointer=reinterpret_cast<BYTE*>(&replacement);slot.after.assign(pointer,pointer+4);
    prepared.push_back(slot);edits.insert(edits.end(),prepared.begin(),prepared.end());
    Log("WOI transition diagnosis prepared: mode entry, request origin and preparation completion only; native playback/queue/clock unchanged");
    return true;
}
