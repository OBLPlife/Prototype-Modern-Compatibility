// Sampled D3D9 GPU command intervals. Requires engine_telemetry.h before this file.
// Call from the bridge under its existing device mutex. Never waits for the GPU,
// flushes a command buffer, changes drawing/state, or writes a game-thread log.
#pragma once
#include <d3d9.h>
#include <stdint.h>
#include <atomic>

namespace pmdiag {
struct D3D9QueryBackend {
    using Query = IDirect3DQuery9;
    IDirect3DDevice9* device;
    explicit D3D9QueryBackend(IDirect3DDevice9* d):device(d){}
    HRESULT Create(D3DQUERYTYPE type,Query** out){return device->CreateQuery(type,out);}
    HRESULT Issue(Query* query,DWORD flags){return query->Issue(flags);}
    HRESULT Read(Query* query,void* data,DWORD bytes){return query->GetData(data,bytes,0);}
    void Release(Query* query){query->Release();}
    uint64_t Now(){LARGE_INTEGER value={};QueryPerformanceCounter(&value);return value.QuadPart;}
    uint64_t Frequency(){LARGE_INTEGER value={};QueryPerformanceFrequency(&value);return value.QuadPart;}
};

// Template keeps the query lifecycle independently testable without a D3D device.
template<class Backend>class GpuProbe {
    using Query = typename Backend::Query;
    enum FastState:unsigned {Idle,Work,Disabled};
    // Only a lock-elision hint. Mutable query/sample state still uses the existing
    // device mutex; the relaxed load does not publish that state to other threads.
    std::atomic<unsigned> fastState{Idle};
    Backend backend;
    Query* queries[12]={}; // frame start/end/frequency/disjoint, up to four upload pairs
    bool active=false,pending=false,disabled=false,presentTimed=false;
    uint64_t cpuFrequency=0,presentBegin=0,nextPoll=0,deadline=0,sampledFrames=0;
    GpuSample sample={};
    unsigned uploadPairs=0,openUpload=4,uploadEpoch=0;
    bool uploadClosed[4]={};
    enum { Start,End,Frequency,Disjoint };
    void ReleaseQueries(){for(auto& q:queries){if(q)backend.Release(q);q=nullptr;}}
    void Publish(unsigned status,HRESULT error=S_OK) {
        sample.status=status;sample.error=error;
        PublishGpu(sample);
        active=pending=presentTimed=false;
        fastState.store(Idle,std::memory_order_relaxed);
    }
    void Failed(HRESULT error) {
        Publish(error==D3DERR_NOTAVAILABLE?2:3,error);
        ReleaseQueries();disabled=true;fastState.store(Disabled,std::memory_order_relaxed);
    }
    bool EnsureQueries() {
        if(queries[0])return true;
        static constexpr D3DQUERYTYPE types[12]={D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,
            D3DQUERYTYPE_TIMESTAMPFREQ,D3DQUERYTYPE_TIMESTAMPDISJOINT,
            D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,
            D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP,D3DQUERYTYPE_TIMESTAMP};
        for(unsigned i=0;i<12;++i){
            HRESULT hr=backend.Create(types[i],&queries[i]);
            if(hr!=S_OK||!queries[i]){Failed(hr==S_OK?E_FAIL:hr);return false;}
        }
        return true;
    }
    bool Issue(unsigned index,DWORD flags) {
        HRESULT hr=backend.Issue(queries[index],flags);
        if(hr!=S_OK){Failed(hr);return false;}return true;
    }
    double Milliseconds(uint64_t ticks)const{return cpuFrequency?1000.0*double(ticks)/double(cpuFrequency):0.0;}
    void Poll(uint64_t now) {
        if(!pending||now<nextPoll)return;
        if(now>=deadline||sample.query_polls>=32){Publish(5,HRESULT_FROM_WIN32(WAIT_TIMEOUT));ReleaseQueries();return;}
        ++sample.query_polls;
        nextPoll=now+(cpuFrequency+19)/20; // at most one poll round per 50 ms
        uint64_t start=0,end=0,frequency=0;BOOL disjoint=TRUE;
        void* values[4]={&start,&end,&frequency,&disjoint};
        const DWORD sizes[4]={sizeof(start),sizeof(end),sizeof(frequency),sizeof(disjoint)};
        // S_FALSE is not a valid answer. At most twelve non-flushing reads per round.
        for(unsigned i=0;i<4;++i){
            HRESULT hr=backend.Read(queries[i],values[i],sizes[i]);
            if(hr==S_FALSE)return;
            if(hr!=S_OK){Failed(hr);return;}
        }
        sample.gpu_start=start;sample.gpu_end=end;sample.gpu_frequency=frequency;
        if(disjoint){sample.query_ready_qpc=backend.Now();Publish(4);return;}
        if(!frequency||end<start){sample.query_ready_qpc=backend.Now();Publish(3,E_FAIL);return;}
        uint64_t uploadTicks=0,previousUploadEnd=start;
        for(unsigned pair=0;pair<uploadPairs;++pair){
            uint64_t begin=0,finish=0;
            HRESULT hr=backend.Read(queries[4+pair*2],&begin,sizeof(begin));
            if(hr==S_FALSE)return;if(hr!=S_OK){Failed(hr);return;}
            hr=backend.Read(queries[5+pair*2],&finish,sizeof(finish));
            if(hr==S_FALSE)return;if(hr!=S_OK){Failed(hr);return;}
            if(!uploadClosed[pair]||finish<begin||begin<previousUploadEnd||finish>end){sample.query_ready_qpc=backend.Now();Publish(3,E_FAIL);return;}
            uploadTicks+=finish-begin;
            previousUploadEnd=finish;
        }
        if(uploadPairs){
            // Bracketed commands on the same GPU timeline, not isolated DMA-engine
            // service time. Gaps/preemption or deferred copy work remain possible.
            sample.upload_gpu_ms=1000.0*double(uploadTicks)/double(frequency);
            sample.upload_gpu_samples=uploadPairs;sample.flags|=0x10;
        }
        sample.query_ready_qpc=backend.Now(); // CPU receipt upper bound, not GPU completion timestamp.
        // GPU timestamp span includes bubbles/preemption between these commands.
        // It is not continuous GPU Busy, nor all pre-BeginScene frame work.
        sample.command_span_ms=1000.0*double(end-start)/double(frequency);
        sample.flags|=1;
        Publish(1);
    }
public:
    struct UploadToken {uint64_t qpc;unsigned slot,epoch;};
    explicit GpuProbe(Backend b):backend(b){}
    ~GpuProbe(){Reset();}
    GpuProbe(const GpuProbe&)=delete;
    GpuProbe& operator=(const GpuProbe&)=delete;
    bool HasPresentWork()const{return fastState.load(std::memory_order_relaxed)==Work;}
    bool MayBeginScene()const{return fastState.load(std::memory_order_relaxed)==Idle;}
    // Invoke only after a successful original BeginScene. Extra BeginScene/EndScene
    // pairs in the same frame do not restart its interval or consume a request.
    void BeginScene() {
        if(disabled||active||pending||!Enabled()||!GpuRequested())return;
        // This is a diagnostic sample serial, not an exact native Present ID.
        // Idle frames perform no counter increment or diagnostic clock read.
        sample={};sample.frame_id=++sampledFrames;
        sample.command_span_ms=sample.cpu_submit_span_ms=sample.present_wall_ms=-1.0;
        sample.upload_wall_ms=0.0;sample.upload_gpu_ms=-1.0;
        uploadPairs=0;openUpload=4;++uploadEpoch;for(auto& closed:uploadClosed)closed=false;
        if(!cpuFrequency)cpuFrequency=backend.Frequency();
        if(!cpuFrequency){Failed(E_FAIL);return;}
        if(!EnsureQueries())return;
        if(!Issue(Disjoint,D3DISSUE_BEGIN)||!Issue(Frequency,D3DISSUE_END)||!Issue(Start,D3DISSUE_END))return;
        sample.qpc_begin=backend.Now();sample.flags|=2|4|8;
        active=true;fastState.store(Work,std::memory_order_relaxed);
    }
    // Invoke immediately before the original Present, still before its GPU flush.
    void BeforePresent() {
        if(!active)return;
        if(openUpload!=4){Publish(5,E_PENDING);ReleaseQueries();return;}
        sample.qpc_end=backend.Now();
        sample.cpu_submit_span_ms=Milliseconds(sample.qpc_end-sample.qpc_begin);
        if(!Issue(End,D3DISSUE_END)||!Issue(Disjoint,D3DISSUE_END))return;
        active=false;pending=presentTimed=true;
        presentBegin=backend.Now();nextPoll=presentBegin;
        deadline=presentBegin+cpuFrequency*2;
    }
    // Original HRESULT is retained by the caller. A failed/occluded frame is not
    // turned into a valid GPU performance result. Normal Presents advance queries.
    void AfterPresent(HRESULT result) {
        if(!pending)return;
        uint64_t now=backend.Now();
        if(presentTimed){sample.present_wall_ms=Milliseconds(now-presentBegin);presentTimed=false;}
        if(result!=S_OK){Publish(5,result);ReleaseQueries();return;}
        Poll(now);
    }
    void Draw(){if(active)++sample.draw_calls;}
    UploadToken BeginUpload(){
        if(!active)return {0,4,uploadEpoch};
        unsigned slot=4;
        // Nested calls stay CPU-only; no overlapping GPU spans are double-counted.
        if(uploadPairs<4&&openUpload==4){
            slot=uploadPairs++;
            if(!Issue(4+slot*2,D3DISSUE_END))return {0,4,uploadEpoch};
            openUpload=slot;
        }else ++sample.upload_gpu_omitted;
        return {backend.Now(),slot,uploadEpoch};
    }
    void EndUpload(UploadToken token){
        if(!token.qpc||!active||token.epoch!=uploadEpoch)return;
        sample.upload_wall_ms+=Milliseconds(backend.Now()-token.qpc);++sample.upload_calls;
        if(token.slot<4){
            if(openUpload!=token.slot||uploadClosed[token.slot]){Publish(5,E_UNEXPECTED);ReleaseQueries();return;}
            if(!Issue(5+token.slot*2,D3DISSUE_END))return;
            uploadClosed[token.slot]=true;openUpload=4;
        }
    }
    // Before any native reset or before releasing the real device. No outstanding
    // diagnostics query survives reset; future supported devices can try again.
    void Reset() {
        if(active||pending)Publish(5,D3DERR_DEVICELOST);
        ReleaseQueries();disabled=false;active=pending=presentTimed=false;++uploadEpoch;
        fastState.store(Idle,std::memory_order_relaxed);
    }
};

using GpuDiagnostics=GpuProbe<D3D9QueryBackend>;
}
