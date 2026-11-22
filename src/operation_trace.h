// Observes the original engine's API results. No global hooks, polling thread,
// synthetic input, key suppression, save contents or changed return values.
static SRWLOCK operationLock=SRWLOCK_INIT;
struct TraceObject {DWORD type=0,last=0;bool known=false;};
struct TraceInputDevice {
    IDirectInputDevice8A* device=nullptr;DWORD bytes=0,kind=0,id=0;LONG epoch=0;
    bool sampled=false;HRESULT status=E_UNEXPECTED;
    std::vector<TraceObject> objects;
    ULONGLONG motionFirst=0,motionLast=0,motionSamples=0;
    LONGLONG motionDelta[3]={};
};
static std::vector<TraceInputDevice> traceInputDevices;
static DWORD nextTraceDevice;
static void FlushTraceMotion(TraceInputDevice& state,ULONGLONG now,const char* reason) {
    if(!state.motionSamples)return;
    Log("input motion device=%lu samples=%llu first-ms=%llu last-ms=%llu span-ms=%llu dx=%lld dy=%lld wheel=%lld reason=%s",
        state.id,state.motionSamples,state.motionFirst-started,state.motionLast-started,now-state.motionFirst,
        state.motionDelta[0],state.motionDelta[1],state.motionDelta[2],reason);
    state.motionSamples=0;state.motionFirst=state.motionLast=0;
    state.motionDelta[0]=state.motionDelta[1]=state.motionDelta[2]=0;
}
static bool TraceForeground(){return gameWindow&&GetForegroundWindow()==gameWindow;}
static TraceInputDevice& TraceDevice(IDirectInputDevice8A* device,DWORD bytes) {
    LONG epoch=InterlockedCompareExchange(&traceDeviceEpoch,0,0);
    for(auto& item:traceInputDevices)if(item.device==device&&item.bytes==bytes&&item.epoch==epoch)return item;
    for(auto& item:traceInputDevices)if(item.device==device&&item.motionSamples)
        FlushTraceMotion(item,GetTickCount64(),"device-format-or-epoch");
    if(traceInputDevices.size()>=32){FlushTraceMotion(traceInputDevices.front(),GetTickCount64(),"device-evicted");traceInputDevices.erase(traceInputDevices.begin());}
    TraceInputDevice state;state.device=device;state.bytes=bytes;state.epoch=epoch;state.id=++nextTraceDevice;
    DIDEVICEINSTANCEA info={};info.dwSize=sizeof(info);
    HRESULT identity=device->GetDeviceInfo(&info);state.kind=GET_DIDEVICE_TYPE(info.dwDevType);
    Log("input device=%lu api=DirectInput kind=%lu info=%08lx name=%.*s bytes=%lu epoch=%ld",
        state.id,state.kind,identity,static_cast<int>(sizeof(info.tszInstanceName)),info.tszInstanceName,bytes,epoch);
    // Original constructor's format places one object every four bytes.
    if(bytes%4==0&&bytes<=4096) {
        state.objects.resize(bytes/4);
        for(DWORD i=0;i<bytes/4;++i) {
            DIDEVICEOBJECTINSTANCEA object={};object.dwSize=sizeof(object);
            if(SUCCEEDED(device->GetObjectInfo(&object,i*4,DIPH_BYOFFSET))) {
                state.objects[i].known=true;state.objects[i].type=object.dwType;
                Log("input map device=%lu offset=%lu type=%08lx instance=%lu name=%.*s",state.id,i*4,
                    object.dwType,DIDFT_GETINSTANCE(object.dwType),static_cast<int>(sizeof(object.tszName)),object.tszName);
            }
        }
    } else Log("input trace unsupported custom format bytes=%lu; native input unchanged",bytes);
    traceInputDevices.push_back(std::move(state));return traceInputDevices.back();
}
static void ObserveInputStateAt(TraceInputDevice& state,const void* data,bool foreground,ULONGLONG now) {
    if(!foreground){FlushTraceMotion(state,now,"focus-lost");state.sampled=false;return;}
    const auto* values=static_cast<const BYTE*>(data);
    bool shift=false,next=false,previous=false,issue=false,mouseButton=false;
    bool moved=false;
    if(state.kind==DI8DEVTYPE_KEYBOARD)for(unsigned i=0;i<state.objects.size();++i) {
        const auto& o=state.objects[i];DWORD key=DIDFT_GETINSTANCE(o.type);
        bool down=(values[i*4]&0x80)!=0;
        if(o.known&&(key==DIK_LSHIFT||key==DIK_RSHIFT)&&down)shift=true;
    }
    char changes[620]={};size_t used=0;
    for(unsigned i=0;i<state.objects.size();++i) {
        auto& o=state.objects[i];DWORD value=0;memcpy(&value,values+i*4,4);
        bool button=o.known&&(DIDFT_GETTYPE(o.type)&DIDFT_BUTTON)!=0;
        bool relative=o.known&&(DIDFT_GETTYPE(o.type)&DIDFT_RELAXIS)!=0&&state.kind==DI8DEVTYPE_MOUSE;
        if(button)value&=0x80;
        // The native custom mouse map identifies X/Y/wheel as relative
        // instances 0/1/2. Other axes, including absolute axes, stay immediate.
        DWORD axis=DIDFT_GETINSTANCE(o.type);
        if(relative&&axis<3){
            LONG delta=static_cast<LONG>(value);
            if(delta){state.motionDelta[axis]+=delta;moved=true;}
            o.last=value;continue;
        }
        bool changed=relative?static_cast<LONG>(value)!=0:(!state.sampled||value!=o.last);
        // On focus return record held states, without inventing hotkey presses.
        if(button&&state.sampled&&value&&!o.last&&state.kind==DI8DEVTYPE_KEYBOARD) {
            DWORD key=DIDFT_GETINSTANCE(o.type);
            if(key==DIK_F8){next=!shift;previous=shift;}
            if(key==DIK_F7)issue=true;
        }
        if(changed&&(!button||state.sampled||value)) {
            if(used>sizeof(changes)-80){Log("input state device=%lu%s %s",state.id,state.sampled?"":" baseline",changes);used=0;changes[0]=0;}
            int n=_snprintf_s(changes+used,sizeof(changes)-used,_TRUNCATE,"%s%u:%s%ld",used?" ":"",i*4,
                button?(value?"down=":"up="):relative?"delta=":"value=",static_cast<LONG>(value));
            if(n>0)used+=n;
            if(button&&state.kind==DI8DEVTYPE_MOUSE)mouseButton=true;
        }
        o.last=value;
    }
    if(used)Log("input state device=%lu%s %s",state.id,state.sampled?"":" baseline",changes);
    if(mouseButton){POINT p={};if(GetCursorPos(&p)&&ScreenToClient(gameWindow,&p))Log("input mouse-button client=%ld,%ld",p.x,p.y);}
    if(moved){if(!state.motionSamples)state.motionFirst=now;++state.motionSamples;state.motionLast=now;}
    if(state.motionSamples&&now-state.motionFirst>=1000)FlushTraceMotion(state,now,"interval");
    state.sampled=true;
    if(next||previous)TestMarker(previous,false);
    if(issue)TestMarker(false,true);
}
static void ObserveInputState(TraceInputDevice& state,const void* data,bool foreground) {
    // One inexpensive clock read for mouse polling; no extra device queries.
    ObserveInputStateAt(state,data,foreground,state.kind==DI8DEVTYPE_MOUSE?GetTickCount64():0);
}
static HRESULT WINAPI TraceGetDeviceState(IDirectInputDevice8A* device,DWORD bytes,void* data) {
    HRESULT result=device->GetDeviceState(bytes,data);DWORD error=GetLastError();
    AcquireSRWLockExclusive(&operationLock);
    if(TraceForeground()) {
        auto& state=TraceDevice(device,bytes);
        if(state.status!=result){Log("input status device=%lu result=%08lx",state.id,result);state.status=result;}
        if(result==DI_OK&&data)ObserveInputState(state,data,true);
        else {if(state.motionSamples)FlushTraceMotion(state,GetTickCount64(),"input-unavailable");state.sampled=false;}
    } else for(auto& state:traceInputDevices)if(state.device==device){
        if(state.motionSamples)FlushTraceMotion(state,GetTickCount64(),"focus-lost");state.sampled=false;
    }
    ReleaseSRWLockExclusive(&operationLock);SetLastError(error);return result;
}
typedef DWORD (WINAPI* NativeXInputState)(DWORD,XINPUT_STATE*);
static NativeXInputState nativeXInputState;
struct TracePad {XINPUT_GAMEPAD pad={};DWORD status=~0u;bool sampled=false;};
static TracePad tracePads[XUSER_MAX_COUNT];
static void TraceWindowEvent(HWND window,UINT message,WPARAM w,LPARAM l) {
    if((message==WM_ACTIVATEAPP&&!w)||message==WM_DEVICECHANGE||message==WM_CLOSE||message==WM_NCDESTROY){
        AcquireSRWLockExclusive(&operationLock);
        ULONGLONG now=GetTickCount64();
        const char* reason=message==WM_ACTIVATEAPP?"focus-lost":message==WM_DEVICECHANGE?"device-change":"window-close";
        for(auto& state:traceInputDevices)FlushTraceMotion(state,now,reason);
        ReleaseSRWLockExclusive(&operationLock);
    }
    if(message==WM_ACTIVATEAPP&&!w) {
        AcquireSRWLockExclusive(&operationLock);
        for(auto& state:traceInputDevices)state.sampled=false;
        for(auto& state:tracePads)state.sampled=false;
        ReleaseSRWLockExclusive(&operationLock);
        Log("input focus lost; held-state baseline invalidated, no fabricated releases");
    }
    if(message==WM_DEVICECHANGE||message==WM_DISPLAYCHANGE||message==WM_DPICHANGED||message==WM_CLOSE||message==WM_NCDESTROY)
        Log("operation window=%p message=%04x wparam=%08lx lparam=%08lx",window,message,static_cast<DWORD>(w),static_cast<DWORD>(l));
}
static DWORD WINAPI TraceXInputState(DWORD index,XINPUT_STATE* output) {
    DWORD result=nativeXInputState(index,output),error=GetLastError();
    if(index<XUSER_MAX_COUNT) {
        AcquireSRWLockExclusive(&operationLock);auto& state=tracePads[index];
        if(state.status!=result){Log("input XInput controller=%lu status=%lu",index,result);state.status=result;}
        if(result==ERROR_SUCCESS&&output&&TraceForeground()) {
            const auto& p=output->Gamepad;
            if(!state.sampled||memcmp(&state.pad,&p,sizeof(p)))
                Log("input XInput controller=%lu packet=%lu buttons=%04x LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d%s",
                    index,output->dwPacketNumber,p.wButtons,p.bLeftTrigger,p.bRightTrigger,p.sThumbLX,p.sThumbLY,p.sThumbRX,p.sThumbRY,state.sampled?"":" baseline");
            state.pad=p;state.sampled=true;
        } else state.sampled=false;
        ReleaseSRWLockExclusive(&operationLock);
    }
    SetLastError(error);return result;
}

