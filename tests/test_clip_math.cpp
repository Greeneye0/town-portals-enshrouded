#include "../src/portal_clip_math.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace portal::math;
static void Perspective(float* p){std::memset(p,0,64);p[0]=0.5f;p[5]=1;p[10]=-0.1f/999.9f;p[11]=1;p[14]=100/999.9f;}
static float NearDistance(const float* p,Vec3 v){return (p[3]-p[2])*v.x+(p[7]-p[6])*v.y+(p[11]-p[10])*v.z+p[15]-p[14];}
int main(){
 float p[16];Perspective(p);
 assert(ClipReverseZ(p,{{0,0,1},-5}));
 assert(std::fabs(NearDistance(p,{0,0,5}))<1e-5f);
 assert(NearDistance(p,{0,0,4})<0);assert(NearDistance(p,{0,0,6})>0);
 assert(std::fabs(p[10]*1000+p[14])<0.001f); // Original far corner stays at z=0.
 assert(p[0]==0.5f&&p[5]==1&&p[11]==1); // Screen projection is unchanged.
 Perspective(p);assert(ClipReverseZ(p,{{0.6f,0,0.8f},-5}));
 assert(std::fabs(NearDistance(p,{3,0,4}))<1e-5f);
 assert(NearDistance(p,{3.6f,0,4.8f})>0);assert(NearDistance(p,{2.4f,0,3.2f})<0);
 float saved[16];std::memcpy(saved,p,64);assert(!ClipReverseZ(p,{{0,0,1},0}));assert(std::memcmp(p,saved,64)==0);
 float singular[16]{};assert(!ClipReverseZ(singular,{{0,0,1},-1}));
 const float q[4]={0,std::sqrt(0.5f),0,std::sqrt(0.5f)};
 auto v=InverseRotate({1,0,0},q);assert(std::fabs(v.x)<1e-5f&&std::fabs(v.z-1)<1e-5f);
 std::cout<<"reverse-Z exit-plane tests passed\n";
}
