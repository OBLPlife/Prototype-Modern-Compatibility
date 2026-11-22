// Fixed storage, bounded work and no per-frame file I/O. Present intervals and
// simulation callbacks are separate measurements; neither proves gameplay.
#pragma once
#include <psapi.h>
#include "engine_telemetry.h"
namespace pmmetrics {
struct Summary {unsigned count;double averageFps,low1Fps,p50Ms,p95Ms,p99Ms,maxMs;};
static Summary Summarize(double* samples,unsigned count) {
    Summary result={};result.count=count;if(!count)return result;
    double sum=0;for(unsigned i=0;i<count;++i)sum+=samples[i];
    std::sort(samples,samples+count);
    unsigned slow=(count+99)/100;double tail=0;
    for(unsigned i=count-slow;i<count;++i)tail+=samples[i];
    auto percentile=[&](unsigned p){return samples[(count*p+99)/100-1];};
    result.averageFps=sum>0?1000.0*count/sum:0;result.low1Fps=tail>0?1000.0*slow/tail:0;
    result.p50Ms=percentile(50);result.p95Ms=percentile(95);result.p99Ms=percentile(99);result.maxMs=samples[count-1];
    return result;
}
alignas(8) static volatile LONG64 simulationCount,simulationMicroseconds,renderDispatchCount,renderWithSimulation;
static LONG64 lastSimulationCount,lastSimulationMicroseconds,lastRenderDispatchCount,lastRenderWithSimulation;
static double samples[4096];static unsigned sampleCount;
static LARGE_INTEGER frequency,previousPresent,windowStart;
static void SnapshotCounters() {
    lastSimulationCount=InterlockedCompareExchange64(&simulationCount,0,0);
    lastSimulationMicroseconds=InterlockedCompareExchange64(&simulationMicroseconds,0,0);
    lastRenderDispatchCount=InterlockedCompareExchange64(&renderDispatchCount,0,0);
    lastRenderWithSimulation=InterlockedCompareExchange64(&renderWithSimulation,0,0);
}
static void RecordPresent(HRESULT result) {
    LARGE_INTEGER now;if(!QueryPerformanceCounter(&now))return;
    pmdiag::Tick(now.QuadPart); // Reuse this QPC; no extra clock read per frame.
    if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
    if(!frequency.QuadPart)return;
    // Occluded/lost/reset frames do not silently contaminate active intervals.
    // Native focus/Reset logs retain the transition and recovery duration.
    if(result!=S_OK){previousPresent.QuadPart=0;sampleCount=0;windowStart=now;return;}
    if(!previousPresent.QuadPart){previousPresent=windowStart=now;SnapshotCounters();return;}
    double interval=1000.0*double(now.QuadPart-previousPresent.QuadPart)/frequency.QuadPart;
    previousPresent=now;if(interval<=0)return;
    samples[sampleCount++]=interval;
    if(sampleCount<_countof(samples)&&now.QuadPart-windowStart.QuadPart<frequency.QuadPart*5)return;
    Summary summary=Summarize(samples,sampleCount);
    LONG64 sim=InterlockedCompareExchange64(&simulationCount,0,0),us=InterlockedCompareExchange64(&simulationMicroseconds,0,0);
    LONG64 render=InterlockedCompareExchange64(&renderDispatchCount,0,0),fresh=InterlockedCompareExchange64(&renderWithSimulation,0,0);
    Log("metrics present intervals=%u avg-fps=%.3f low1-fps=%.3f p50-ms=%.3f p95-ms=%.3f p99-ms=%.3f max-ms=%.3f sim-callbacks=%lld sim-us=%lld render-dispatch=%lld dispatch-with-sim=%lld foreground=%u",
        summary.count,summary.averageFps,summary.low1Fps,summary.p50Ms,summary.p95Ms,summary.p99Ms,summary.maxMs,
        sim-lastSimulationCount,us-lastSimulationMicroseconds,render-lastRenderDispatchCount,fresh-lastRenderWithSimulation,GetForegroundWindow()==gameWindow);
    lastSimulationCount=sim;lastSimulationMicroseconds=us;lastRenderDispatchCount=render;lastRenderWithSimulation=fresh;
    PROCESS_MEMORY_COUNTERS_EX memory={};memory.cb=sizeof(memory);DWORD handles=0;
    using MemoryFunction=BOOL(WINAPI*)(HANDLE,PPROCESS_MEMORY_COUNTERS,DWORD);
    static auto query=reinterpret_cast<MemoryFunction>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32GetProcessMemoryInfo"));
    bool memoryOk=query&&query(GetCurrentProcess(),reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memory),sizeof(memory));
    bool handlesOk=GetProcessHandleCount(GetCurrentProcess(),&handles)!=FALSE;
    Log("resources memory-valid=%u private-bytes=%llu working-set=%llu peak-working-set=%llu handles-valid=%u handles=%lu",
        memoryOk,static_cast<unsigned long long>(memory.PrivateUsage),static_cast<unsigned long long>(memory.WorkingSetSize),
        static_cast<unsigned long long>(memory.PeakWorkingSetSize),handlesOk,handles);
    sampleCount=0;windowStart=now;
}
}
