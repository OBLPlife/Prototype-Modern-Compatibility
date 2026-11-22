// Production include for PrototypeModern_Start only.
#if !defined(PM_REPAIR_LIBRARY)
#error Include from startup_bridge.cpp
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include <vector>

extern "C" const BYTE RemoteBegin[],RemoteEnd[];
struct RemoteBlock {
    DWORD socket,getPeer,cancelIo,lastError,expectedPort,peerLength;
    sockaddr_in peer;
    DWORD status;
    int result,error;
    DWORD socketLastError;
};
static_assert(sizeof(RemoteBlock)==56&&offsetof(RemoteBlock,peer)==24&&offsetof(RemoteBlock,status)==40,"assembly ABI");
struct Module {DWORD base,size;wchar_t name[MAX_PATH];};
struct Observation {DWORD thread,socket,buffer,frame;};
static std::vector<Module> Modules(DWORD pid) {
    std::vector<Module> result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,pid);
    if(snapshot==INVALID_HANDLE_VALUE)return result;
    MODULEENTRY32W entry={};entry.dwSize=sizeof(entry);
    if(Module32FirstW(snapshot,&entry))do {
        Module m={reinterpret_cast<DWORD>(entry.modBaseAddr),entry.modBaseSize,{}};
        wcscpy_s(m.name,entry.szModule);result.push_back(m);
    }while(Module32NextW(snapshot,&entry));
    CloseHandle(snapshot);return result;
}
static bool Read(HANDLE process,DWORD at,void* buffer,SIZE_T length) {
    SIZE_T got=0;return ReadProcessMemory(process,reinterpret_cast<void*>(at),buffer,length,&got)&&got==length;
}
static DWORD Base(const std::vector<Module>& modules,const wchar_t* name) {
    for(const auto& module:modules)if(!_wcsicmp(module.name,name))return module.base;
    return 0;
}
static DWORD RemoteApi(const std::vector<Module>& modules,const wchar_t* library,const char* name) {
    HMODULE local=GetModuleHandleW(library),owner=nullptr;
    FARPROC procedure=GetProcAddress(local,name);if(!procedure)return 0;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(procedure),&owner))return 0;
    wchar_t path[MAX_PATH];DWORD length=GetModuleFileNameW(owner,path,_countof(path));
    const wchar_t* filename=wcsrchr(path,L'\\');
    if(!length||length>=_countof(path)||!filename)return 0;
    DWORD offset=reinterpret_cast<DWORD>(procedure)-reinterpret_cast<DWORD>(owner);
    for(const auto& module:modules)if(!_wcsicmp(module.name,filename+1)&&offset<module.size)
        return module.base+offset;
    return 0;
}
static bool ObserveThread(HANDLE process,DWORD pid,DWORD tid,DWORD image,Observation& observation) {
    HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,tid);
    if(!thread)return false;
    if(GetProcessIdOfThread(thread)!=pid){CloseHandle(thread);return false;}
    DWORD previous=SuspendThread(thread);
    if(previous==DWORD(-1)){CloseHandle(thread);return false;}
    CONTEXT context={};context.ContextFlags=CONTEXT_CONTROL;bool found=false;
    if(previous==0&&GetThreadContext(thread,&context)) {
        DWORD frame=context.Ebp;
        for(unsigned i=0;i<32&&frame>=context.Esp&&frame-context.Esp<65536&&(frame&3)==0;++i) {
            DWORD words[6];if(!Read(process,frame,words,sizeof(words)))break;
            if(words[1]==image+0x134a&&words[4]==128&&words[5]==0) {
                observation={tid,words[2],words[3],frame};found=true;break;
            }
            if(words[0]<=frame)break;frame=words[0];
        }
    }
    // No file I/O, symbols, sockets, or cross-thread waits while suspended.
    ResumeThread(thread);CloseHandle(thread);return found;
}
static bool Observe(HANDLE process,DWORD pid,Observation& observation,std::vector<Module>& modules) {
    modules=Modules(pid);DWORD image=Base(modules,L"prototypef.exe");
    if(!image||Base(modules,L"prototypeenginef.dll"))return false;
    static const BYTE expected[]={0x33,0xdb,0x53,0x68,0x80,0,0,0,0x8d,0x94,0x24,
        0xa4,0x07,0,0,0x52,0x56,0xe8,0xa8,0x18,0,0,0x56,0xe8,0x9c,0x18,0,0};
    BYTE bytes[sizeof(expected)];
    if(!Read(process,image+0x1334,bytes,sizeof(bytes))||memcmp(bytes,expected,sizeof(bytes)))return false;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if(snapshot==INVALID_HANDLE_VALUE)return false;
    THREADENTRY32 entry={};entry.dwSize=sizeof(entry);unsigned matches=0;
    if(Thread32First(snapshot,&entry))do {
        if(entry.th32OwnerProcessID==pid) {
            Observation candidate={};
            if(ObserveThread(process,pid,entry.th32ThreadID,image,candidate)){observation=candidate;++matches;}
        }
    }while(Thread32Next(snapshot,&entry));
    CloseHandle(snapshot);return matches==1;
}
static bool RunSocketOperation(HANDLE process,RemoteBlock& block) {
    SIZE_T codeSize=static_cast<SIZE_T>(RemoteEnd-RemoteBegin);
    if(!codeSize||codeSize>512||!block.getPeer||!block.cancelIo||!block.lastError||!block.socketLastError)return false;
    void* code=VirtualAllocEx(process,nullptr,codeSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    void* data=VirtualAllocEx(process,nullptr,sizeof(block),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    HANDLE thread=nullptr;bool completed=false,ok=false;
    if(code&&data) {
        SIZE_T written=0;DWORD protection=0;
        if(WriteProcessMemory(process,code,RemoteBegin,codeSize,&written)&&written==codeSize&&
            WriteProcessMemory(process,data,&block,sizeof(block),&written)&&written==sizeof(block)&&
            VirtualProtectEx(process,code,codeSize,PAGE_EXECUTE_READ,&protection)&&
            FlushInstructionCache(process,code,codeSize))
            thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(code),data,0,nullptr);
        if(thread) {
            completed=WaitForSingleObject(thread,5000)==WAIT_OBJECT_0;
            DWORD exitCode=0;
            if(completed&&GetExitCodeThread(thread,&exitCode)&&exitCode<=3)
                ok=Read(process,reinterpret_cast<DWORD>(data),&block,sizeof(block));
            if(!completed)printf("operation pending; do not release code/data still potentially in use\n");
        }
    }
    if(!thread||completed) {
        if(code)VirtualFreeEx(process,code,0,MEM_RELEASE);
        if(data)VirtualFreeEx(process,data,0,MEM_RELEASE);
    }
    if(thread)CloseHandle(thread);return ok;
}
