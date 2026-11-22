// Test markers belong to the patch, never to the game's profile/save files.
static std::wstring traceSettingsPath;
static const char* testNames[]={"", "exclusive-startup","exclusive-focus","resolution-list",
    "display-cancel-timeout","borderless","windowed","mode-roundtrip","aa-vsync",
    "all-menus","loading-transitions","pause-map-hud","keyboard-mouse","controller",
    "movement","combat-qte","vehicle-physics","save-load","audio","steam-relaunch",
    "frame-rate-comparison","dpi-monitors","long-session","full-story","installer-restore","other-configurations"};
static void OpenLogAt(const std::wstring& root) {
    std::wstring backup=root+L"PrototypeModern.Backup";
    CreateDirectoryW(backup.c_str(),nullptr);
    traceSettingsPath=backup+L"\\test-trace.ini";
    LONG saved=static_cast<LONG>(GetPrivateProfileIntW(L"Test",L"Step",1,traceSettingsPath.c_str()));
    testStep=saved>=1&&saved<static_cast<LONG>(_countof(testNames))?saved:1;
    std::wstring logs=backup+L"\\Logs";
    CreateDirectoryW(logs.c_str(),nullptr);
    std::wstring path=root+L"PrototypeModern.log";
    SYSTEMTIME now={};GetLocalTime(&now);
    wchar_t suffix[100];swprintf_s(suffix,L"\\session-%04u%02u%02u-%02u%02u%02u-%03u-%lu.log",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds,GetCurrentProcessId());
    // If archiving fails (e.g. a reader denies rename), append; never erase evidence.
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)
        MoveFileExW(path.c_str(),(logs+suffix).c_str(),0);
    logFile=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
}
static void OpenLog() {
    wchar_t modulePath[32768];
    DWORD length=GetModuleFileNameW(selfModule,modulePath,_countof(modulePath));
    if(!length||length>=_countof(modulePath))return;
    wchar_t* slash=wcsrchr(modulePath,L'\\');if(!slash)return;slash[1]=0;
    OpenLogAt(modulePath);
}
static void LogTestSession() {
    SYSTEMTIME now={};GetLocalTime(&now);
    Log("test session local=%04u-%02u-%02uT%02u:%02u:%02u pid=%lu step=T%02ld name=%s; F8=next Shift+F8=previous F7=issue; no automatic PASS",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetCurrentProcessId(),testStep,testNames[testStep]);
}
static void TestMarker(bool previous,bool issue) {
    if(issue){Log("TEST ISSUE user-marked; describe visible/audio symptom in checklist; no automatic diagnosis");return;}
    LONG old=InterlockedCompareExchange(&testStep,0,0);
    LONG next=std::max<LONG>(1,std::min<LONG>(static_cast<LONG>(_countof(testNames)-1),old+(previous?-1:1)));
    Log("TEST LEAVE T%02ld outcome=unassessed",old);
    InterlockedExchange(&testStep,next);
    wchar_t number[16];swprintf_s(number,L"%ld",next);
    BOOL saved=!traceSettingsPath.empty()&&WritePrivateProfileStringW(L"Test",L"Step",number,traceSettingsPath.c_str());
    Log("TEST ENTER T%02ld name=%s persisted=%u%s",next,testNames[next],saved,
        next==old?" boundary; marker remains on this test":"");
}
