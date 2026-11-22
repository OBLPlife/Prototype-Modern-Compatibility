#pragma once
#include <cmath>

namespace pmwheel {
// Native UI coordinates are integers. Keep only the subpixel part lost by
// projecting onto the wheel's circle, not a second cursor or a new sensitivity.
struct Remainder {
    double x=0,y=0;
    int lastX=0,lastY=0,width=0,height=0;
    float radius=0;
    bool valid=false;
    void Reset(){*this=Remainder{};}
    void Project(int& px,int& py,int dx,int dy,int w,int h,float r) {
        if(!valid||w!=width||h!=height||r!=radius||
           static_cast<long long>(px)-dx!=lastX||static_cast<long long>(py)-dy!=lastY) {
            x=y=0;
        }
        const double cx=w/2,cy=h/2;
        double preciseX=px+x,preciseY=py+y;
        const double vx=preciseX-cx,vy=preciseY-cy;
        const double length=std::sqrt(vx*vx+vy*vy);
        if(length>r&&length>0) {
            preciseX=cx+vx*r/length;
            preciseY=cy+vy*r/length;
        }
        px=static_cast<int>(preciseX);py=static_cast<int>(preciseY);
        x=preciseX-px;y=preciseY-py;
        lastX=px;lastY=py;width=w;height=h;radius=r;valid=true;
    }
};
}
