#pragma once
// Bounded, memory-only handoff to the existing external text collector.
// Schema is shared with tools/capture_engine.py; no game-thread file writes.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

namespace pmdiag {
static const uint32_t Magic=0x31444745,Version=1,WaitSlots=128;
#pragma pack(push,8)
struct GpuSample {
    uint32_t status,flags;
    uint64_t frame_id,qpc_begin,qpc_end,gpu_frequency,gpu_start,gpu_end;
    double command_span_ms,cpu_submit_span_ms,present_wall_ms,upload_wall_ms;
    uint32_t draw_calls,upload_calls,query_polls;
    int32_t error;
    double upload_gpu_ms;
    uint32_t upload_gpu_samples,upload_gpu_omitted;
    uint64_t query_ready_qpc;
};
struct WaitSample {
    uint32_t generation,tid,kind,timeout_ms,result,last_error,frame_count,status;
    uint64_t qpc_begin,qpc_end;
    uint32_t frames[12];
    uint32_t object,count,flags;
};
struct GpuSlot {volatile LONG sequence;uint32_t reserved;GpuSample sample;};
struct WaitSlot {
    volatile LONG owner_tid;
    uint32_t kind;
    volatile LONG sequence;
    uint32_t reserved;
    uint64_t thread_creation;
    WaitSample sample;
};
struct SharedBlock {
    volatile LONG magic;
    uint32_t version,bytes,pid;
    uint64_t creation,qpc_frequency;
    uint32_t engine_base,patch_base,wait_hook_mask,wait_slots;
    volatile LONG wait_drops,generation,wait_generation;
    uint32_t reserved;
    GpuSlot gpu;
    WaitSlot waits[WaitSlots];
};
#pragma pack(pop)
static_assert(sizeof(GpuSample)==128,"GPU telemetry ABI");
static_assert(sizeof(WaitSample)==112,"Wait telemetry ABI");
static_assert(sizeof(WaitSlot)==136,"Wait slot ABI");
static_assert(sizeof(SharedBlock)==17608,"Shared telemetry ABI");
static SharedBlock* channel;
static HANDLE mapping;
static LONGLONG nextWindow,windowEnd;
static volatile LONG gpuClaimedGeneration;
static volatile LONG waitGate,channelEpoch;
static bool Enabled(){return channel!=nullptr;}
static uint32_t WaitGeneration(){return static_cast<uint32_t>(waitGate);}
static bool GpuRequestPending(){return channel&&channel->generation!=gpuClaimedGeneration;}
static bool GpuRequested(){
    if(!channel)return false;
    LONG generation=channel->generation;
    return generation && InterlockedExchange(&gpuClaimedGeneration,generation)!=generation;
}
static void SetWaitHookMask(uint32_t value){if(channel)channel->wait_hook_mask=value;}
static void Tick(LONGLONG now){
    if(!channel)return;
    if(now>=nextWindow){
        LONG generation=InterlockedIncrement(&channel->generation);
        if(generation<=0){InterlockedExchange(&channel->generation,1);generation=1;}
        nextWindow=now+static_cast<LONGLONG>(channel->qpc_frequency)*5;
        windowEnd=now+static_cast<LONGLONG>(channel->qpc_frequency)/10;
        InterlockedExchange(&channel->wait_generation,generation);
        InterlockedExchange(&waitGate,generation);
    }else if(now>=windowEnd && channel->wait_generation){
        InterlockedExchange(&channel->wait_generation,0);
        InterlockedExchange(&waitGate,0);
    }
}
static bool CreateChannel(uint32_t engine,uint32_t patch){
    if(channel)return true;
    FILETIME born={},end={},kernel={},user={};LARGE_INTEGER frequency={},now={};
    if(!GetProcessTimes(GetCurrentProcess(),&born,&end,&kernel,&user)||
       !QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0||!QueryPerformanceCounter(&now))return false;
    ULARGE_INTEGER creation;creation.LowPart=born.dwLowDateTime;creation.HighPart=born.dwHighDateTime;
    wchar_t name[160]={};swprintf_s(name,L"Local\\PrototypeModern.EngineTelemetry.%lu.%llu",GetCurrentProcessId(),creation.QuadPart);
    HANDLE created=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(SharedBlock),name);
    if(!created)return false;
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(created);return false;}
    auto* view=static_cast<SharedBlock*>(MapViewOfFile(created,FILE_MAP_WRITE,0,0,sizeof(SharedBlock)));
    if(!view){CloseHandle(created);return false;}
    memset(view,0,sizeof(*view));
    view->version=Version;view->bytes=sizeof(*view);view->pid=GetCurrentProcessId();
    view->creation=creation.QuadPart;view->qpc_frequency=frequency.QuadPart;
    view->engine_base=engine;view->patch_base=patch;view->wait_slots=WaitSlots;
    view->generation=view->wait_generation=1;
    nextWindow=now.QuadPart+frequency.QuadPart*5;windowEnd=now.QuadPart+frequency.QuadPart/10;
    mapping=created;channel=view;InterlockedIncrement(&channelEpoch);InterlockedExchange(&gpuClaimedGeneration,0);
    MemoryBarrier();InterlockedExchange(&view->magic,Magic);InterlockedExchange(&waitGate,1);
    return true;
}
static bool Initialize(HMODULE patch,BYTE* engine){
    wchar_t path[32768]={};DWORD length=GetModuleFileNameW(patch,path,_countof(path));
    if(!length||length>=_countof(path))return false;
    wchar_t* last=wcsrchr(path,L'\\');if(!last)return false;
    const wchar_t suffix[]=L"PrototypeModern.Performance.ini";
    if(static_cast<size_t>(last-path)+1+_countof(suffix)>_countof(path))return false;
    wcscpy_s(last+1,_countof(path)-static_cast<size_t>(last-path)-1,suffix);
    if(GetPrivateProfileIntW(L"Capture",L"Enabled",0,path)!=1)return false;
    return CreateChannel(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(engine)),
                         static_cast<uint32_t>(reinterpret_cast<uintptr_t>(patch)));
}
static void PublishGpu(const GpuSample& sample){
    if(!channel)return;
    auto& slot=channel->gpu;
    InterlockedIncrement(&slot.sequence);MemoryBarrier();
    slot.sample=sample;MemoryBarrier();InterlockedIncrement(&slot.sequence);
}
static void PublishWait(const WaitSample& sample){
    if(!channel||sample.kind<1||sample.kind>4)return;
    // Four small TLS caches, no allocation/lock on either wait path. Slots never
    // move, and birth timestamps distinguish a recycled Windows TID in the file.
    static thread_local WaitSlot* cached[4]={};
    static thread_local LONG cachedEpoch=0;
    static thread_local uint64_t birth=0;
    if(cachedEpoch!=channelEpoch){memset(cached,0,sizeof(cached));cachedEpoch=channelEpoch;}
    if(!birth){
        FILETIME b={},e={},k={},u={};
        if(GetThreadTimes(GetCurrentThread(),&b,&e,&k,&u))birth=(uint64_t(b.dwHighDateTime)<<32)|b.dwLowDateTime;
    }
    WaitSlot*& slot=cached[sample.kind-1];
    if(!slot){
        for(unsigned i=0;i<WaitSlots;++i){
            auto& candidate=channel->waits[i];
            if(candidate.owner_tid==static_cast<LONG>(sample.tid)&&candidate.kind==sample.kind){slot=&candidate;break;}
        }
        if(!slot)for(unsigned i=0;i<WaitSlots;++i){
            auto& candidate=channel->waits[i];
            if(InterlockedCompareExchange(&candidate.owner_tid,sample.tid,0)==0){
                candidate.kind=sample.kind;slot=&candidate;break;
            }
        }
        if(!slot){InterlockedIncrement(&channel->wait_drops);return;}
    }
    InterlockedIncrement(&slot->sequence);MemoryBarrier();
    slot->thread_creation=birth;slot->sample=sample;
    MemoryBarrier();InterlockedIncrement(&slot->sequence);
}
// Only initialization failure/offline fixtures close explicitly. At game process
// teardown Windows releases the mapping; no new thread or exit hook is needed.
static void CloseChannel(){
    InterlockedExchange(&waitGate,0);InterlockedIncrement(&channelEpoch);
    SharedBlock* old=channel;channel=nullptr;
    if(old)UnmapViewOfFile(old);
    if(mapping){CloseHandle(mapping);mapping=nullptr;}
}
}
