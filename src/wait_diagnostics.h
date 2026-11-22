// Bounded, sampled waits from the original engine's four verified IAT slots.
// Requires engine_telemetry.h, Edit, engineBase and Log from compat.cpp.
// No global system hooks, target suspension, heap allocation or per-wait I/O.
#pragma once
#include <intrin.h>
namespace pmwaitdiag {
enum Kind : uint32_t { Single=1, Multiple=2, Critical=3, Delay=4 };
enum Flags : uint32_t { WaitAll=1, StackBoundReached=2, CallerOnly=4 };
using SingleFunction=DWORD(WINAPI*)(HANDLE,DWORD);
using MultipleFunction=DWORD(WINAPI*)(DWORD,const HANDLE*,BOOL,DWORD);
using CriticalFunction=VOID(WINAPI*)(LPCRITICAL_SECTION);
using SleepFunction=VOID(WINAPI*)(DWORD);
static SingleFunction originalSingle=&::WaitForSingleObject;
static MultipleFunction originalMultiple=&::WaitForMultipleObjects;
static CriticalFunction originalCritical=&::EnterCriticalSection;
static SleepFunction originalSleep=&::Sleep;
static thread_local uint32_t lastGeneration[4]={};
static thread_local bool insideSample=false;

// The patch is built with /O2. Keep EBP in these observer frames so the x86
// runtime unwinder can reach the original engine call site. The original
// engine's optimized frames may still stop a deeper unwind; never guess PCs.
#pragma optimize("y",off)

// First call of each kind per thread/window, not every wait in that window.
// These are samples, never total wait counts or OS ready/wait decomposition.
__declspec(noinline) static bool Begin(Kind kind,DWORD timeout,uintptr_t object,
                                     DWORD count,DWORD flags,void* caller,pmdiag::WaitSample& sample) {
    const uint32_t generation=pmdiag::WaitGeneration();
    if(!generation||insideSample||lastGeneration[kind-1]==generation)return false;
    const DWORD incomingError=GetLastError();
    insideSample=true;lastGeneration[kind-1]=generation;
    sample={};sample.generation=generation;sample.tid=GetCurrentThreadId();
    sample.kind=kind;sample.timeout_ms=timeout;sample.object=static_cast<uint32_t>(object);
    sample.count=count;sample.flags=flags;sample.status=1;
    // The immediate caller is a real return PC even when x86 unwinding stops
    // at an optimized/FPO frame. Remaining PCs are actual runtime unwinds.
    sample.frames[0]=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(caller));
    sample.frame_count=1;
    void* frames[12]={};
    const USHORT captured=CaptureStackBackTrace(2,_countof(frames),frames,nullptr);
    if(!captured)sample.flags|=CallerOnly;
    for(USHORT i=0;i<captured;++i) {
        const auto pc=static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[i]));
        if(pc==sample.frames[sample.frame_count-1])continue;
        if(sample.frame_count==_countof(sample.frames)){sample.flags|=StackBoundReached;break;}
        sample.frames[sample.frame_count++]=pc;
    }
    if(captured==_countof(frames))sample.flags|=StackBoundReached;
    if(sample.frame_count==1)sample.flags|=CallerOnly;
    LARGE_INTEGER now={};QueryPerformanceCounter(&now);sample.qpc_begin=now.QuadPart;
    pmdiag::PublishWait(sample); // Publish in-flight state before a possibly infinite wait.
    // Narrow completed-call timing to after publication. In-flight observations
    // retain the slightly earlier publish time until this call returns.
    QueryPerformanceCounter(&now);sample.qpc_begin=now.QuadPart;
    SetLastError(incomingError);
    return true;
}

__declspec(noinline) static void End(pmdiag::WaitSample& sample,DWORD result,DWORD error) {
    LARGE_INTEGER now={};QueryPerformanceCounter(&now);sample.qpc_end=now.QuadPart;
    sample.result=result;sample.last_error=error;sample.status=2;
    pmdiag::PublishWait(sample);
    insideSample=false;
    SetLastError(error);
}

