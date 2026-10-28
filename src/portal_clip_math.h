#pragma once
#include "portal_math.h"

namespace portal::math {
struct Plane { Vec3 normal{}; float distance{}; };

inline Vec3 InverseRotate(Vec3 v,const float* q) {
    const Vec3 u{-q[0],-q[1],-q[2]};
    const Vec3 cross{u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
    return v*(2*q[3]*q[3]-1)+u*(2*Dot(u,v))+cross*(2*q[3]);
}

// Column-major, left-handed projection with native reverse-Z [0,1]. Replace
// the near plane (w-z >= 0) with the exit half-space. Preserve the far corner
// opposite that plane; ordinary positive-Z formulas have the wrong sign here.
inline bool ClipReverseZ(float* projection,Plane plane) {
    if(!Finite(plane.normal)||!std::isfinite(plane.distance)||plane.distance>=-0.02f)
        return false;
    double inverse[4][8]{};
    for(int r=0;r<4;++r)for(int c=0;c<4;++c){
        if(!std::isfinite(projection[c*4+r]))return false;
        inverse[r][c]=projection[c*4+r];inverse[r][c+4]=r==c?1:0;
    }
    for(int c=0;c<4;++c){
        int pivot=c;
        for(int r=c+1;r<4;++r)if(std::fabs(inverse[r][c])>std::fabs(inverse[pivot][c]))pivot=r;
        if(std::fabs(inverse[pivot][c])<1e-10)return false;
        for(int k=0;k<8;++k)std::swap(inverse[c][k],inverse[pivot][k]);
        double divisor=inverse[c][c];for(double& v:inverse[c])v/=divisor;
        for(int r=0;r<4;++r)if(r!=c){double f=inverse[r][c];for(int k=0;k<8;++k)inverse[r][k]-=f*inverse[c][k];}
    }
    const double corner[4]={plane.normal.x<0?-1.0:1.0,plane.normal.y<0?-1.0:1.0,0,1};
    const double p[4]={plane.normal.x,plane.normal.y,plane.normal.z,plane.distance};
    double denominator=0;
    for(int r=0;r<4;++r)for(int c=0;c<4;++c)denominator+=p[r]*inverse[r][c+4]*corner[c];
    if(!std::isfinite(denominator)||denominator<1e-5)return false;
    float row[4]{};
    for(int c=0;c<4;++c){
        double v=projection[c*4+3]-p[c]/denominator;
        if(!std::isfinite(v)||std::fabs(v)>1e6)return false;
        row[c]=static_cast<float>(v);
    }
    for(int c=0;c<4;++c)projection[c*4+2]=row[c];
    return true;
}
}
