// Steam invokes this program with its original game command. It only watches
// the child it creates; there is no system service or unrelated-process scan.
#define PM_REPAIR_LIBRARY
#include "../tools/repair_existing.cpp"
#include <shellapi.h>
#include <string>
#include <stdarg.h>

static std::wstring bridgeRoot;
static ULONGLONG started;
static void Log(const char* format,...) {
    char message[1024],line[1152];va_list args;va_start(args,format);
    vsnprintf_s(message,sizeof(message),_TRUNCATE,format,args);va_end(args);
    int size=snprintf(line,sizeof(line),"[%llu ms bridge=%lu] %s\r\n",GetTickCount64()-started,GetCurrentProcessId(),message);
    HANDLE file=CreateFileW((bridgeRoot+L"\\PrototypeModern.Startup.log").c_str(),FILE_APPEND_DATA,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written;WriteFile(file,line,size,&written,nullptr);CloseHandle(file);}
}
#include "performance_launch.h"
// Keep Steam's original game command byte-for-byte (UTF-16 code units). The
// first token is our own module name, which cannot contain embedded quotes.
static const wchar_t* GameCommand(const wchar_t* command) {
    while(*command==L' '||*command==L'\t')++command;
    if(*command==L'"'){++command;while(*command&&*command!=L'"')++command;if(*command)++command;}
    else while(*command&&*command!=L' '&&*command!=L'\t')++command;
    while(*command==L' '||*command==L'\t')++command;return command;
}
static void ConfigureInitialWindow(STARTUPINFOW& startup) {
    startup.cb=sizeof(startup);
    // The original launcher uses nCmdShow for its first 800x450 window.
    // Keep that placeholder hidden; the compatibility module reveals the
    // correctly sized window after its native D3D device has a black frame.
    startup.dwFlags|=STARTF_USESHOWWINDOW;
    startup.wShowWindow=SW_HIDE;
}
static int WatchChild(HANDLE process,DWORD pid) {
    const ULONGLONG deadline=GetTickCount64()+60000;
    bool operationSubmitted=false;
    while(GetTickCount64()<deadline) {
        if(WaitForSingleObject(process,100)!=WAIT_TIMEOUT) {
            DWORD exitCode=0;GetExitCodeProcess(process,&exitCode);Log("child pid=%lu exited code=%lu",pid,exitCode);return static_cast<int>(exitCode);
        }
        auto modules=Modules(pid);
        if(Base(modules,L"prototypeenginef.dll")){Log("child pid=%lu engine loaded; startup helper finished",pid);return 0;}
        if(operationSubmitted)continue;
        Observation first={},second={};
        if(!Observe(process,pid,first,modules))continue;
        if(WaitForSingleObject(process,1000)!=WAIT_TIMEOUT||!Observe(process,pid,second,modules)||
            first.thread!=second.thread||first.socket!=second.socket||first.buffer!=second.buffer||first.frame!=second.frame)continue;
        Log("verified startup receive pid=%lu tid=%lu socket=0x%lx stable for 1000 ms",pid,second.thread,second.socket);
        RemoteBlock block={second.socket,RemoteApi(modules,L"WS2_32.dll","getpeername"),RemoteApi(modules,L"kernel32.dll","CancelIoEx"),
            RemoteApi(modules,L"kernel32.dll","GetLastError"),htons(27015),sizeof(sockaddr_in),{},99,-99,0,
            RemoteApi(modules,L"WS2_32.dll","WSAGetLastError")};
        operationSubmitted=true;
        bool completed=RunSocketOperation(process,block);
        Log("peer-checked cancellation completed=%d status=%lu result=%d error=%d",completed,block.status,block.result,block.error);
        if(!completed||block.status!=0||block.result==0)return 7;
    }
    Log("startup observation deadline reached; child left unchanged; helper exits");return 8;
}
#ifndef PM_BRIDGE_TEST
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    started=GetTickCount64();wchar_t ownPath[32768];
    DWORD length=GetModuleFileNameW(nullptr,ownPath,_countof(ownPath));
    if(!length||length>=_countof(ownPath)||!wcsrchr(ownPath,L'\\'))return 2;
    bridgeRoot.assign(ownPath,wcsrchr(ownPath,L'\\')-ownPath);
    int argc=0;wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    const std::wstring expected=bridgeRoot+L"\\prototypef.exe";
    wchar_t target[32768];bool valid=argv&&argc>=2;
    if(valid){DWORD count=GetFullPathNameW(argv[1],_countof(target),target,nullptr);valid=count&&count<_countof(target)&&!_wcsicmp(target,expected.c_str());}
    if(argv)LocalFree(argv);
    if(!valid){Log("no matching Steam game command; no process created");return 2;}
    std::wstring command=GameCommand(GetCommandLineW());
    STARTUPINFOW startup={};ConfigureInitialWindow(startup);PROCESS_INFORMATION child={};
    Log("PrototypeModern 0.1.4 startup bridge begin; initial placeholder window hidden");
    // Inherit Steam's environment and normal process attributes. Do not change
    // affinity, priority, original EXE bytes, authentication, or graphics flags.
    if(!CreateProcessW(expected.c_str(),&command[0],nullptr,nullptr,FALSE,0,nullptr,bridgeRoot.c_str(),&startup,&child)) {
        DWORD error=GetLastError();Log("CreateProcess failed error=%lu",error);
        MessageBoxW(nullptr,L"无法启动原版游戏，请查看游戏目录中的 PrototypeModern.Startup.log。",L"虐杀原形 1",MB_OK|MB_ICONERROR);return 3;
    }
    CloseHandle(child.hThread);Log("created Steam game child pid=%lu",child.dwProcessId);
    StartAutomaticPerformanceCapture(child.hProcess,child.dwProcessId);
    int result=WatchChild(child.hProcess,child.dwProcessId);CloseHandle(child.hProcess);return result;
}
#endif
