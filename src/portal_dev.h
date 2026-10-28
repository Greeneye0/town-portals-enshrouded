#pragma once
#include <cstdint>

// Optional development-only ABI. All calls must run on the game's UI thread.
// The normal build does not export this entry point.
struct TownPortalDevRequest {
    uint32_t size = sizeof(TownPortalDevRequest);
    uint32_t action{}; // 0=status, 1=open, 2=close, 3=base, 4=field, 5=ray
    float from[3]{};
    float to[3]{};
};
struct TownPortalDevStatus {
    uint32_t size = sizeof(TownPortalDevStatus);
    uint32_t generation{}, screen{}, portals{}, ownPortal{}, rayVisibility{};
    float player[3]{}, field[3]{}, base[3]{};
    uint64_t renderedFrames{}, drawnQuads{}, rayQueries{}, textureUploads{};
    double averageMs{}, maxMs{};
};
using TownPortalDevFn = int (*)(const TownPortalDevRequest*,TownPortalDevStatus*);
