#include "../src/portal_math.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <algorithm>
#include <cmath>
using namespace portal::math;
void Check(bool ok,const char* what) {
    if (!ok) { std::printf("FAIL: %s\n",what);std::exit(1); }
}
int main() {
    float view[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    float proj[16]={2,0,0,0,0,3,0,0,0,0,1,1,0,0,0,0};
    float vp[4]={100,50,1920,1080};
    Vec3 ray{},hit{};
    Check(ScreenRay(view,proj,vp,1060,590,ray),"center screen ray");
    Check(IntersectPlane({},ray,{0,0,10},{0,0,1},hit),"center plane intersection");
    Check(LengthSq(hit-Vec3{0,0,10})<1e-8f,"center projection inversion");
    // Round trip known 3D points, including lens offsets and a rotated camera.
    view[0]=0;view[2]=1;view[8]=-1;view[10]=0;
    proj[8]=0.12f;proj[9]=-0.08f;
    for (float side:{-0.5f,0.0f,0.5f}) for(float up:{-0.4f,0.0f,0.4f}) {
        Vec3 expected{10,up,side};
        float sx=vp[0]+(0.5f+0.5f*(-side/10*proj[0]+proj[8]))*vp[2];
        float sy=vp[1]+(0.5f-0.5f*(up/10*proj[5]+proj[9]))*vp[3];
        Check(ScreenRay(view,proj,vp,sx,sy,ray),"rotated screen ray");
        Check(IntersectPlane({},ray,{10,0,0},{1,0,0},hit),"rotated plane intersection");
        Check(LengthSq(hit-expected)<1e-8f,"rotated projection round trip");
    }
    Check(!IntersectPlane({}, {1,0,0},{0,0,10},{0,0,1},hit),"edge-on plane is rejected");
    Check(!IntersectPlane({}, {0,0,-1},{0,0,10},{0,0,1},hit),"plane behind camera is rejected");
    Check(SegmentHitsPlayer({0,1,0},{0,1,10},{0,0,5}),"player between camera and portal blocks");
    Check(!SegmentHitsPlayer({0,1,0},{0,1,4},{0,0,5}),"player behind portal does not block");
    Check(!SegmentHitsPlayer({0,3,0},{0,3,10},{0,0,5}),"ray above head does not block");
    Check(SegmentHitsPlayer({0.38f,1,0},{0.38f,1,10},{0,0,5},0.1f),"tile footprint protects body edge");
    Check(!SegmentHitsPlayer({0.45f,1,0},{0.45f,1,10},{0,0,5}),"ray beside the shoulders is not masked");
    Check(!SegmentHitsPlayer({0.25f,1.9f,0},{0.25f,1.9f,10},{0,0,5}),"rounded top leaves the corners above the shoulders");
    Check(SegmentHitsPlayer({0,1.85f,0},{0,1.85f,10},{0,0,5}),"head height still blocks");
    Check(SegmentHitsPlayer({0,5,5},{0,-5,5},{0,0,5}),"vertical ray through the body blocks");
    // Compare against dense sampling of the ray for random camera/portal pairs.
    srand(7);
    auto rnd=[](float lo,float hi){return lo+(hi-lo)*float(rand())/float(RAND_MAX);};
    int disagreements=0;
    for (int i=0;i<20000;++i) {
        Vec3 from{rnd(-3,3),rnd(-1,3),rnd(-3,-1)},to{rnd(-3,3),rnd(-1,3),rnd(1,6)},feet{rnd(-.5f,.5f),0,rnd(-.5f,1.5f)};
        float expansion=rnd(0,0.1f),radius=0.32f+expansion;
        float lo=feet.y-0.1f+radius,hiY=feet.y+1.92f-radius;
        Vec3 d=to-from;float len=std::sqrt(LengthSq(d));
        float best=1e9f;
        for (int k=0;k<=4000;++k) {
            float t=std::max(0.02f,(len-0.015f)*float(k)/4000);
            Vec3 p=from+d*(t/len);
            float y=std::clamp(p.y,lo,hiY);
            best=std::min(best,(p.x-feet.x)*(p.x-feet.x)+(p.y-y)*(p.y-y)+(p.z-feet.z)*(p.z-feet.z));
        }
        float margin=0.004f;
        bool sampled=std::sqrt(best)<=radius;
        if (std::fabs(std::sqrt(best)-radius)<margin) continue;
        if (SegmentHitsPlayer(from,to,feet,expansion)!=sampled) ++disagreements;
    }
    if (disagreements) std::printf("capsule disagreements: %d\n",disagreements);
    Check(disagreements==0,"capsule matches 20000 sampled rays");
    Check(!SegmentHitsPlayer({2,1,0},{2,1,10},{0,0,5}),"unobstructed ray beside player");
    Check(!CanDraw(Visibility::unknown)&&!CanDraw(Visibility::blocked)&&CanDraw(Visibility::clear),"unknown visibility fails closed");
    Vec3 mapped=MapPortalPoint({1,2,3},{},{1,0,0},{0,0,1},{10,0,0},{0,0,-1},{1,0,0});
    Check(LengthSq(mapped-Vec3{7,2,1})<1e-8f,"linked camera position maps through a rotated portal");
    Vec3 direction=MapPortalDirection({0,0,-2},{1,0,0},{0,0,1},{0,0,-1},{1,0,0});
    Check(LengthSq(direction-Vec3{2,0,0})<1e-8f,"linked direction preserves speed and rotates toward exit");
    Vec3 original=MapPortalPoint(mapped,{10,0,0},{0,0,-1},{1,0,0},{},{1,0,0},{0,0,1});
    Check(LengthSq(original-Vec3{1,2,3})<1e-8f,"linked camera mapping round trip");
    proj[0]=0;proj[5]=0;
    Check(!ScreenRay(view,proj,vp,1060,590,ray),"singular camera rejected");
    std::puts("PASS: portal projection, clipping, and conservative visibility tests");
}