__declspec(noinline) static DWORD WINAPI SampledSingleWait(HANDLE handle,DWORD timeout) {
    pmdiag::WaitSample sample;
    if(!Begin(Single,timeout,reinterpret_cast<uintptr_t>(handle),1,0,_ReturnAddress(),sample))
        return originalSingle(handle,timeout);
    const DWORD result=originalSingle(handle,timeout),error=GetLastError();
    End(sample,result,error);return result;
}
__declspec(noinline) static DWORD WINAPI SampledMultipleWait(DWORD count,const HANDLE* handles,BOOL all,DWORD timeout) {
    pmdiag::WaitSample sample;
    if(!Begin(Multiple,timeout,0,count,all?WaitAll:0,_ReturnAddress(),sample))
        return originalMultiple(count,handles,all,timeout);
    const DWORD result=originalMultiple(count,handles,all,timeout),error=GetLastError();
    End(sample,result,error);return result;
}
__declspec(noinline) static VOID WINAPI SampledCriticalWait(LPCRITICAL_SECTION section) {
    pmdiag::WaitSample sample;
    if(!Begin(Critical,INFINITE,reinterpret_cast<uintptr_t>(section),1,0,_ReturnAddress(),sample)) {
        originalCritical(section);return;
    }
    originalCritical(section);const DWORD error=GetLastError();
    End(sample,0,error); // EnterCriticalSection has no return value; result=0 is a schema sentinel.
}
__declspec(noinline) static VOID WINAPI SampledSleepWait(DWORD timeout) {
    pmdiag::WaitSample sample;
    if(!Begin(Delay,timeout,0,0,0,_ReturnAddress(),sample)){originalSleep(timeout);return;}
    originalSleep(timeout);const DWORD error=GetLastError();
    End(sample,0,error); // Sleep has no return value; result=0 is a schema sentinel.
}
#pragma optimize("y",on)

// The 98%-of-time dormant gate has no prologue, stack sample, TLS access or
// helper call. Tail jumps preserve the engine return PC and stdcall arguments.
// pmdiag publishes this aligned 32-bit gate only at window boundaries.
using pmdiag::waitGate;
__declspec(naked) static DWORD WINAPI SingleWait(HANDLE,DWORD) {
    __asm {
        cmp waitGate,0
        jne sampled
        jmp dword ptr [originalSingle]
    sampled:
        jmp SampledSingleWait
    }
}
__declspec(naked) static DWORD WINAPI MultipleWait(DWORD,const HANDLE*,BOOL,DWORD) {
    __asm {
        cmp waitGate,0
        jne sampled
        jmp dword ptr [originalMultiple]
    sampled:
        jmp SampledMultipleWait
    }
}
__declspec(naked) static VOID WINAPI CriticalWait(LPCRITICAL_SECTION) {
    __asm {
        cmp waitGate,0
        jne sampled
        jmp dword ptr [originalCritical]
    sampled:
        jmp SampledCriticalWait
    }
}
__declspec(naked) static VOID WINAPI SleepWait(DWORD) {
    __asm {
        cmp waitGate,0
        jne sampled
        jmp dword ptr [originalSleep]
    sampled:
        jmp SampledSleepWait
    }
}

// Original SHA256 98521ac239852fd308446da3eacb6004083e490f3be0c39778ba99abd82701a2.
// All four slots are KERNEL32 imports in that PE. Compare resolved original
// pointers before adding any edit; a conflict disables this optional observer.
// Do not fail the game's existing compatibility initialization on false.
static bool PrepareEdits(std::vector<Edit>& edits) {
    static_assert(sizeof(void*)==4,"Engine observer is for the original x86 executable");
    if(!pmdiag::Enabled())return true;
    if(!engineBase)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(engineBase);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))||dos->e_lfanew>0x1000)return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(engineBase+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_I386||
       nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR32_MAGIC||nt->OptionalHeader.SizeOfImage!=21123072||
       nt->FileHeader.TimeDateStamp!=1755193382)return false;
    HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
    struct Import {uint32_t rva;const char* name;void* replacement;};
    const Import imports[]={
        {0x1331cc0,"WaitForSingleObject",reinterpret_cast<void*>(&SingleWait)},
        {0x1331d50,"WaitForMultipleObjects",reinterpret_cast<void*>(&MultipleWait)},
        {0x1331c9c,"EnterCriticalSection",reinterpret_cast<void*>(&CriticalWait)},
        {0x1331cbc,"Sleep",reinterpret_cast<void*>(&SleepWait)}};
    void* originals[_countof(imports)]={};
    for(unsigned i=0;i<_countof(imports);++i) {
        originals[i]=*reinterpret_cast<void**>(engineBase+imports[i].rva);
        auto expected=reinterpret_cast<void*>(GetProcAddress(kernel,imports[i].name));
        if(!expected||originals[i]!=expected) {
            Log("engine diagnostic wait import conflict name=%s rva=%08lx; wait sampling unavailable",imports[i].name,imports[i].rva);
            return false;
        }
    }
    originalSingle=reinterpret_cast<SingleFunction>(originals[0]);
    originalMultiple=reinterpret_cast<MultipleFunction>(originals[1]);
    originalCritical=reinterpret_cast<CriticalFunction>(originals[2]);
    originalSleep=reinterpret_cast<SleepFunction>(originals[3]);
    for(const auto& item:imports) {
        Edit edit;edit.site=engineBase+item.rva;
        edit.before.assign(edit.site,edit.site+sizeof(void*));
        const BYTE* pointer=reinterpret_cast<const BYTE*>(&item.replacement);
        edit.after.assign(pointer,pointer+sizeof(void*));edits.push_back(edit);
    }
    Log("engine diagnostic waits: sampled first call per kind/thread/window; four original IAT slots; bounded 12-PC stacks; no OS ready-time or driver-internal wait coverage");
    return true;
}
}
