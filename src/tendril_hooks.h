// The native retract loop reaches this point after snapping a segment to its
// destination. Only its current last segment can be removed. Keep movement and
// contact evaluation at their normal simulation frequency.
static BYTE* tendrilRetireNative;
static BYTE* tendrilContinueNative;

__declspec(naked) static void TendrilReachedSegment() {
    __asm {
        cmp ebx,dword ptr [esi+38h]
        jne continueLoop

        // Preserve the exact flags produced by the displaced CMP.
        pushfd
        push eax
        push ecx
        push edx
        push edi
        mov ecx,dword ptr [engineBase]
        mov eax,dword ptr [esi]
        sub eax,ecx
        cmp eax,0daacc8h                   // TendrilAction
        jne nativeRetirement
        mov edx,dword ptr [esi+1ch]         // DevastatorTargetParams
        test edx,edx
        jz nativeRetirement
        mov eax,dword ptr [edx]
        sub eax,ecx
        cmp eax,0d9e618h
        jne nativeRetirement
        cmp dword ptr [edx+3ch],16h         // DevastatorTendril
        jne nativeRetirement
        cmp byte ptr [edx+28h],0            // Positions without an entity are decorative.
        jne nativeRetirement

        mov eax,dword ptr [edx+0ch]         // Casting actor handle
        cmp ax,0ffffh
        je nativeRetirement
        cmp eax,dword ptr [ecx+126e40ch]    // Local player handle
        jne nativeRetirement
        mov ecx,dword ptr [ecx+128348ch]    // Native actor handle table
        test ecx,ecx
        jz nativeRetirement
        movzx edi,ax
        shr eax,10h
        cmp word ptr [ecx+edi*8+4],ax
        jne nativeRetirement
        cmp dword ptr [ecx+edi*8],0
        je nativeRetirement

        mov eax,dword ptr [edx+18h]         // Target can expire while retracting.
        cmp ax,0ffffh
        je nativeRetirement
        movzx edi,ax
        shr eax,10h
        cmp word ptr [ecx+edi*8+4],ax
        jne nativeRetirement
        cmp dword ptr [ecx+edi*8],0
        je nativeRetirement

        cmp dword ptr [tendrilCanRetire],0
        je deferRetirement
    nativeRetirement:
        pop edi
        pop edx
        pop ecx
        pop eax
        popfd
        jmp dword ptr [tendrilRetireNative]
    deferRetirement:
        pop edi
        pop edx
        pop ecx
        pop eax
        popfd
    continueLoop:
        jmp dword ptr [tendrilContinueNative]
    }
}

static bool PrepareTendrilEdits(std::vector<Edit>& edits) {
    const BYTE reachedLast[] = {0x3b,0x5e,0x38,0x75,0x60};
    const BYTE hidePart[] = {0x8b,0x4e,0x30,0x6a,0x00,0x03,0xcf,0xe8};
    const BYTE decrementLast[] = {0x83,0x46,0x38,0xff,0x79,0x4e,0xc6,0x44,0x24,0x23,0x00};
    const BYTE nextPart[] = {0x83,0xc3,0x01,0x83,0xc7,0x1c,0x3b,0xdd};
    if (memcmp(engineBase+0x138e5a,reachedLast,sizeof(reachedLast)) ||
        memcmp(engineBase+0x138e5f,hidePart,sizeof(hidePart)) ||
        memcmp(engineBase+0x138e6b,decrementLast,sizeof(decrementLast)) ||
        memcmp(engineBase+0x138ebf,nextPart,sizeof(nextPart))) {
        Log("tendril retraction instruction conflict at RVA 00138e5a");
        return false;
    }
    tendrilRetireNative = engineBase+0x138e5f;
    tendrilContinueNative = engineBase+0x138ebf;
    edits.push_back(Branch(engineBase+0x138e5a,0xe9,&TendrilReachedSegment,sizeof(reachedLast)));
    Log("player Tendril Barrage target retraction uses original segment retirement cadence");
    return true;
}
