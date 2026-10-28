#include "../src/portal_terrain_policy.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
using namespace portal::terrain;
// Replay the native loader's relevant rules: requested entries can start IO;
// completed entries aged >=2 ticks can be evicted under pressure. IO completion
// is deliberately between 10 Hz render requests at a 60 Hz scene update rate.
bool DelayedLoadSurvives(bool refreshEachUpdate) {
    int lastUse=-100,state=0,completes=-1;bool sampledReady=false;
    for(int tick=0;tick<24;++tick){
        if(refreshEachUpdate||tick%6==0){lastUse=tick;if(tick%6==0&&state==2)sampledReady=true;}
        if(state==1&&tick>=completes)state=2;
        if(state==2&&tick-lastUse>=2)state=0;
        if(state==0&&tick-lastUse<2){state=1;completes=tick+3;}
    }
    return sampledReady;
}
int main() {
    assert(!DelayedLoadSurvives(false));assert(DelayedLoadSurvives(true));
    Bounds box{};assert(DestinationBounds({8192,128,4096},box));
    assert(box.values[0]==8032&&box.values[6]==4256);
    assert(!DestinationBounds({std::numeric_limits<float>::quiet_NaN(),0,0},box));
    assert(!DestinationBounds({1000001,0,0},box));
    std::mt19937 rng(91927);uint32_t verified=0;
    for(int trial=0;trial<3000;++trial){
        float ox=float(int(rng()%4000)-2000),oz=float(int(rng()%4000)-2000);
        assert(DestinationBounds({ox+float(rng()%16000),128,oz+float(rng()%16000)},box));
        const uint32_t level=rng()%7,cellX=32u<<(rng()%4),cellZ=32u<<(rng()%4),width=1+rng()%40,height=1+rng()%40;
        const float scale=32.f/float(1u<<level);
        uint32_t expected=0;
        // Independent geometric overlap count, including origin offsets,
        // regions outside the world and exact half-open cell boundaries.
        for(uint32_t z=0;z<height;++z)for(uint32_t x=0;x<width;++x)
            if(float(x*cellX)*scale+ox<box.values[4]&&float((x+1)*cellX)*scale+ox>box.values[0]&&
               float(z*cellZ)*scale+oz<box.values[6]&&float((z+1)*cellZ)*scale+oz>box.values[2])++expected;
        uint32_t count{};bool allowed=BlockTouches(box,ox,oz,level,cellX,cellZ,width,height,count);
        assert(allowed==(expected<=kMaxBlockTouches));if(allowed)assert(count==expected);++verified;
    }
    uint32_t count{};assert(DestinationBounds({160,0,160},box));
    assert(BlockTouches(box,0,0,6,128,128,20,20,count)&&count==25);
    assert(!BlockTouches(box,0,0,7,128,128,20,20,count));
    assert(!BlockTouches(box,0,0,6,0,128,20,20,count));
    assert(!BlockTouches(box,0,0,6,1,1,65536,65536,count));
    Identity primary{1,2,3,4,5,6,1,3,2,2},owned{11,12,13,14,15,16,11,13,12,12};
    assert(IsolationFaults(primary,owned)==0);
    constexpr uintptr_t Identity::* fields[]={&Identity::view,&Identity::models,&Identity::camera,
        &Identity::resources,&Identity::mesher,&Identity::queue};
    for(auto field:fields){auto bad=owned;bad.*field=primary.*field;assert(IsolationFaults(primary,bad)!=0);}
    auto bad=owned;bad.cameraModels=primary.models;assert(IsolationFaults(primary,bad)&4);
    bad=owned;bad.meshModels=primary.models;assert(IsolationFaults(primary,bad)&8);
    bad=owned;bad.resources=0;assert(IsolationFaults(primary,bad)&1);
    bad=owned;bad.queue=0;assert(IsolationFaults(primary,bad)&8);
    auto unbound=owned;unbound.mesher=unbound.queue=unbound.meshModels=0;
    assert(IsolationFaults(primary,unbound)==0); // native construction before world binding
    std::printf("PASS: %u terrain request regions, eviction replay, resource isolation failures\n",verified);
}
