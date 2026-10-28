#pragma once
// Open portals for the map mod: a shared table (Local\TownPortalMarkers.<pid>) it reads to
// draw each portal end on its map and minimap in the owner's colour.
#include "portal_types.h"
#include <cstdint>
#include <vector>

namespace portal::markers {
void Publish(uint8_t* uiClient, const std::vector<PortalInstance>& portals, uint64_t now);
}
