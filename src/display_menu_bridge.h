#include "display_text.h"

// The existing GFx external-interface dispatcher uses five thiscall arguments:
// movie, method, 16-byte argument array, count, result. Extend only our own names.
struct UiValue {DWORD type,padding;union {double number;uint64_t bits;};};
static_assert(sizeof(UiValue)==16,"Original GFx value layout");
typedef void (__thiscall* NativeUiExternal)(void*,void*,const char*,const UiValue*,unsigned,UiValue*);
static NativeUiExternal nativeUiExternal;
static std::string TraceUiValue(const UiValue& value) {
    if(value.type==4) {
        const char* text=reinterpret_cast<const char*>(static_cast<uintptr_t>(value.bits));
        if(!text)return "null";
        std::string s(text,strnlen_s(text,150));
        for(auto& c:s)if(static_cast<unsigned char>(c)<32)c=' ';
        return s;
    }
    char s[64];
    if(value.type==3)_snprintf_s(s,sizeof(s),_TRUNCATE,"%.12g",value.number);
    else if(value.type==2)strcpy_s(s,value.bits&0xff?"true":"false");
    else _snprintf_s(s,sizeof(s),_TRUNCATE,"type:%lu",value.type);
    return s;
}
static void TraceUiArguments(const char* method,const UiValue* args,unsigned count) {
    std::string text=method?method:"null";
    for(unsigned i=0;args&&i<count&&i<6;++i){text+=" | ";text+=TraceUiValue(args[i]);}
    Log("UI action %s%s",text.c_str(),count>6?" | remaining arguments omitted":"");
}
static void __fastcall DisplayMenuExternal(void* handler,void*,void* movie,const char* method,
        const UiValue* args,unsigned count,UiValue* result) {
    if(method&&!strcmp(method,"PM_ReportAction")) {
        TraceUiArguments(method,args,count);
        if(result){result->type=2;result->padding=0;result->bits=1;}return;
    }
    if(method&&!strcmp(method,"PM_ReportUiCanvas")) {
        const char* names[]={"loading","main-menu-full","legal-full","save-load","front-restored",
            "options","audio-options","controller-options","gamepad-options","keyboard-options","video-options",
            "extras","credits","promocode","difficulty","controller","events","pause-audio","pause-controller",
            "pause-keyboard","pause-video","powers","quit","save","statistics","woi","loading-symbol"};
        bool ok=count==3&&args&&args[0].type==3&&args[1].type==3&&args[2].type==3&&
            args[0].number>=0&&args[0].number<_countof(names)&&args[0].number==static_cast<int>(args[0].number);
        if(ok)Log("native UI canvas applied=%s ratio=%.6f local xscale=%.4f%%",
            names[static_cast<int>(args[0].number)],args[1].number,args[2].number);
        if(result){result->type=2;result->padding=0;result->bits=ok?1:0;}
        return;
    }
    if(method&&result&&(!strcmp(method,"PM_GetDisplayMode")||!strcmp(method,"PM_GetAppliedDisplayMode"))) {
        LONG mode=!strcmp(method,"PM_GetDisplayMode")?requestedDisplayMode:appliedDisplayMode;
        result->type=3;result->padding=0;result->number=mode;return;
    }
    if(method&&result&&!strcmp(method,"PM_SetDisplayMode")) {
        bool ok=count==1&&args&&args[0].type==3&&args[0].number>=0&&args[0].number<=2&&
            args[0].number==static_cast<LONG>(args[0].number);
        if(ok) {
            LONG mode=static_cast<LONG>(args[0].number);InterlockedExchange(&requestedDisplayMode,mode);
            Log("native video menu requested mode=%s",DisplayModeName(mode));
        }
        result->type=2;result->padding=0;result->bits=ok?1:0;return;
    }
    if(method&&result&&!strcmp(method,"PM_CommitDisplayMode")) {
        result->type=2;result->padding=0;result->bits=CommitDisplayPreference()?1:0;return;
    }
    // Read-only queries run every frame. Record native commands, preserving the
    // dispatcher and its exact arguments/return object. Original x86 confirms
    // these external-interface strings use plain type 4, no managed indirection.
    if(method&&!strcmp(method,"PlaceLuaCall")&&args&&count&&args[0].type==4) {
        std::string name=TraceUiValue(args[0]);
        const char* op=name.c_str();
        if(const char* separator=strchr(op,'_'))op=separator+1;
        if(_strnicmp(op,"Get",3)&&_strnicmp(op,"Is",2)&&_strnicmp(op,"Has",3)&&
           _strnicmp(op,"Can",3)&&_strnicmp(op,"Query",5))
            TraceUiArguments(method,args,count);
    }
    nativeUiExternal(handler,movie,method,args,count,result);
}
