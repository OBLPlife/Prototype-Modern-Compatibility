// English resources owned by the compatibility patch. FormatText removes '$'
// before calling the native text-bible query. Preserve that query for every key
// and extend only its missing-entry result for these four new resources.
#pragma once
struct DisplayTextEntry { const char* key; const char* english; };
static const DisplayTextEntry displayTextEntries[] = {
    {"PM_DISPLAY_MODE", "Display Mode"},
    {"PM_DISPLAY_MODE_FULLSCREEN", "Fullscreen"},
    {"PM_DISPLAY_MODE_BORDERLESS", "Borderless"},
    {"PM_DISPLAY_MODE_WINDOWED", "Windowed"}
};
typedef const char* (__cdecl* NativeTextQuery)(const char*);
static NativeTextQuery nativeTextQuery;
static const char* __cdecl QueryDisplayText(const char* key) {
    const char* text=nativeTextQuery(key);
    if(text)return text;
    for(const auto& entry:displayTextEntries)
        if(!strcmp(key,entry.key))return entry.english;
    return nullptr;
}
