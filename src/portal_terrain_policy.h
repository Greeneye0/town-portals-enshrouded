#pragma once
#include "portal_math.h"
#include <cstdint>

namespace portal::terrain {
constexpr uint32_t kLevels=7, kMaxBlockTouches=256;
constexpr float kRequestRadius=160.f;
struct Bounds {float values[8]{};};
inline bool DestinationBounds(math::Vec3 center,Bounds& out) {
    if(!math::Finite(center))return false;
    // Avoid undefined native float-to-integer conversion on corrupt poses.
    if(std::fabs(center.x)>1000000||std::fabs(center.y)>1000000||std::fabs(center.z)>1000000)return false;
    out={{center.x-kRequestRadius,center.y-kRequestRadius,center.z-kRequestRadius,0,
          center.x+kRequestRadius,center.y+kRequestRadius,center.z+kRequestRadius,0}};
    return true;
}
// Count the X/Z cache entries native 10a8a30 will touch. Do not invoke native
// iteration for malformed grids or a range exceeding the per-update budget.
inline bool BlockTouches(const Bounds& box,float originX,float originZ,uint32_t level,
                         uint32_t cellX,uint32_t cellZ,uint32_t width,uint32_t height,uint32_t& count) {
    count=0;
    if(level>=kLevels||!cellX||!cellZ||!width||!height||width>65536||height>65536||
       !std::isfinite(originX)||!std::isfinite(originZ))return false;
    for(float v:box.values)if(!std::isfinite(v))return false;
    if(box.values[0]>=box.values[4]||box.values[2]>=box.values[6])return false;
    const float scale=32.f/float(1u<<level);
    auto range=[&](float lo,float hi,float origin,uint32_t cell,uint32_t extent,uint64_t& n){
        // Keep float arithmetic consistent with native floorf/ceilf.
        const double first=std::max(0.f,std::floor((lo-origin)/scale));
        const double last=std::max(0.f,std::ceil((hi-origin)/scale));
        if(!std::isfinite(first)||!std::isfinite(last)||first>UINT32_MAX||last>UINT32_MAX-cell)return false;
        const uint64_t a=uint64_t(first)/cell;
        const uint64_t b=std::min<uint64_t>(extent,(uint64_t(last)+cell-1)/cell);
        n=b>a?b-a:0;return true;
    };
    uint64_t x{},z{};
    if(!range(box.values[0],box.values[4],originX,cellX,width,x)||
       !range(box.values[2],box.values[6],originZ,cellZ,height,z)||
       x>kMaxBlockTouches||z>kMaxBlockTouches||x*z>kMaxBlockTouches)return false;
    count=uint32_t(x*z);return true;
}
struct Identity {
    uintptr_t view{},models{},camera{},resources{},mesher{},queue{};
    uintptr_t modelView{},modelCamera{},cameraModels{},meshModels{};
};
inline uint32_t IsolationFaults(const Identity& primary,const Identity& owned) {
    uint32_t errors=0;
    if(!owned.view||!owned.models||!owned.camera||!owned.resources)errors|=1;
    const uintptr_t p[]={primary.view,primary.models,primary.camera,primary.resources,primary.mesher,primary.queue};
    const uintptr_t q[]={owned.view,owned.models,owned.camera,owned.resources,owned.mesher,owned.queue};
    for(auto a:q)if(a)for(auto b:p)if(a==b)errors|=2;
    if(owned.modelView!=owned.view||owned.modelCamera!=owned.camera||owned.cameraModels!=owned.models)errors|=4;
    if(owned.mesher&&(!owned.queue||owned.meshModels!=owned.models))errors|=8;
    return errors;
}
}
