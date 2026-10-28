#pragma once
#include "portal_math.h"

namespace portal::math {
// Rows map screen NDC (x,y,1) to homogeneous portal coordinates and reverse-Z.
// Depth is affine in screen space; UV must be divided by the third row.
struct PortalScreenPlane { double rows[4][3]{}; };
inline bool ProjectPortalPlane(const float* view,const float* projection,
                              Vec3 center,Vec3 horizontal,Vec3 vertical,
                              PortalScreenPlane& out) {
    for(int i=0;i<16;++i)if(!std::isfinite(projection[i]))return false;
    if(!Finite(center)||!Finite(horizontal)||!Finite(vertical))return false;
    const Vec3 basis[3]={horizontal,vertical,center};
    double h[3][6]{},z[3]{};
    for(int col=0;col<3;++col){
        const auto p=basis[col];const double w=col==2?1.:0.;
        double v[4]={p.x*double(view[0])+p.y*double(view[4])+p.z*double(view[8])+w*view[12],
            p.x*double(view[1])+p.y*double(view[5])+p.z*double(view[9])+w*view[13],
            p.x*double(view[2])+p.y*double(view[6])+p.z*double(view[10])+w*view[14],w};
        double clip[4]{};for(int r=0;r<4;++r)for(int k=0;k<4;++k)clip[r]+=projection[k*4+r]*v[k];
        for(double value:clip)if(!std::isfinite(value))return false;
        h[0][col]=clip[0];h[1][col]=clip[1];h[2][col]=clip[3];z[col]=clip[2];
    }
    if(h[2][2]<0.1)return false;
    for(int r=0;r<3;++r)h[r][r+3]=1;
    for(int col=0;col<3;++col){
        int pivot=col;for(int r=col+1;r<3;++r)if(std::fabs(h[r][col])>std::fabs(h[pivot][col]))pivot=r;
        if(std::fabs(h[pivot][col])<1e-8)return false;
        for(int k=0;k<6;++k)std::swap(h[pivot][k],h[col][k]);
        double divisor=h[col][col];for(double& v:h[col])v/=divisor;
        for(int r=0;r<3;++r)if(r!=col){double f=h[r][col];for(int k=0;k<6;++k)h[r][k]-=f*h[col][k];}
    }
    PortalScreenPlane result{};
    for(int r=0;r<3;++r)for(int k=0;k<3;++k){result.rows[r][k]=h[r][k+3];result.rows[3][k]+=z[r]*h[r][k+3];}
    for(const auto& row:result.rows)for(double value:row)if(!std::isfinite(value))return false;
    out=result;return true;
}
inline void EvaluatePortalPlane(const PortalScreenPlane& plane,float x,float y,float* out) {
    for(int r=0;r<4;++r)out[r]=float(plane.rows[r][0]*x+plane.rows[r][1]*y+plane.rows[r][2]);
}
// Row-major 4x4 taking (NDC x, NDC y, depth, 1) to homogeneous portal-local
// coordinates (horizontal, vertical, normal axes; divide by the fourth row),
// so a shader can place any depth-buffer pixel relative to the portal.
inline bool PortalLocalFromScreen(const float* view,const float* projection,
                                  Vec3 center,Vec3 horizontal,Vec3 vertical,Vec3 normal,
                                  float out[16]) {
    if(!Finite(center)||!Finite(horizontal)||!Finite(vertical)||!Finite(normal))return false;
    const Vec3 basis[4]={horizontal,vertical,normal,center};
    double m[4][8]{};
    for(int col=0;col<4;++col){
        const auto p=basis[col];const double w=col==3?1.:0.;
        double v[4]={p.x*double(view[0])+p.y*double(view[4])+p.z*double(view[8])+w*view[12],
            p.x*double(view[1])+p.y*double(view[5])+p.z*double(view[9])+w*view[13],
            p.x*double(view[2])+p.y*double(view[6])+p.z*double(view[10])+w*view[14],w};
        for(int r=0;r<4;++r)for(int k=0;k<4;++k)m[r][col]+=double(projection[k*4+r])*v[k];
    }
    // Clip rows are x,y,z,w; screen input is x,y,z (all over w) and 1.
    for(int r=0;r<4;++r)m[r][r+4]=1;
    for(int col=0;col<4;++col){
        int pivot=col;for(int r=col+1;r<4;++r)if(std::fabs(m[r][col])>std::fabs(m[pivot][col]))pivot=r;
        if(std::fabs(m[pivot][col])<1e-12)return false;
        for(int k=0;k<8;++k)std::swap(m[pivot][k],m[col][k]);
        double divisor=m[col][col];for(double& v:m[col])v/=divisor;
        for(int r=0;r<4;++r)if(r!=col){double f=m[r][col];for(int k=0;k<8;++k)m[r][k]-=f*m[col][k];}
    }
    for(int r=0;r<4;++r)for(int k=0;k<4;++k){
        out[r*4+k]=float(m[r][k+4]);
        if(!std::isfinite(out[r*4+k]))return false;
    }
    return true;
}
}
