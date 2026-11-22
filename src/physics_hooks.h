// The original integrate-kernel call, independently fingerprinted. Retain its
// impulse/constraint correction, orientation, inertia and angular-velocity work.
#include "physics_step.h"
using PhysicsVector=pmphysics::Vector;
typedef void (__thiscall* NativeIntegrate)(BYTE*,BYTE*,float,PhysicsVector,PhysicsVector,PhysicsVector,PhysicsVector);
static NativeIntegrate nativeIntegrate;
// Bounded rounding remainders, not snapshots of game objects. External position
// or velocity writes invalidate the corresponding remainder immediately. A
// hash collision discards sub-float-ULP precision only; it never reuses another
// body's physical state. No allocation or unbounded object-lifetime registry.
struct PhysicsRemainderSlot {SRWLOCK lock=SRWLOCK_INIT;pmphysics::RoundingRemainder value;};
static PhysicsRemainderSlot physicsRemainders[1024];
static PhysicsRemainderSlot& PhysicsRemainder(BYTE* body) {
    uintptr_t key=reinterpret_cast<uintptr_t>(body);key^=key>>16;key*=0x9e3779b1u;
    return physicsRemainders[(key>>10)&1023];
}
static void __fastcall IntegrateBody(BYTE* body,void*,BYTE* settings,float dt,PhysicsVector impulse,
                                     PhysicsVector angularImpulse,PhysicsVector correction,PhysicsVector angularCorrection) {
    double velocity[3],acceleration[3],position[3];
    float actual[6];double precise[6];
    for(unsigned i=0;i<3;++i){actual[i]=*reinterpret_cast<float*>(body+0xac+i*4);actual[i+3]=*reinterpret_cast<float*>(body+0x20+i*4);}
    auto& slot=PhysicsRemainder(body);AcquireSRWLockShared(&slot.lock);
    slot.value.Read(reinterpret_cast<uintptr_t>(body),actual,precise);ReleaseSRWLockShared(&slot.lock);
    const float* linearImpulse=&impulse.x;const float* linearCorrection=&correction.x;
    const float inverseMass=*reinterpret_cast<float*>(body+0x9c);
    bool valid=dt>0&&std::isfinite(dt)&&std::isfinite(inverseMass);
    for(unsigned axis=0;axis<3;++axis) {
        // Match the original float addition for instantaneous impulses before
        // integrating continuous forces. Never distribute an impulse over dt.
        velocity[axis]=linearImpulse[axis]!=0?static_cast<float>(actual[axis+3]+linearImpulse[axis]):precise[axis+3];
        acceleration[axis]=static_cast<double>(*reinterpret_cast<float*>(body+0x50+axis*4))*inverseMass;
        position[axis]=precise[axis];
        if(settings[0x20]&&linearCorrection[axis]!=0)position[axis]=static_cast<float>(actual[axis]+linearCorrection[axis]);
        valid=valid&&std::isfinite(velocity[axis])&&std::isfinite(acceleration[axis])&&std::isfinite(position[axis]);
    }
    nativeIntegrate(body,settings,dt,impulse,angularImpulse,correction,angularCorrection);
    if(!valid)return; // original invalid-state diagnostics remain authoritative
    auto integrated=pmphysics::Integrate(velocity,acceleration,dt);
    for(unsigned i=0;i<3;++i){precise[i]=position[i]+integrated.displacement[i];precise[i+3]=integrated.velocity[i];}
    AcquireSRWLockExclusive(&slot.lock);slot.value.Write(reinterpret_cast<uintptr_t>(body),precise,actual);ReleaseSRWLockExclusive(&slot.lock);
    for(unsigned axis=0;axis<3;++axis) {
        *reinterpret_cast<float*>(body+0xac+axis*4)=actual[axis];
        *reinterpret_cast<float*>(body+0x20+axis*4)=actual[axis+3];
        // Preserve the solver's velocity-before-continuous-force convention.
        *reinterpret_cast<float*>(body+0x38+axis*4)=static_cast<float>(integrated.velocity[axis]-acceleration[axis]*dt);
        double center=0;
        for(unsigned column=0;column<3;++column)
            center+=static_cast<double>(*reinterpret_cast<float*>(body+0xf0+column*16+axis*4))*
                *reinterpret_cast<float*>(body+0xa0+column*4);
        *reinterpret_cast<float*>(body+0x120+axis*4)=static_cast<float>(*reinterpret_cast<float*>(body+0xac+axis*4)-center);
    }
}
static bool PreparePhysicsEdits(std::vector<Edit>& edits) {
    nativeIntegrate=reinterpret_cast<NativeIntegrate>(engineBase+0x6bd8c0);
    edits.push_back(Branch(engineBase+0x6be838,0xe8,&IntegrateBody,5));
    Log("rigid-body linear integration uses elapsed-time force displacement and time-normalized native drag; native collision impulses and angular integration retained");
    return true;
}
