#include "../src/portal_composite_math.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
using namespace portal::math;
int main() {
    // Compare reconstructed UV/depth against independently projected world
    // samples, including yawed cameras, oblique portals, jitter and world offsets.
    std::mt19937 random(71529);std::uniform_real_distribution<float> unit(-1,1);
    for(int test=0;test<2000;++test){
        const float angle=unit(random)*3.14f,c=std::cos(angle),s=std::sin(angle);
        Vec3 camera{unit(random)*4000,unit(random)*1000,unit(random)*4000};
        float view[16]={c,0,s,0, 0,1,0,0, -s,0,c,0,
            -(camera.x*c-camera.z*s),-camera.y,-(camera.x*s+camera.z*c),0};
        // Native affine matrix's unused bottom-right lane must be ignored.
        view[15]=std::numeric_limits<float>::quiet_NaN();
        float projection[16]={.52f,0,0,0, 0,1.85f,0,0, unit(random)*.001f,unit(random)*.001f,0,1, 0,0,.05f,0};
        auto world=[&](Vec3 p){return Vec3{c*p.x+s*p.z,p.y,-s*p.x+c*p.z}+camera;};
        Vec3 center=world({unit(random)*5,unit(random)*3,10+float(test%200)});
        float yaw=unit(random)*1.45f;
        Vec3 horizontal{c*std::cos(yaw)+s*std::sin(yaw),0,-s*std::cos(yaw)+c*std::sin(yaw)};
        Vec3 vertical{0,1.42f,0};PortalScreenPlane plane{};
        assert(ProjectPortalPlane(view,projection,center,horizontal,vertical,plane));
        for(int point=0;point<8;++point){
            double u=unit(random),v=unit(random);
            double p[3]={center.x+horizontal.x*u+vertical.x*v,center.y+horizontal.y*u+vertical.y*v,center.z+horizontal.z*u+vertical.z*v};
            double xyz[4]={p[0]*view[0]+p[1]*view[4]+p[2]*view[8]+view[12],
                p[0]*view[1]+p[1]*view[5]+p[2]*view[9]+view[13],
                p[0]*view[2]+p[1]*view[6]+p[2]*view[10]+view[14],1};
            double clip[4]{};for(int r=0;r<4;++r)for(int k=0;k<4;++k)clip[r]+=projection[k*4+r]*xyz[k];
            float reconstructed[4];EvaluatePortalPlane(plane,float(clip[0]/clip[3]),float(clip[1]/clip[3]),reconstructed);
            assert(reconstructed[2]>0);
            assert(std::fabs(reconstructed[0]/reconstructed[2]-u)<.004);
            assert(std::fabs(reconstructed[1]/reconstructed[2]-v)<.001);
            assert(std::fabs(reconstructed[3]-clip[2]/clip[3])<1e-6);
            // A foreground surface has larger native reverse-Z.
            assert(.05/(xyz[2]*.5)>reconstructed[3]);
            assert(.05/(xyz[2]*2)<reconstructed[3]);
        }
        // Screen + depth back to portal space, for points off the plane too.
        Vec3 normal{horizontal.z,0,-horizontal.x};float local[16]{};
        assert(PortalLocalFromScreen(view,projection,center,horizontal,vertical,normal,local));
        for(int point=0;point<8;++point){
            double u=unit(random)*3,v=unit(random)*2,n=unit(random)*4;
            double p[3]={center.x+horizontal.x*u+normal.x*n,center.y+vertical.y*v,center.z+horizontal.z*u+normal.z*n};
            double xyz[4]={p[0]*view[0]+p[1]*view[4]+p[2]*view[8]+view[12],
                p[0]*view[1]+p[1]*view[5]+p[2]*view[9]+view[13],
                p[0]*view[2]+p[1]*view[6]+p[2]*view[10]+view[14],1};
            if(xyz[2]<1)continue;
            double clip[4]{};for(int r=0;r<4;++r)for(int k=0;k<4;++k)clip[r]+=projection[k*4+r]*xyz[k];
            const double screen[4]={clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3],1};
            double out[4]{};for(int r=0;r<4;++r)for(int k=0;k<4;++k)out[r]+=double(local[r*4+k])*screen[k];
            assert(std::fabs(out[0]/out[3]-u)<.01&&std::fabs(out[1]/out[3]-v)<.01&&std::fabs(out[2]/out[3]-n)<.01);
        }
    }
    float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    float projection[16]={1,0,0,0,0,1,0,0,0,0,0,1,0,0,.05f,0};
    PortalScreenPlane result{};
    assert(!ProjectPortalPlane(identity,projection,{0,0,-3},{1,0,0},{0,1,0},result));
    assert(!ProjectPortalPlane(identity,projection,{0,0,3},{0,0,1},{0,1,0},result));
    projection[0]=std::numeric_limits<float>::quiet_NaN();
    assert(!ProjectPortalPlane(identity,projection,{0,0,3},{1,0,0},{0,1,0},result));
    std::puts("PASS: 16000 perspective/depth samples; degenerate and behind-camera rejection");
}
