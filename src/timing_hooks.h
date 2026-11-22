// Independently located in the original engine: real-time dispatch contains
// rendering, while simulation scheduling is a separate path immediately after
// it. The rate=0 simulation path already measures elapsed microseconds; its
// 15ms gate skips world updates at high render cadences. Keep native elapsed
// time, scaling, pause, fixed-rate callers and the 66666us stall clamp intact.
typedef void (__thiscall* NativeTimedCallback)(BYTE*,unsigned,unsigned,float);
static NativeTimedCallback nativeSimulationCallback,nativeRenderCallback;
static void __fastcall ObserveSimulation(BYTE* self,void*,unsigned us,unsigned ms,float seconds) {
    nativeSimulationCallback(self,us,ms,seconds);
    InterlockedIncrement64(&pmmetrics::simulationCount);
    InterlockedExchangeAdd64(&pmmetrics::simulationMicroseconds,static_cast<LONG64>(ms)*1000+us);
}
static void __fastcall ObserveRender(BYTE* self,void*,unsigned us,unsigned ms,float seconds) {
    bool fresh=*reinterpret_cast<unsigned*>(self+8)!=0;
    nativeRenderCallback(self,us,ms,seconds);
    InterlockedIncrement64(&pmmetrics::renderDispatchCount);
    if(fresh)InterlockedIncrement64(&pmmetrics::renderWithSimulation);
}
static bool PrepareTimingEdits(std::vector<Edit>& edits) {
    const BYTE realtime[]={0x81,0xf9,0xd0,0x07,0,0};
    const BYTE vsyncDefault[]={0x6a,0x01};
    const BYTE simulation[]={0x3d,0x98,0x3a,0,0};
    if(memcmp(engineBase+0x838f90,realtime,sizeof(realtime))||
       memcmp(engineBase+0x4dfbe3,vsyncDefault,sizeof(vsyncDefault))||
       memcmp(engineBase+0x897bf3,simulation,sizeof(simulation))) {
        Log("timing/default synchronization hook conflict; no hooks applied");return false;
    }
    Edit gate;gate.site=engineBase+0x838f90;gate.before.assign(std::begin(realtime),std::end(realtime));
    gate.after={0x81,0xf9,0,0,0,0};edits.push_back(gate);
    Edit vsync;vsync.site=engineBase+0x4dfbe3;vsync.before.assign(std::begin(vsyncDefault),std::end(vsyncDefault));
    vsync.after={0x6a,0};edits.push_back(vsync);
    Edit sim;sim.site=engineBase+0x897bf3;sim.before.assign(std::begin(simulation),std::end(simulation));
    sim.after={0x3d,1,0,0,0};edits.push_back(sim); // one positive native clock tick; never divide by zero
    nativeSimulationCallback=reinterpret_cast<NativeTimedCallback>(engineBase+0x8490e0);
    nativeRenderCallback=reinterpret_cast<NativeTimedCallback>(engineBase+0x849190);
    for(auto item:{std::pair<DWORD,void*>(0xe70a04,reinterpret_cast<void*>(&ObserveSimulation)),
                  std::pair<DWORD,void*>(0xe70a0c,reinterpret_cast<void*>(&ObserveRender))}) {
        Edit slot;slot.site=engineBase+item.first;slot.before.assign(slot.site,slot.site+4);
        BYTE* pointer=reinterpret_cast<BYTE*>(&item.second);slot.after.assign(pointer,pointer+4);edits.push_back(slot);
    }
    Log("2000-us render and 15000-us variable-simulation gates removed; native elapsed time/pause/scale/stall clamp retained; separate callback and Present metrics enabled; VSync default off");
    return true;
}
