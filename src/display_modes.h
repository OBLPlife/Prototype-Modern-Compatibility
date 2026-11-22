#pragma once
#include <algorithm>
#include <vector>
#include <stdint.h>
namespace pm {
struct Mode {uint32_t width,height;};
inline bool SameMode(const Mode& a,const Mode& b){return a.width==b.width&&a.height==b.height;}
inline std::vector<Mode> MergeModes(const std::vector<Mode>& legacy,std::vector<Mode> actual) {
    // Preserve all legacy option indices. New device modes have a stable order
    // independent of D3D refresh-rate enumeration and format duplicates.
    std::sort(actual.begin(),actual.end(),[](const Mode& a,const Mode& b){return a.width<b.width||(a.width==b.width&&a.height<b.height);});
    std::vector<Mode> result=legacy;
    for(const Mode& mode:actual) {
        if(!mode.width||!mode.height)continue;
        if(std::none_of(result.begin(),result.end(),[&](const Mode& old){return SameMode(old,mode);}))result.push_back(mode);
    }
    return result;
}
inline int FindMode(const std::vector<Mode>& modes,uint32_t width,uint32_t height) {
    for(size_t i=0;i<modes.size();++i)if(modes[i].width==width&&modes[i].height==height)return static_cast<int>(i);
    return -1;
}
inline unsigned ClampMode(int index,size_t count) {
    if(index<0||!count)return 0;
    return static_cast<size_t>(index)<count?static_cast<unsigned>(index):static_cast<unsigned>(count-1);
}
}
