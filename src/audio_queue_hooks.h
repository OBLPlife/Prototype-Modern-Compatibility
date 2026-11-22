// Original audio::EventQueue reserve (engine RVA 0x5ba7b0) publishes pending
// commands, then sleeps 60 ms whenever neither contiguous region has room.
// The consumer can release space much earlier. Change only that sleep operand;
// retain the native alignment, strict free-space comparisons, wrap marker,
// atomic cursor operations and execution/destruction of every audio command.
// This reduces polling granularity, not consumer work or audio update cadence.
// Sleep(1) still yields to the OS; it does not guarantee a 1 ms wake-up.
static bool PrepareAudioQueueEdits(std::vector<Edit>& edits) {
    const BYTE waitPath[] = {
        0x8b,0xce,0xe8,0x23,0xff,0xff,0xff, // publish local write cursor
        0x6a,0x3c,0xe8,0x4c,0x7d,0x00,0x00, // Sleep(60)
        0x83,0xc4,0x04
    };
    const BYTE reserveEntry[] = {0x51,0x8b,0x44,0x24,0x08,0x56,0x57,0x6a,0x08};
    if(memcmp(engineBase+0x5ba7e6,waitPath,sizeof(waitPath)) ||
       memcmp(engineBase+0x5ba7b0,reserveEntry,sizeof(reserveEntry))) {
        Log("audio queue reserve instruction conflict; no hooks applied");
        return false;
    }
    Edit edit;
    edit.site=engineBase+0x5ba7ee;
    edit.before={0x3c};
    edit.after={0x01};
    edits.push_back(edit);
    Log("audio command ring full-space polling: Sleep(60) -> Sleep(1); native queue and command work retained; OS wake timing is not guaranteed");
    return true;
}
