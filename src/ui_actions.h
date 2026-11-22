// The original main timeline is recognized before appending our own layout code.
// No original resource bytes are embedded in this module or changed on disk.
#pragma once
#include <cstdint>
#include <cstddef>
#include "ui_layout_bytes.h"
#include "display_menu_bytes.h"
namespace pm { namespace menu {
inline bool IsVideoMenu(const uint8_t* bytes,size_t length) {
    if(!bytes||(length!=14055&&length!=14229)||bytes[length-1]!=0)return false;
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<length;++i)hash=(hash^bytes[i])*1099511628211ull;
    return (length==14055&&hash==0xf49e89052dfac37bull)||(length==14229&&hash==0xde0216dc3283a9beull);
}
inline bool IsLayoutRoot(const uint8_t* bytes,size_t length) {
    if(!bytes||length!=20202||bytes[length-1]!=0)return false;
    uint64_t hash=14695981039346656037ull;
    for(size_t i=0;i<length;++i)hash=(hash^bytes[i])*1099511628211ull;
    return hash==0x477023e0b7cf8250ull;
}
} }
