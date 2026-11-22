// Original GrabButtonStruggleAction update, shared by tank/APC and helicopter
// tracks. Input state 1 is a press edge, not a held button. Multiplying this
// one-off contribution by the current frame delta makes rapid updates erase
// the player's progress against the independent, time-based decay track.
// Convert the authored rate using the original variable-simulation 15000-us
// admission threshold. This is a per-press calibration, never a frame limiter.
// The rest of the native action (input window, decay, target, difficulty and
// completion events) remains in place. Zero/invalid dt follows original math.
static const float vehiclePressSeconds=15000.0f/1000000.0f;
static const float vehiclePressGain=1.5f;
static BYTE* vehiclePressReturn;
__declspec(naked) static void VehiclePressCharge() {
    __asm {
        pushfd
        push eax
        mov eax,dword ptr [esp+4ch]
        test eax,eax
        jle nativeDelta
        cmp eax,7f800000h
        jae nativeDelta
        pop eax
        popfd
        mulss xmm0,dword ptr [vehiclePressSeconds]
        mulss xmm0,dword ptr [vehiclePressGain]
        jmp dword ptr [vehiclePressReturn]
    nativeDelta:
        pop eax
        popfd
        mulss xmm0,dword ptr [esp+44h]
        jmp dword ptr [vehiclePressReturn]
    }
}
static bool PrepareVehicleEdits(std::vector<Edit>& edits) {
    const BYTE press[]={0x83,0x38,0x01,0x5f,0x75,0x16,0x8b,0x4e,0x1c,0xf3,0x0f,0x10,0x41,0x20};
    const BYTE charge[]={0xf3,0x0f,0x59,0x44,0x24,0x44,0xf3,0x0f,0x58,0x44,0x24,0x08,0xeb,0x06};
    if(memcmp(engineBase+0xcc819,press,sizeof(press))||
       memcmp(engineBase+0xcc827,charge,sizeof(charge))) {
        Log("vehicle struggle instruction conflict; no hooks applied");return false;
    }
    vehiclePressReturn=engineBase+0xcc82d;
    edits.push_back(Branch(engineBase+0xcc827,0xe9,&VehiclePressCharge,6));
    Log("vehicle struggle press edge gain 1.5x on the calibrated 15000-us award (tank/APC/helicopter)");
    return true;
}
