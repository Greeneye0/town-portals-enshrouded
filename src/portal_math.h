#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace portal::math {

struct Vec3 {
    float x{}, y{}, z{};
    Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec3 operator*(float t) const { return {x*t,y*t,z*t}; }
};
inline float Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float LengthSq(Vec3 a) { return Dot(a,a); }
inline bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }

// Invert the same perspective transform used by the game's HUD projection.
inline bool ScreenRay(const float* view, const float* proj, const float* viewport,
                      float sx, float sy, Vec3& direction) {
    if (viewport[2]<=0 || viewport[3]<=0) return false;
    float det=proj[0]*proj[5]-proj[4]*proj[1];
    if (!std::isfinite(det) || std::fabs(det)<1e-8f) return false;
    float x=2*(sx-viewport[0])/viewport[2]-1-proj[8]-proj[12];
    float y=1-2*(sy-viewport[1])/viewport[3]-proj[9]-proj[13];
    float vx=(x*proj[5]-proj[4]*y)/det;
    float vy=(proj[0]*y-x*proj[1])/det;
    direction={vx*view[0]+vy*view[1]+view[2],
               vx*view[4]+vy*view[5]+view[6],
               vx*view[8]+vy*view[9]+view[10]};
    return Finite(direction);
}

inline bool IntersectPlane(Vec3 origin, Vec3 direction, Vec3 center, Vec3 normal, Vec3& hit) {
    float denom=Dot(direction,normal);
    if (!std::isfinite(denom) || std::fabs(denom)<1e-5f) return false;
    float t=Dot(center-origin,normal)/denom;
    if (!std::isfinite(t) || t<=0.08f) return false;
    hit=origin+direction*t;
    return Finite(hit);
}

// Body-sized vertical capsule with a rounded top. The old 0.43 m flat-topped
// cylinder projected as a box ~1.7x wider than the character and cut a
// rectangular hole into the portal. Expansion covers the projected tile
// footprint; only intersections BEFORE the portal count.
inline bool SegmentHitsPlayer(Vec3 from, Vec3 to, Vec3 feet, float expansion=0) {
    Vec3 d=to-from;
    float len=std::sqrt(LengthSq(d));
    if (!std::isfinite(len) || len<0.1f) return false;
    d=d*(1/len);
    float radius=0.32f+std::max(0.0f,expansion);
    float axisLo=feet.y-0.1f+radius,axisHi=std::max(axisLo,feet.y+1.92f-radius);
    float hi=len-0.015f;
    // Closest points between the ray segment from+d*[0,hi] and the vertical
    // axis (feet.x, [axisLo,axisHi], feet.z) (Ericson, RTCD 5.1.9).
    Vec3 r{from.x-feet.x,from.y-axisLo,from.z-feet.z};
    float e=axisHi-axisLo;                 // axis is (0,e,0)
    float b=d.y*e,c=Dot(d,r),f=r.y*e;      // dot(d1,d2), dot(d1,r), dot(d2,r)
    float ee=e*e,denom=ee-b*b;             // a=1 because d is unit length
    float t=0,s=0;
    if (ee<1e-8f) {
        t=std::clamp(-c,0.0f,hi);
    } else {
        // Parallel (vertical) rays: any t works; start at the camera end.
        t=denom>1e-8f?std::clamp((b*f-c*ee)/denom,0.0f,hi):0.0f;
        s=(b*t+f)/ee;
    }
    if (s<0||s>1) {
        s=std::clamp(s,0.0f,1.0f);
        t=std::clamp(s*b-c,0.0f,hi);
    }
    if (t<0.02f) return false;
    Vec3 p=from+d*t;
    float dx=p.x-feet.x,dy=p.y-(axisLo+s*e),dz=p.z-feet.z;
    return dx*dx+dy*dy+dz*dz<=radius*radius;
}

// Visibility is tri-state: unknown queries must not turn into see-through walls.
enum class Visibility : uint8_t { unknown, blocked, clear };
inline bool CanDraw(Visibility visibility) { return visibility==Visibility::clear; }

// The same linked-camera transform used by portal renderers: express in the
// entry frame, turn 180 degrees about up, then express in the exit frame.
inline Vec3 MapPortalPoint(Vec3 point,Vec3 from,Vec3 fromRight,Vec3 fromNormal,
                          Vec3 to,Vec3 toRight,Vec3 toNormal) {
    Vec3 local=point-from;
    return to-toRight*Dot(local,fromRight)+Vec3{0,local.y,0}-toNormal*Dot(local,fromNormal);
}
inline Vec3 MapPortalDirection(Vec3 direction,Vec3 fromRight,Vec3 fromNormal,
                              Vec3 toRight,Vec3 toNormal) {
    return toRight*(-Dot(direction,fromRight))+Vec3{0,direction.y,0}-toNormal*Dot(direction,fromNormal);
}

} // namespace portal::math
