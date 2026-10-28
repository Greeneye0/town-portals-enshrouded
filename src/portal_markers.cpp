#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "portal_markers.h"
#include "portal_colors.h"
#include "portal_names.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace portal::markers {
namespace {
// Shared with the map mod (UnlockMapMarkers), which draws these on its map and minimap. Both
// mods live in the game process; the pid keeps a game and a dedicated server on one PC apart.
#pragma pack(push, 4)
struct Marker {
    float x, y, z;          // world position of the portal end
    uint32_t color;         // ABGR, the vivid portal colour
    uint32_t ownerPlayerId; // player-list id
    uint8_t side;           // 0 field end, 1 base end (at the Flame Altar)
    uint8_t isLocalOwner;
    uint8_t forming;        // still being cast
    uint8_t reserved;
    float yaw;              // atan2(nz, nx) of the portal normal
    char ownerName[32];
};
struct Table {
    uint32_t magic;             // 'TPM1'
    uint32_t version;           // 1
    volatile uint32_t sequence; // odd while being written (seqlock)
    uint32_t count;
    Marker markers[16];
};
#pragma pack(pop)
static_assert(sizeof(Marker) == 60 && sizeof(Table) == 976); // the map mod checks the same sizes

Table* g_table = nullptr;

Table* Open() {
    if (g_table) return g_table;
    wchar_t name[64]{};
    std::swprintf(name, 64, L"Local\\TownPortalMarkers.%lu", GetCurrentProcessId());
    // Kept for the life of the process: a hot reload reopens the same mapping.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Table), name);
    if (!mapping) return nullptr;
    g_table = static_cast<Table*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Table)));
    if (g_table) {
        g_table->magic = 0x314D5054u;
        g_table->version = 1;
    }
    return g_table;
}
}

void Publish(uint8_t* uiClient, const std::vector<PortalInstance>& portals, uint64_t now) {
    Table* table = Open();
    if (!table) return;
    uint32_t local = uiClient ? *reinterpret_cast<uint32_t*>(uiClient + 0x18) : 0;
    Marker out[16]{};
    uint32_t count = 0;
    for (const auto& p : portals) {
        if (p.closing) continue;
        char owner[32]{};
        names::PlayerName(uiClient, p.ownerPlayerId, owner, sizeof(owner));
        for (int side = 0; side < 2 && count < 16; ++side) {
            const auto& e = side ? p.baseEnd : p.fieldEnd;
            auto& m = out[count++];
            m.x = e.wx; m.y = e.wy; m.z = e.wz;
            m.color = colors::ForPlayer(p.ownerPlayerId, local);
            m.ownerPlayerId = p.ownerPlayerId;
            m.side = uint8_t(side);
            m.isLocalOwner = p.ownerPlayerId && p.ownerPlayerId == local;
            m.forming = now - std::min(now, p.openedTick) < kPortalFormMs;
            m.yaw = std::atan2(e.nz, e.nx);
            std::memcpy(m.ownerName, owner, sizeof(owner));
        }
    }
    table->sequence = table->sequence + 1; // odd: writing
    MemoryBarrier();
    std::memcpy(table->markers, out, sizeof(out));
    table->count = count;
    MemoryBarrier();
    table->sequence = table->sequence + 1; // even: stable
}
}
