#pragma once
// Names for portal ends and their owners, read from the game's own client data: the altar's
// name, the nearest discovered place on the map, and players' display names.
#include "portal_types.h"
#include <cstddef>
#include <cstdint>

namespace portal::names {
// Where travelling from this end leads (localized fallback when nothing is named).
const char* Destination(uint8_t* uiClient, const PortalInstance& portal, bool fromField);
// A player's display name; false when not in the client's player list.
bool PlayerName(uint8_t* uiClient, uint32_t playerId, char* out, size_t cap);
// The region (portal_regions.h order) at a world point; -1 outside the map.
int RegionAt(float x, float z);
// A region's name (portal_regions.h order), or a place's by its loca id, in the game's language
// (tags stripped), else english. False when the game's text is not ready yet (english given).
bool RegionTitle(int region, char* out, size_t cap);
bool PlaceTitle(uint32_t loca, const char* english, char* out, size_t cap);
// Whether the point is in a Hollow Halls dungeon (the game's dungeon area boxes).
bool InHollowHalls(float x, float y, float z);
// A point in a Hollow Halls dungeon to its entrance in the overworld (x, z); false outside one.
bool HollowHallsEntrance(float x, float y, float z, float& entranceX, float& entranceZ);
// The Hollow Halls' name for a point in one (game text, else English).
bool HollowHallsName(float x, float y, float z, char* out, size_t cap);
}