// I/O records only for the original game's profile.bin and slot-*.bin names.
// Pending overlapped I/O is a submission, never reported as completed success.
struct TraceSaveHandle {HANDLE handle;std::string name;};
static std::vector<TraceSaveHandle> traceSaveHandles;
static decltype(&CreateFileA) nativeTraceCreateFile;
static decltype(&ReadFile) nativeTraceReadFile;
static decltype(&WriteFile) nativeTraceWriteFile;
static decltype(&CloseHandle) nativeTraceCloseHandle;
static decltype(&MoveFileA) nativeTraceMoveFile;
static decltype(&DeleteFileA) nativeTraceDeleteFile;
static const char* SaveTraceName(const char* path) {
    if(!path)return nullptr;const char* name=path;
    for(const char* p=path;*p;++p)if(*p=='\\'||*p=='/')name=p+1;
    size_t n=strlen(name);
    return !_stricmp(name,"profile.bin")||(n>9&&!_strnicmp(name,"slot-",5)&&!_stricmp(name+n-4,".bin"))?name:nullptr;
}
static std::string TracedSaveName(HANDLE handle) {
    std::string name;AcquireSRWLockShared(&operationLock);
    for(const auto& entry:traceSaveHandles)if(entry.handle==handle){name=entry.name;break;}
    ReleaseSRWLockShared(&operationLock);return name;
}
static HANDLE WINAPI TraceCreateFile(LPCSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE templ) {
    HANDLE result=nativeTraceCreateFile(path,access,share,security,creation,flags,templ);DWORD error=GetLastError();
    if(const char* name=SaveTraceName(path)) {
        if(result!=INVALID_HANDLE_VALUE) {
            AcquireSRWLockExclusive(&operationLock);
            traceSaveHandles.push_back({result,name});ReleaseSRWLockExclusive(&operationLock);
        }
        Log("save open name=%s handle=%p access=%08lx disposition=%lu flags=%08lx ok=%u last-error=%lu",name,result,access,creation,flags,result!=INVALID_HANDLE_VALUE,error);
    }
    SetLastError(error);return result;
}
static BOOL WINAPI TraceReadFile(HANDLE file,LPVOID buffer,DWORD size,LPDWORD done,LPOVERLAPPED overlapped) {
    DWORD incomingError=GetLastError();std::string name=TracedSaveName(file);SetLastError(incomingError);
    BOOL result=nativeTraceReadFile(file,buffer,size,done,overlapped);DWORD error=GetLastError();
    if(!name.empty())Log("save read name=%s handle=%p requested=%lu transferred=%lu ok=%u overlapped=%u error=%lu",
        name.c_str(),file,size,result&&done?*done:0,result,overlapped!=nullptr,result?0:error);
    SetLastError(error);return result;
}
static BOOL WINAPI TraceWriteFile(HANDLE file,LPCVOID buffer,DWORD size,LPDWORD done,LPOVERLAPPED overlapped) {
    DWORD incomingError=GetLastError();std::string name=TracedSaveName(file);SetLastError(incomingError);
    BOOL result=nativeTraceWriteFile(file,buffer,size,done,overlapped);DWORD error=GetLastError();
    if(!name.empty())Log("save write name=%s handle=%p requested=%lu transferred=%lu ok=%u overlapped=%u error=%lu",
        name.c_str(),file,size,result&&done?*done:0,result,overlapped!=nullptr,result?0:error);
    SetLastError(error);return result;
}
static BOOL WINAPI TraceCloseHandle(HANDLE file) {
    DWORD incomingError=GetLastError();std::string name=TracedSaveName(file);SetLastError(incomingError);
    BOOL result=nativeTraceCloseHandle(file);DWORD error=GetLastError();
    if(!name.empty()) {
        if(result){AcquireSRWLockExclusive(&operationLock);
            traceSaveHandles.erase(std::remove_if(traceSaveHandles.begin(),traceSaveHandles.end(),[&](const TraceSaveHandle& h){return h.handle==file;}),traceSaveHandles.end());
            ReleaseSRWLockExclusive(&operationLock);}
        Log("save close name=%s handle=%p ok=%u error=%lu",name.c_str(),file,result,result?0:error);
    }
    SetLastError(error);return result;
}
static BOOL WINAPI TraceMoveFile(LPCSTR source,LPCSTR dest) {
    BOOL result=nativeTraceMoveFile(source,dest);DWORD error=GetLastError();
    const char* a=SaveTraceName(source);const char* b=SaveTraceName(dest);
    if(a||b)Log("save move from=%s to=%s ok=%u error=%lu",a?a:"other",b?b:"other",result,result?0:error);
    SetLastError(error);return result;
}
static BOOL WINAPI TraceDeleteFile(LPCSTR path) {
    BOOL result=nativeTraceDeleteFile(path);DWORD error=GetLastError();
    if(const char* name=SaveTraceName(path))Log("save delete name=%s ok=%u error=%lu",name,result,result?0:error);
    SetLastError(error);return result;
}
template<class T> static bool ObserveImport(std::vector<Edit>& edits,const char* dll,const char* name,WORD ordinal,T replacement,T& original) {
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(engineBase+reinterpret_cast<IMAGE_DOS_HEADER*>(engineBase)->e_lfanew);
    auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!directory.VirtualAddress)return false;
    auto* descriptor=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(engineBase+directory.VirtualAddress);
    for(;descriptor->Name;++descriptor)if(!_stricmp(reinterpret_cast<const char*>(engineBase+descriptor->Name),dll)) {
        if(!descriptor->OriginalFirstThunk)return false;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA*>(engineBase+descriptor->OriginalFirstThunk);
        auto* slots=reinterpret_cast<IMAGE_THUNK_DATA*>(engineBase+descriptor->FirstThunk);
        for(unsigned i=0;names[i].u1.AddressOfData;++i) {
            bool byOrdinal=IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)!=FALSE;
            bool match=byOrdinal?(!name&&IMAGE_ORDINAL(names[i].u1.Ordinal)==ordinal):
                (name&&!strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(engineBase+names[i].u1.AddressOfData)->Name,name));
            if(match){original=reinterpret_cast<T>(slots[i].u1.Function);if(!original)return false;
                Edit edit;edit.site=reinterpret_cast<BYTE*>(&slots[i].u1.Function);
                edit.before.assign(edit.site,edit.site+sizeof(T));auto* bytes=reinterpret_cast<BYTE*>(&replacement);
                edit.after.assign(bytes,bytes+sizeof(T));edits.push_back(edit);return true;}
        }
    }
    Log("operation trace import missing dll=%s name=%s ordinal=%u",dll,name?name:"ordinal",ordinal);return false;
}
static bool PrepareOperationTraceEdits(std::vector<Edit>& edits) {
    // Guarded native mov eax,[ecx+24h]; call eax. Stack already contains
    // (COM this, custom byte count, buffer), exactly our stdcall signature.
    edits.push_back(Branch(engineBase+0x642e2b,0xe8,&TraceGetDeviceState,5));
    bool ok=ObserveImport(edits,"XINPUT1_3.dll",nullptr,2,&TraceXInputState,nativeXInputState)&&
        ObserveImport(edits,"KERNEL32.dll","CreateFileA",0,&TraceCreateFile,nativeTraceCreateFile)&&
        ObserveImport(edits,"KERNEL32.dll","ReadFile",0,&TraceReadFile,nativeTraceReadFile)&&
        ObserveImport(edits,"KERNEL32.dll","WriteFile",0,&TraceWriteFile,nativeTraceWriteFile)&&
        ObserveImport(edits,"KERNEL32.dll","CloseHandle",0,&TraceCloseHandle,nativeTraceCloseHandle)&&
        ObserveImport(edits,"KERNEL32.dll","MoveFileA",0,&TraceMoveFile,nativeTraceMoveFile)&&
        ObserveImport(edits,"KERNEL32.dll","DeleteFileA",0,&TraceDeleteFile,nativeTraceDeleteFile);
    if(ok)Log("operation trace prepared: native custom DirectInput, XInput state, save I/O, UI; input values and API results preserved");
    return ok;
}
