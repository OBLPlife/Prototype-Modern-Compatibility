#pragma once
#include <cmath>
#include <cstdint>
namespace pmphysics {
struct Vector {float x,y,z;};
static_assert(sizeof(Vector)==12,"native thiscall vectors occupy three stack words");
struct LinearStep {double displacement[3],velocity[3];};
struct RoundingRemainder {
    uintptr_t key=0;float last[6]={};double remainder[6]={};
    void Read(uintptr_t object,const float actual[6],double precise[6]) const {
        for(unsigned i=0;i<6;++i)precise[i]=static_cast<double>(actual[i])+((key==object&&last[i]==actual[i])?remainder[i]:0.0);
    }
    void Write(uintptr_t object,const double precise[6],float actual[6]) {
        key=object;
        for(unsigned i=0;i<6;++i){actual[i]=last[i]=static_cast<float>(precise[i]);remainder[i]=precise[i]-actual[i];}
    }
};
// Preserve the original damping at its normal 60Hz reference, then express it
// as a rate per second. The native expression embeds exp(-200*dt) inside a
// velocity-dependent ratio; repeating that ratio at a different frequency is
// not the same physical drag. This constant is a calibration, not a frame cap.
inline double DampingRate(const double velocity[3]) {
    const double reference=1.0/60.0,epsilon=static_cast<double>(9.999999747378752e-5f);
    double speed2=velocity[0]*velocity[0]+velocity[1]*velocity[1]+velocity[2]*velocity[2];
    double removed=-std::expm1(-200.0*reference)*epsilon/(speed2+epsilon);
    return -std::log1p(-removed)/reference;
}
// Exact constant-acceleration/constant-drag local solution, evaluated with
// stable series near zero. It cannot overshoot merely because drag*dt > 2.
inline void Solve(const double velocity[3],const double acceleration[3],double dt,double rate,LinearStep& result) {
    double z=rate*dt,decay=std::exp(-z),first,second;
    if(std::abs(z)<0.001) {
        first=dt*(1-z/2+z*z/6-z*z*z/24+z*z*z*z/120);
        second=dt*dt*(0.5-z/6+z*z/24-z*z*z/120+z*z*z*z/720);
    } else {
        first=-std::expm1(-z)/rate;second=(dt-first)/rate;
    }
    for(unsigned axis=0;axis<3;++axis) {
        result.velocity[axis]=velocity[axis]*decay+acceleration[axis]*first;
        result.displacement[axis]=velocity[axis]*first+acceleration[axis]*second;
    }
}
inline LinearStep Midpoint(const double velocity[3],const double acceleration[3],double dt) {
    // Estimate drag at the midpoint, preserving the 1/2*a*dt^2 displacement
    // absent from the native explicit-Euler position update.
    LinearStep midpoint,result;
    Solve(velocity,acceleration,dt*0.5,DampingRate(velocity),midpoint);
    Solve(velocity,acceleration,dt,DampingRate(midpoint.velocity),result);
    return result;
}
inline LinearStep Integrate(const double velocity[3],const double acceleration[3],double dt,unsigned depth=0) {
    auto coarse=Midpoint(velocity,acceleration,dt);
    auto first=Midpoint(velocity,acceleration,dt*0.5);
    auto second=Midpoint(first.velocity,acceleration,dt*0.5);
    bool accurate=true;
    for(unsigned i=0;i<3;++i) {
        second.displacement[i]+=first.displacement[i];
        accurate=accurate&&std::abs(second.velocity[i]-coarse.velocity[i])<=dt*1e-6&&
            std::abs(second.displacement[i]-coarse.displacement[i])<=dt*1e-7;
    }
    if(accurate||depth==10)return second;
    first=Integrate(velocity,acceleration,dt*0.5,depth+1);
    second=Integrate(first.velocity,acceleration,dt*0.5,depth+1);
    for(unsigned i=0;i<3;++i)second.displacement[i]+=first.displacement[i];
    return second;
}
}
