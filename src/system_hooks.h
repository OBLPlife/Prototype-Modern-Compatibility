// Independently verified original callsites. No global system/API replacement.
// See evidence/engine-audit-0.3.0-pass2.c and docs/engine-audit-0.3.0.md.
typedef MMRESULT (WINAPI* NativeWaveOutOpen)(LPHWAVEOUT,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD);
static NativeWaveOutOpen nativeWaveOutOpen;
static bool NormalizeNativeWaveFormat(const WAVEFORMATEX* input,WAVEFORMATEXTENSIBLE& normalized) {
    // This native renderer builds a 40-byte PCM structure, but cbSize claims
    // 40 *additional* bytes. Do not apply this workaround to other codecs.
    static const GUID pcm={1,0,0x0010,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    if(!input||input->wFormatTag!=WAVE_FORMAT_EXTENSIBLE||input->cbSize!=sizeof(WAVEFORMATEXTENSIBLE)||
       input->wBitsPerSample!=16)return false;
    const auto* source=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(input);
    if(memcmp(&source->SubFormat,&pcm,sizeof(pcm)))return false;
    normalized=*source;
    normalized.Format.cbSize=sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
    if(!normalized.Samples.wValidBitsPerSample)normalized.Samples.wValidBitsPerSample=input->wBitsPerSample;
    return true;
}
static MMRESULT WINAPI OpenNativeWaveOut(LPHWAVEOUT output,UINT device,const WAVEFORMATEX* format,
                                        DWORD_PTR callback,DWORD_PTR instance,DWORD flags) {
    WAVEFORMATEXTENSIBLE normalized={};
    bool repaired=NormalizeNativeWaveFormat(format,normalized);
    MMRESULT result=nativeWaveOutOpen(output,device,repaired?&normalized.Format:format,callback,instance,flags);
    Log("native wave output result=%u repaired-format=%u channels=%u rate=%lu bits=%u; callback/device unchanged",
        result,repaired,format?format->nChannels:0,format?format->nSamplesPerSec:0,format?format->wBitsPerSample:0);
    return result;
}
static bool PrepareSystemEdits(std::vector<Edit>& edits) {
    // CheckDisplayGuards already verified the complete six-byte callsites with
    // relocation adjustment before any edit is committed.
    nativeWaveOutOpen=*reinterpret_cast<NativeWaveOutOpen*>(engineBase+0x1332270);
    if(!nativeWaveOutOpen)return false;
    edits.push_back(Branch(engineBase+0xab7fd9,0xe8,&OpenNativeWaveOut,6));
    // The original constructor pins a new thread using 1 << (index & 31).
    // Keep the OS-assigned eligible CPUs instead. Neither return value nor
    // flags from SetThreadAffinityMask are consumed by the following code.
    Edit affinity;affinity.site=engineBase+0x6450b5;
    affinity.before.assign(affinity.site,affinity.site+6);
    affinity.after={0x83,0xc4,0x08,0x33,0xc0,0x90}; // consume its two stdcall arguments
    edits.push_back(affinity);
    Log("native thread fixed-core binding removed; worker capacity and process affinity unchanged; PCM extension length normalized");
    return true;
}
