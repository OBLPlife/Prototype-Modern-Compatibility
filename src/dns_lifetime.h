// The original Massive SDK waits only 30 ms before its caller destroys the
// DNS thread wrapper and releases its private heap. A blocked getaddrinfo can
// outlive that heap. Keep the original stop request, but join the actual worker
// before allowing the original shutdown and pool release to continue.
// Evidence: work/PrototypeZh/continue-crash-20261113 (original-code experiment).
static void __cdecl FinishMassiveDns(uint32_t, uint32_t) {
    auto* stop = reinterpret_cast<volatile LONG*>(engineBase + 0x1320298);
    InterlockedExchange(stop, 1);
    BYTE* wrapper = *reinterpret_cast<BYTE**>(engineBase + 0x1320294);
    if (!wrapper) return; // No worker was created, or finished work was cleaned up.
    HANDLE thread = *reinterpret_cast<HANDLE*>(wrapper + 0x14);
    if (!thread) return; // Original CreateThread failed; no worker owns the pool.
    ULONGLONG begin = GetTickCount64();
    DWORD result = WaitForSingleObject(thread, INFINITE);
    Log("Massive DNS shutdown join result=%08lx elapsed=%llu ms; original cleanup follows",
        result, GetTickCount64() - begin);
}

static bool PrepareDnsLifetimeEdits(std::vector<Edit>& edits) {
    BYTE* site = engineBase + 0xa91805;
    const BYTE prefix[] = {0x55, 0x8b, 0xec, 0x83, 0x3d};
    uint32_t address = 0;
    memcpy(&address, site + 5, sizeof(address));
    // Verify the complete first three original instructions, including the
    // relocated active-flag operand. Commit with the existing hook transaction.
    if (memcmp(site, prefix, sizeof(prefix)) || site[9] != 0 ||
        address != reinterpret_cast<uintptr_t>(engineBase + 0x11f21f8)) {
        Log("Massive DNS stop-wait entry conflict; no hooks applied");
        return false;
    }
    edits.push_back(Branch(site, 0xe9, &FinishMassiveDns, 10));
    Log("Massive DNS worker joined before native request/heap release; no localization dependency");
    return true;
}
