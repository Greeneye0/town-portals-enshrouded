#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "portal_names.h"
#include "portal_render.h"
#include "portal_regions.h"
#include "portal_locations.h"
#include <cstdio>
#include <cstring>
#include <iterator>

namespace portal::names {
namespace {
// Where a portal end leads, for the prompt and the loading screen: the nearest named map
// marker - a discovered place - within 150 m. Map markers are 0xA0-byte entries in two arrays
// on the UI client with their fixed-point position at +0x10 and name text (UiText) at +0x68.
bool CopyName(const char* text, uint64_t length, char* out, size_t cap) {
    if (!text || length == 0 || length >= cap || uint8_t(text[0]) < 0x20) return false;
    std::memcpy(out, text, size_t(length));
    out[length] = 0;
    return true;
}

bool ReadNearestPlace(uint8_t* uiClient, float x, float z, char* out, size_t cap) {
    __try {
        float best = 150.0f * 150.0f;
        bool found = false;
        for (size_t offset : {size_t(0x36ea00), size_t(0x3e6a78)}) {
            auto* markers = *reinterpret_cast<uint8_t**>(uiClient + offset);
            uint64_t count = *reinterpret_cast<uint64_t*>(uiClient + offset + 8);
            if (!markers || count > 10000) continue;
            for (uint64_t i = 0; i < count; ++i) {
                uint8_t* m = markers + i * 0xA0;
                if ((*reinterpret_cast<uint32_t*>(m) & 0xFFFF0000u) == 0x77700000u) continue; // map mod's own pins
                float mx = float(double(*reinterpret_cast<int64_t*>(m + 0x10)) * portal::kInvFixedScale);
                float mz = float(double(*reinterpret_cast<int64_t*>(m + 0x20)) * portal::kInvFixedScale);
                float d = (mx - x) * (mx - x) + (mz - z) * (mz - z);
                if (d < best && CopyName(*reinterpret_cast<const char**>(m + 0x68),
                                         *reinterpret_cast<uint64_t*>(m + 0x70), out, cap)) {
                    best = d;
                    found = true;
                }
            }
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// A player's display name from the client's player list (0x240-byte entries at
// uiClient+0x4642c0, id at +0x00; name text at +0xc0, else +0xd8 - as the map mod reads it).
bool ReadPlayerName(uint8_t* uiClient, uint32_t playerId, char* out, size_t cap) {
    __try {
        auto* players = *reinterpret_cast<uint8_t**>(uiClient + 0x4642c0);
        uint64_t count = *reinterpret_cast<uint64_t*>(uiClient + 0x4642c8);
        if (!players || !playerId || count > 64) return false;
        for (uint64_t i = 0; i < count; ++i) {
            uint8_t* p = players + i * 0x240;
            if (*reinterpret_cast<uint32_t*>(p) != playerId) continue;
            if (CopyName(*reinterpret_cast<const char**>(p + 0xc0), *reinterpret_cast<uint64_t*>(p + 0xc8), out, cap)) return true;
            return CopyName(*reinterpret_cast<const char**>(p + 0xd8), *reinterpret_cast<uint64_t*>(p + 0xe0), out, cap);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// The region at a world point (defined below, outside this unnamed namespace).
int RegionAtImpl(float x, float z) {
    int cx = int(x / float(kRegionCellMeters)), cz = int(z / float(kRegionCellMeters));
    if (cx < 0 || cz < 0 || cx >= kRegionGridSize || cz >= kRegionGridSize) return -1;
    int at = 0;
    for (uint32_t i = kRegionRowStart[cz]; i < kRegionRowStart[cz + 1]; ++i) {
        at += kRegionRuns[i].cells;
        if (cx < at) return kRegionRuns[i].region < kRegionCount ? kRegionRuns[i].region : -1;
    }
    return -1;
}

bool g_fallback = false;

bool RegionName(float x, float z, char* out, size_t cap) {
    int region = RegionAtImpl(x, z);
    if (region < 0) return false;
    if (render::GameText(kRegionNameLoca[region], out, cap)) return true;
    g_fallback = true;
    std::snprintf(out, cap, "%s", kRegionNameEnglish[region]);
    return true;
}

// The game's named location (town, camp, ruin, lake ...) whose trigger area holds the point:
// the narrowest one, from tools/make_locations.py. Game text can carry <location> tags.
bool LocationName(float x, float y, float z, char* out, size_t cap) {
    int index = FindGameLocation(x, y, z);
    if (index < 0) return false;
    const GameLocation& location = kGameLocations[index];
    char raw[128]{};
    if (!render::GameText(location.nameLoca, raw, sizeof(raw))) {
        g_fallback = true;
        std::snprintf(out, cap, "%s", location.english);
        return true;
    }
    size_t n = 0;
    bool inTag = false;
    for (const char* c = raw; *c && n + 1 < cap; ++c) {
        if (*c == '<') inTag = true;
        else if (*c == '>') inTag = false;
        else if (!inTag) out[n++] = *c;
    }
    out[n] = 0;
    if (!n) std::snprintf(out, cap, "%s", location.english);
    return true;
}

struct Cached { uint64_t portalId = 0, tick = 0; bool fromField = false; char name[80]{}; };
Cached g_destinations[16];
} // namespace

// The region at a world point from the biome map: 8 m cells, rows from the south, each row
// run-length encoded. -1 outside the map.
int RegionAt(float x, float z) { return RegionAtImpl(x, z); }

bool PlaceTitle(uint32_t loca, const char* english, char* out, size_t cap) {
    char raw[128]{};
    size_t n = 0;
    if (render::GameText(loca, raw, sizeof(raw))) {
        bool inTag = false;
        for (const char* c = raw; *c && n + 1 < cap; ++c) {
            if (*c == '<') inTag = true;
            else if (*c == '>') inTag = false;
            else if (!inTag) out[n++] = *c;
        }
        out[n] = 0;
    }
    if (n) return true;
    std::snprintf(out, cap, "%s", english);
    return false;
}
bool RegionTitle(int region, char* out, size_t cap) {
    if (region < 0 || region >= int(kRegionCount)) { if (cap) out[0] = 0; return false; }
    return PlaceTitle(kRegionNameLoca[region], kRegionNameEnglish[region], out, cap);
}

// The destination of travelling from one end of a portal, cached for two seconds per end.
const char* Destination(uint8_t* uiClient, const PortalInstance& p, bool fromField) {
    uint64_t now = GetTickCount64();
    Cached* slot = &g_destinations[0];
    for (auto& c : g_destinations) {
        if (c.portalId == p.portalId && c.fromField == fromField) { slot = &c; break; }
        if (c.tick < slot->tick) slot = &c;
    }
    if (slot->portalId == p.portalId && slot->fromField == fromField && now - slot->tick < 2000) return slot->name;
    slot->portalId = p.portalId;
    slot->fromField = fromField;
    slot->tick = now;
    // The narrowest name for the far end's area: the game's named location around it, else a
    // named place on the map within 150 m, else the region it lies in.
    const auto& end = fromField ? p.baseEnd : p.fieldEnd;
    const float x = end.wx, z = end.wz;
    g_fallback = false;
    // A Hollow Halls dungeon first: it lies at the world's edge, where the region map would name
    // whatever region borders it.
    bool named = (InHollowHalls(x, end.wy, z) && HollowHallsName(x, end.wy, z, slot->name, sizeof(slot->name))) ||
                 LocationName(x, end.wy, z, slot->name, sizeof(slot->name)) ||
                 ReadNearestPlace(uiClient, x, z, slot->name, sizeof(slot->name)) ||
                 RegionName(x, z, slot->name, sizeof(slot->name));
    if (!named)
        std::snprintf(slot->name, sizeof(slot->name), "%s",
                      text::Get(render::Language(), fromField ? text::YourAltar : text::OpeningPlace));
    // The game's text for it was not ready (queued for the UI thread): look again soon.
    if (g_fallback) slot->tick = now - 1800;
    return slot->name;
}

bool PlayerName(uint8_t* uiClient, uint32_t playerId, char* out, size_t cap) {
    return uiClient && ReadPlayerName(uiClient, playerId, out, cap);
}


// The Hollow Halls dungeons: the entrances teleport players into their own region at the map's
// edge. These are the game's "playable area" boxes flagged BigDungeon in the overworld scene
// (they set the player's IsInBigDungeon state), each with its dungeon's map marker, which is
// also the loca id of its name (evidence: C:\temp\enshrouded-re\out\shroud\areas.txt).
// Inside when min <= point < max on every axis.
struct DungeonBox { float minX, minY, minZ, maxX, maxY, maxZ; uint32_t nameLoca; };
constexpr DungeonBox kHollowHalls[] = {
    {359.5f, 298.0f, 9421.0f, 1159.5f, 448.0f, 9771.0f, 0xb12ce8fcu},
    {1139.8f, 249.0f, 9226.2f, 1439.8f, 493.0f, 9767.2f, 0xb12ce8fcu},
    {275.8f, 412.2f, 9473.0f, 675.8f, 612.2f, 9973.0f, 0xb12ce8fcu},
    {1390.8f, 47.8f, 8939.7f, 1960.8f, 447.8f, 9639.7f, 0xb12ce8fcu},
    {359.7f, 134.5f, 8767.3f, 1409.7f, 334.5f, 9067.3f, 0x3ad15149u},
    {258.8f, 350.1f, 9537.8f, 378.8f, 390.1f, 9617.8f, 0x7991af3eu},
    {1224.3f, 530.3f, 7938.7f, 1824.3f, 880.3f, 8838.7f, 0x46ebe7a9u},
    {59.0f, 115.0f, 9003.2f, 609.0f, 315.0f, 9603.2f, 0x0356b963u},
    {26.0f, 106.2f, 8655.5f, 326.0f, 326.2f, 9014.1f, 0x0356b963u},
    {595.0f, 156.0f, 9078.5f, 1165.0f, 306.0f, 9528.5f, 0x0356b963u},
    {466.8f, 527.2f, 8101.1f, 1186.8f, 777.2f, 9351.1f, 0x16a5184du},
    {1418.0f, 163.0f, 9679.3f, 1818.0f, 463.0f, 10079.3f, 0x12f68ccfu},
    {36.5f, 62.5f, 9700.5f, 436.5f, 342.5f, 10150.5f, 0x12f68ccfu},
    {422.5f, 62.5f, 9753.5f, 1422.5f, 342.5f, 10245.5f, 0x12f68ccfu},
};

int HollowHallsBox(float x, float y, float z) {
    for (int i = 0; i < int(std::size(kHollowHalls)); ++i) {
        const auto& b = kHollowHalls[i];
        if (x >= b.minX && x < b.maxX && y >= b.minY && y < b.maxY && z >= b.minZ && z < b.maxZ) return i;
    }
    return -1;
}

bool InHollowHalls(float x, float y, float z) { return HollowHallsBox(x, y, z) >= 0; }

// A point in a Hollow Halls dungeon (they lie at the world's edge) to its entrance in the
// overworld: the "Hollow Halls" marker that carries the dungeon area's name. False outside one,
// or for an area with no such marker.
bool HollowHallsEntrance(float x, float y, float z, float& entranceX, float& entranceZ) {
    const int at = HollowHallsBox(x, y, z);
    if (at < 0) return false;
    for (const auto& location : kGameLocations)
        if (location.nameLoca == kHollowHalls[at].nameLoca && std::strcmp(location.english, "Hollow Halls") == 0) {
            entranceX = location.cx;
            entranceZ = location.cz;
            return true;
        }
    return false;
}

bool HollowHallsName(float x, float y, float z, char* out, size_t cap) {
    const int at = HollowHallsBox(x, y, z);
    if (at >= 0 && render::GameText(kHollowHalls[at].nameLoca, out, cap)) return true;
    std::snprintf(out, cap, "Hollow Halls");
    return true;
}
}
