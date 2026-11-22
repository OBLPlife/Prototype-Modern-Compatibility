#pragma once
// Development diagnostics are opt-in through the local INI. The game keeps
// Steam's token and command line; only the external collector may request UAC.
static std::wstring PerformanceQuote(const std::wstring& value) {
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){++slashes;continue;}
        result.append(c==L'"'?slashes*2+1:slashes,L'\\');slashes=0;result+=c;
    }
    result.append(slashes*2,L'\\');result+=L'"';return result;
}
static std::wstring PerformanceCommand(const std::wstring& host,const std::wstring& script,
                                      const std::wstring& root,DWORD pid,ULONGLONG creation) {
    return PerformanceQuote(host)+L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "+
        PerformanceQuote(script)+L" -TargetProcessId "+std::to_wstring(pid)+L" -TargetStartFileTime "+
        std::to_wstring(creation)+L" -GameDirectory "+PerformanceQuote(root);
}
static void StartAutomaticPerformanceCapture(HANDLE game,DWORD pid) {
    const std::wstring config=bridgeRoot+L"\\PrototypeModern.Performance.ini";
    if(GetPrivateProfileIntW(L"Capture",L"Enabled",0,config.c_str())!=1)return;
    wchar_t script[32768]={},windows[MAX_PATH]={};
    if(!GetPrivateProfileStringW(L"Capture",L"Bootstrap",L"",script,_countof(script),config.c_str())||
       GetFileAttributesW(script)==INVALID_FILE_ATTRIBUTES) {
        Log("performance auto-capture unavailable: bootstrap path missing; game continues");return;
    }
    FILETIME creation={},exit={},kernel={},user={};
    if(!GetProcessTimes(game,&creation,&exit,&kernel,&user)||!GetWindowsDirectoryW(windows,_countof(windows))) {
        Log("performance auto-capture identity failed error=%lu; game continues",GetLastError());return;
    }
    BOOL wow64=FALSE;IsWow64Process(GetCurrentProcess(),&wow64);
    std::wstring host=windows;
    host+=wow64?L"\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe":L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    ULARGE_INTEGER created;created.LowPart=creation.dwLowDateTime;created.HighPart=creation.dwHighDateTime;
    std::wstring command=PerformanceCommand(host,script,bridgeRoot,pid,created.QuadPart);
    STARTUPINFOW startup={};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION helper={};
    if(!CreateProcessW(host.c_str(),&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,bridgeRoot.c_str(),&startup,&helper)) {
        Log("performance auto-capture launch failed error=%lu; game continues",GetLastError());return;
    }
    Log("performance auto-capture requested game=%lu creation=%llu helper=%lu",pid,created.QuadPart,helper.dwProcessId);
    CloseHandle(helper.hThread);CloseHandle(helper.hProcess);
}
