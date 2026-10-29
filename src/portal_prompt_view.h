#pragma once
// Whether the player is turned to a portal they stand by, so its prompt (and E) may claim them.
// Distance alone is not enough: beside a portal but looking at a chest, E must open the chest
// (reported by Lomack). The camera's horizontal look decides, as it does for the game's own
// prompts; right at the portal's face it always counts.
#include <cmath>
#include <initializer_list>

namespace portal::promptview {

constexpr float kHalfWidth = 0.9f;   // the portal's half width: looking at either edge counts
constexpr float kCone = 0.6f;        // cos of ~53 degrees either side of where the camera looks
constexpr float kAlwaysWithin = 1.0f;

// px, pz the player; lookX, lookZ the camera's horizontal look (unit); cx, cz the portal's
// centre and rx, rz its right vector (unit).
inline bool Facing(float px, float pz, float lookX, float lookZ, float cx, float cz, float rx, float rz) {
    if (std::hypot(cx - px, cz - pz) < kAlwaysWithin) return true;
    for (float side : {0.0f, -kHalfWidth, kHalfWidth}) {
        const float tx = cx + rx * side - px, tz = cz + rz * side - pz;
        const float len = std::hypot(tx, tz);
        if (len < 1e-3f || (tx * lookX + tz * lookZ) / len >= kCone) return true;
    }
    return false;
}

} // namespace portal::promptview
