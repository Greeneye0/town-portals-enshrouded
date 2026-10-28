#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace portal {

constexpr double kFixedScale = 4294967296.0;
constexpr double kInvFixedScale = 1.0 / 4294967296.0;

struct FixedPos {
    int64_t x{};
    int64_t y{};
    int64_t z{};
};

struct RemotePlayerPos {
    float wx{};
    float wy{};
    float wz{};
    char name[32]{};
};

inline FixedPos ToFixed(float wx, float wy, float wz) {
    return {
        static_cast<int64_t>(static_cast<double>(wx) * kFixedScale),
        static_cast<int64_t>(static_cast<double>(wy) * kFixedScale),
        static_cast<int64_t>(static_cast<double>(wz) * kFixedScale)
    };
}

inline void ToFloat(const FixedPos& p, float& wx, float& wy, float& wz) {
    wx = static_cast<float>(static_cast<double>(p.x) * kInvFixedScale);
    wy = static_cast<float>(static_cast<double>(p.y) * kInvFixedScale);
    wz = static_cast<float>(static_cast<double>(p.z) * kInvFixedScale);
}

// One 3D end of a portal (either out in the Field or beside the Base Flame Altar).
struct PortalEndpoint {
    FixedPos pos{};      // Ground center (x, y_ground, z) in fixed-point
    float wx{};          // Ground X in meters
    float wy{};          // Ground Y in meters
    float wz{};          // Ground Z in meters
    float nx{0.0f};      // Outward unit normal X (horizontal)
    float nz{1.0f};      // Outward unit normal Z (horizontal)
    float rx{1.0f};      // Right unit vector X (horizontal)
    float rz{0.0f};      // Right unit vector Z (horizontal)
    int32_t yawMillirad{}; // Packed horizontal normal angle in milliradians

    // Sampled 5x5 local terrain relative heights (in meters relative to wy) around this endpoint
    // spanning [-8m..+8m] in local Right/Normal coordinates for the 3D see-through window.
    bool hasTerrainGrid = false;
    float terrainRelY[5][5]{};

    void SetNormalFromYaw(int32_t mrad) {
        yawMillirad = mrad;
        float angle = static_cast<float>(mrad) * 0.001f;
        nx = std::sin(angle);
        nz = std::cos(angle);
        // Right vector R = U x N where U = (0, 1, 0) -> (nz, 0, -nx)
        rx = nz;
        rz = -nx;
    }

    // The view a portal at this endpoint shows of it (snapshots, portal_scene_probe): the base
    // end as it is, looking out toward the altar; the field end turned round, looking the way
    // its caster faced (its normal faces the caster).
    PortalEndpoint Turned() const {
        PortalEndpoint e = *this;
        e.nx = -nx; e.nz = -nz; e.rx = -rx; e.rz = -rz;
        return e;
    }

    void SetNormal(float dirX, float dirZ) {
        float len = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (len > 1e-4f) {
            nx = dirX / len;
            nz = dirZ / len;
        } else {
            nx = 0.0f;
            nz = 1.0f;
        }
        rx = nz;
        rz = -nx;
        yawMillirad = static_cast<int32_t>(std::atan2(nx, nz) * 1000.0f);
    }

    void SyncFloatFromFixed() {
        ToFloat(pos, wx, wy, wz);
    }

    void SyncFixedFromFloat() {
        pos = ToFixed(wx, wy, wz);
    }
};

// Complete state of a player's open Town Portal (up to 8 concurrent portals supported in multiplayer).
struct PortalInstance {
    bool active = false;
    bool closing = false;
    bool ownerVisitedBase = false;
    uint64_t portalId = 0;
    uint64_t ownerAccount = 0;
    uint32_t ownerPlayerId = 0;
    uint64_t openedTick = 0;
    uint64_t closedTick = 0;
    uint64_t lastSyncTick = 0;

    PortalEndpoint fieldEnd{};
    PortalEndpoint baseEnd{};

    // Exact 3D coordinates of the Flame Altar pedestal center so the see-through portal window
    // renders the 3D Flame Altar & roaring golden flame at its true relative 3D offset!
    FixedPos altarPos{};
    float altarX = 0.0f;
    float altarY = 0.0f;
    float altarZ = 0.0f;

    char ownerName[32]{"Player"};
};

constexpr size_t kMaxPortals = 8;
// A new portal forms while its caster channels; nobody can enter it before then.
constexpr uint64_t kPortalFormMs = 3000;

} // namespace portal
