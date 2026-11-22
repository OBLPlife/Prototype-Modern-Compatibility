#pragma once

// Only the final removal of a nearby tendril segment reads this value.  The
// simulation driver is the single writer during a world step.  On x86 the
// aligned 32-bit publication can be read by the native action without a lock.
alignas(4) static volatile LONG tendrilCanRetire = 1;
static BYTE** tendrilActiveDriver;
static BYTE* tendrilPrepareReturn;
static BYTE* tendrilResetReturn;
static BYTE* tendrilClockOwner;
static DWORD tendrilLastNativeLow, tendrilLastNativeHigh;
static DWORD tendrilRetirementLow, tendrilRetirementHigh;

// Enter immediately after the original QPC-to-microseconds call. EDX:EAX is
// now, ESI is the driver. The original five bytes assign EBX and EDI. ECX
// and EFLAGS are restored, and no floating-point register or OS clock is used.
__declspec(naked) static void ObserveTendrilPrepare() {
    __asm {
        pushfd
        push ecx
        mov ebx, dword ptr [tendrilActiveDriver]
        cmp esi, dword ptr [ebx]
        jne allowRetirement
        cmp dword ptr [esi+70h], 0
        jne fixedRate

        mov ebx, dword ptr [esi+78h]
        mov edi, dword ptr [esi+7ch]
        cmp esi, dword ptr [tendrilClockOwner]
        jne seedAnchor
        cmp ebx, dword ptr [tendrilLastNativeLow]
        jne seedAnchor
        cmp edi, dword ptr [tendrilLastNativeHigh]
        je sampleClock
    seedAnchor:
        mov dword ptr [tendrilClockOwner], esi
        mov dword ptr [tendrilRetirementLow], ebx
        mov dword ptr [tendrilRetirementHigh], edi
    sampleClock:
        mov dword ptr [tendrilLastNativeLow], eax
        mov dword ptr [tendrilLastNativeHigh], edx
        mov ecx, eax
        sub ecx, dword ptr [tendrilRetirementLow]
        mov edi, edx
        sbb edi, dword ptr [tendrilRetirementHigh]
        jne advanceAnchor
        cmp ecx, 15000
        jb deferRetirement
    advanceAnchor:
        // Vanilla discards overshoot. A stall or a backwards clock also
        // consumes one opportunity after the native 66666-us clamp.
        mov dword ptr [tendrilRetirementLow], eax
        mov dword ptr [tendrilRetirementHigh], edx
        cmp byte ptr [esi+88h], 0
        je allowRetirement
        cmp byte ptr [esi+89h], 0
        jne deferRetirement
        jmp allowRetirement
    fixedRate:
        mov dword ptr [tendrilClockOwner], 0
    allowRetirement:
        mov dword ptr [tendrilCanRetire], 1
        jmp resumePrepare
    deferRetirement:
        mov dword ptr [tendrilCanRetire], 0
    resumePrepare:
        pop ecx
        popfd
        mov ebx, dword ptr [esi+70h]
        mov edi, eax
        jmp dword ptr [tendrilPrepareReturn]
    }
}

// A native rate change or explicit reset can take a QPC sample identical to
// the last prepare. Detect that reset even if the raw timestamp did not move.
__declspec(naked) static void ObserveTendrilClockReset() {
    __asm {
        mov dword ptr [esi+78h], eax
        mov dword ptr [esi+7ch], edx
        pushfd
        cmp esi, dword ptr [tendrilClockOwner]
        jne resumeReset
        mov dword ptr [tendrilClockOwner], 0
        mov dword ptr [tendrilCanRetire], 1
    resumeReset:
        popfd
        jmp dword ptr [tendrilResetReturn]
    }
}

static bool PrepareTendrilClockEdits(std::vector<Edit>& edits) {
    const BYTE prepare[] = {0x8b,0x5e,0x70,0x8b,0xf8};
    const BYTE reset[] = {0x89,0x46,0x78,0x89,0x56,0x7c};
    BYTE* prepareSite = engineBase + 0x897b7e;
    BYTE* resetSite = engineBase + 0x897d94;
    BYTE* getter = engineBase + 0x836ad0;
    BYTE** activeSlot = reinterpret_cast<BYTE**>(engineBase + 0x130dfe0);
    if (memcmp(prepareSite,prepare,sizeof(prepare)) || memcmp(resetSite,reset,sizeof(reset)) ||
        getter[0] != 0xa1 || getter[5] != 0xc3 || memcmp(getter+1,&activeSlot,sizeof(activeSlot))) {
        Log("tendril retirement clock instruction conflict");
        return false;
    }
    tendrilActiveDriver = activeSlot;
    tendrilPrepareReturn = prepareSite + sizeof(prepare);
    tendrilResetReturn = resetSite + sizeof(reset);
    edits.push_back(Branch(prepareSite,0xe9,&ObserveTendrilPrepare,sizeof(prepare)));
    edits.push_back(Branch(resetSite,0xe9,&ObserveTendrilClockReset,sizeof(reset)));
    return true;
}
