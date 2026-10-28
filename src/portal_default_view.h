#pragma once
// The camera of a default altar-portal picture (portal_default_images.h): a real snapshot's
// camera (taken in game, 2026-10-28) carried rigidly from its endpoint to the endpoint the
// picture is for, so the portal shader projects it exactly as it projects a snapshot. The
// picture covers the snapshot's whole view; its tangents over kNarrow when narrower.
#include "portal_snapshot_wire.h"

namespace portal::defaultview {
// The reference: endpoint (ground centre, outward normal), camera, and world to clip x, y, w.
constexpr double kRefEnd[3]={3730.75830078125,843.5009765625,1430.10107421875};
constexpr double kRefNormal[2]={0.9072052836418152,-0.4206882119178772};
constexpr double kRefCamera[3]={3730.3046875,845.1209716796875,1430.3114013671875};
constexpr double kRefClip[12]={
    -0.15244325258177402,-8.30067088184884e-05,-0.33014065222988853,1040.9338200440202,
    0.08919538027937224,0.693167077055837,-0.041361587069777855,-859.3760127345952,
    0.8983292579650879,-0.13954311609268188,-0.4165724515914917,-2637.282470703125};
// The picture fills the snapshot's whole view (about 70 by 55 degrees either side), so a portal
// seen from any angle never shows past it: export_defaults.py puts the photo in its middle
// (at 1/1.8 of the frame; at its own angles, 3x, a portal frames only the middle third of the
// landmark) and blurs the photo round it. 1: no narrowing here.
constexpr double kNarrow=1.0;
// Every pixel of a default picture lies this far along the camera's view: a backdrop.
constexpr float kDepth=600.f;

// Fills meta's endpoint, camera and clip for a picture shown at endpoint (x, y, z), normal
// (nx, nz). Colour exposure and the Shroud flag are the caller's.
inline void Place(double x,double y,double z,double nx,double nz,sharing::Metadata& meta) {
    // Endpoint frames: right (nz, 0, -nx), up, normal; columns of M.
    const double r1[3]={kRefNormal[1],0,-kRefNormal[0]},n1[3]={kRefNormal[0],0,kRefNormal[1]};
    const double r2[3]={nz,0,-nx},n2[3]={nx,0,nz},up[3]={0,1,0};
    // R = M1 * M2^T: the new endpoint's frame onto the reference's.
    double R[3][3]{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)R[i][j]=r1[i]*r2[j]+up[i]*up[j]+n1[i]*n2[j];
    const double o2[3]={x,y,z};
    double ro2[3]{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)ro2[i]+=R[i][j]*o2[j];
    // clip'(p) = A (o1 + R (p - o2)) + b = (A R) p + A (o1 - R o2) + b
    for(int row=0;row<3;++row){
        const double* a=kRefClip+row*4;
        double offset=a[3];
        for(int k=0;k<3;++k){
            double col=0;for(int i=0;i<3;++i)col+=a[i]*R[i][k];
            meta.clip[row*4+k]=col;
            offset+=a[k]*(kRefEnd[k]-ro2[k]);
        }
        meta.clip[row*4+3]=offset;
    }
    for(int k=0;k<4;++k){meta.clip[k]*=kNarrow;meta.clip[4+k]*=kNarrow;}
    // The camera: the reference camera's place relative to its endpoint, carried over.
    const double c1[3]={kRefCamera[0]-kRefEnd[0],kRefCamera[1]-kRefEnd[1],kRefCamera[2]-kRefEnd[2]};
    double c2[3]{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)c2[i]+=R[j][i]*c1[j]; // R^T
    meta.x=float(x);meta.y=float(y);meta.z=float(z);meta.nx=float(nx);meta.nz=float(nz);
    meta.cx=float(x+c2[0]);meta.cy=float(y+c2[1]);meta.cz=float(z+c2[2]);
}
}
