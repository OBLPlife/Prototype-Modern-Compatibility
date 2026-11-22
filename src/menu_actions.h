// Original AVM1 resolution-list repair. Operates on the native loader's owned
// action bytes, before execution; archive files and other UI actions stay intact.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace pm {
namespace menu {
using Bytes = std::vector<uint8_t>;
inline unsigned Word(const uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
struct Actions {
    Bytes code, args;
    const std::vector<std::string_view>& pool;
    explicit Actions(const std::vector<std::string_view>& p):pool(p) {}
    void U16(Bytes& out,unsigned n) { out.push_back(uint8_t(n));out.push_back(uint8_t(n>>8)); }
    void Op(uint8_t op) { code.push_back(op); }
    void Ref(const char* name) {
        unsigned i=unsigned(std::find(pool.begin(),pool.end(),name)-pool.begin());
        args.push_back(i<256?8:9);args.push_back(uint8_t(i));if(i>=256)args.push_back(uint8_t(i>>8));
    }
    void Text(const char* text) {args.push_back(0);while(*text)args.push_back(uint8_t(*text++));args.push_back(0);}
    void Integer(unsigned n) {args.push_back(7);for(unsigned i=0;i<4;++i)args.push_back(uint8_t(n>>(8*i)));}
    void Zero() {args.push_back(6);args.insert(args.end(),8,0);}
    void Push() {Op(0x96);U16(code,unsigned(args.size()));code.insert(code.end(),args.begin(),args.end());args.clear();}
    void Variable(const char* name) {Ref(name);Push();Op(0x1c);}
    void Member(const char* name) {Ref(name);Push();Op(0x4e);}
    size_t Branch(uint8_t op) {Op(op);U16(code,2);U16(code,0);return code.size()-2;}
    void Target(size_t operand,size_t destination) {
        int delta=int(destination)-int(operand+2);code[operand]=uint8_t(delta);code[operand+1]=uint8_t(delta>>8);
    }
};
enum class Result { NotMenu, Patched, AlreadyPatched, Unrecognized };
struct Repair {Result result=Result::NotMenu;size_t offset=0,length=0;};

inline Repair RepairResolutionList(uint8_t* bytes,size_t length) {
    // Both original video menus have their constant pool as the first action.
    if(!bytes||length<5||bytes[0]!=0x88)return {};
    size_t poolEnd=3+Word(bytes+1);
    if(poolEnd>length||poolEnd<5)return {};
    std::vector<std::string_view> pool;
    size_t at=5;unsigned count=Word(bytes+3);
    for(unsigned i=0;i<count;++i) {
        if(at>=poolEnd)return {};
        const void* end=memchr(bytes+at,0,poolEnd-at);if(!end)return {};
        size_t n=static_cast<const uint8_t*>(end)-(bytes+at);
        pool.emplace_back(reinterpret_cast<const char*>(bytes+at),n);at+=n+1;
    }
    if(at!=poolEnd)return {};
    const char* required[]={"rm_GetNumAvailableResolutions","rm_GetAvailableResolutionWidth",
        "rm_GetAvailableResolutionHeight","rm_IsResolutionSupported","SettingVideoResolution",
        "height","width","commonregisters","DoCallBack","j","labels","length","w","h",
        "mLabel","label","mValue","i","supported","push"};
    for(auto name:required)if(std::find(pool.begin(),pool.end(),name)==pool.end())return {};

    // Reconstruct the independently identified legacy loop using this menu's
    // constant indices. This also supports relocated/reordered constant pools.
    // Matching the support-call prefix and i++ suffix pins the exact data flow.
    Actions old(pool);
    old.Variable("height");old.Variable("width");
    old.Ref("rm_IsResolutionSupported");old.Integer(3);old.Ref("commonregisters");old.Push();old.Op(0x1c);
    old.Ref("DoCallBack");old.Push();old.Op(0x52);old.Op(0x12);
    size_t unsupported=old.Branch(0x9d),start=old.code.size();
    old.Ref("j");old.Zero();old.Push();old.Op(0x3c);
    size_t loop=old.code.size();
    old.Variable("j");old.Variable("labels");old.Member("length");old.Op(0x48);old.Op(0x12);
    size_t done=old.Branch(0x9d);
    old.Variable("labels");old.Variable("j");old.Op(0x4e);old.Member("w");old.Variable("width");old.Op(0x49);
    old.Op(0x4c);old.Op(0x12);size_t notWidth=old.Branch(0x9d);old.Op(0x17);
    old.Variable("labels");old.Variable("j");old.Op(0x4e);old.Member("h");old.Variable("height");old.Op(0x49);
    old.Target(notWidth,old.code.size());old.Op(0x12);size_t mismatch=old.Branch(0x9d);
    old.Ref("mLabel");old.Ref("labels");old.Push();old.Op(0x1c);
    old.Variable("j");old.Op(0x4e);old.Member("label");
    old.Ref("mValue");old.Ref("i");old.Push();old.Op(0x1c);
    old.Integer(2);old.Push();old.Op(0x43);
    old.Integer(1);old.Ref("supported");old.Push();old.Op(0x1c);
    old.Ref("push");old.Push();old.Op(0x52);old.Op(0x17);size_t appended=old.Branch(0x99);
    old.Target(mismatch,old.code.size());
    old.Ref("j");old.Ref("j");old.Push();old.Op(0x1c);old.Op(0x50);old.Op(0x1d);
    size_t again=old.Branch(0x99);old.Target(again,loop);
    size_t end=old.code.size();old.Target(unsupported,end);old.Target(done,end);old.Target(appended,end);
    old.Ref("i");old.Ref("i");old.Push();old.Op(0x1c);old.Op(0x50);old.Op(0x1d);

    Actions next(pool);
    next.Ref("mLabel");next.Ref("width");next.Push();next.Op(0x1c);
    next.Text("x");next.Push();next.Op(0x47);
    next.Ref("height");next.Push();next.Op(0x1c);next.Op(0x47);
    next.Ref("mValue");next.Ref("i");next.Push();next.Op(0x1c);
    // Sort presentation order independently from the persistent engine indices.
    // All supported D3D9 display dimensions fit the 16-bit height part.
    next.Text("mSort");next.Ref("width");next.Push();next.Op(0x1c);
    next.Integer(65536);next.Push();next.Op(0x0c); // Multiply
    next.Ref("height");next.Push();next.Op(0x1c);next.Op(0x47);
    next.Integer(3);next.Push();next.Op(0x43);
    next.Integer(1);next.Ref("supported");next.Push();next.Op(0x1c);
    next.Ref("push");next.Push();next.Op(0x52);next.Op(0x17);
    // The original engine registers Array.NUMERIC=16 and Array.sortOn.
    // This executes while building the menu, never in the rendering loop.
    next.Integer(16);next.Text("mSort");next.Integer(2);next.Ref("supported");next.Push();next.Op(0x1c);
    next.Text("sortOn");next.Push();next.Op(0x52);next.Op(0x17);
    size_t exit=next.Branch(0x99);
    if(next.code.size()>end-start)return {Result::Unrecognized};
    // Use valid unreachable one-byte actions, not ActionEnd (0). Native action
    // walkers must still see the original end of the complete frame script.
    next.Target(exit,end-start);next.code.resize(end-start,0x17);
    Bytes patched=old.code;std::copy(next.code.begin(),next.code.end(),patched.begin()+start);

    auto found=std::search(bytes+poolEnd,bytes+length,old.code.begin(),old.code.end());
    if(found==bytes+length) {
        auto installed=std::search(bytes+poolEnd,bytes+length,patched.begin(),patched.end());
        return {installed==bytes+length?Result::Unrecognized:Result::AlreadyPatched};
    }
    if(std::search(found+old.code.size(),bytes+length,old.code.begin(),old.code.end())!=bytes+length)
        return {Result::Unrecognized};
    memcpy(found+start,next.code.data(),next.code.size());
    return {Result::Patched,size_t(found+start-bytes),end-start};
}
}
}
