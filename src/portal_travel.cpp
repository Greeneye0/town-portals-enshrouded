#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "portal_travel.h"
#include "portal_protocol.h"
#include "portal_travel_signatures.h"
#include "portal_rules.h"
#include "portal_names.h"
#include "portal_snapshot_share.h"
#include "portal_host_identity.h"
#include "portal_easter.h"
#include "portal_water.h"
#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <mutex>
#include <string_view>

namespace portal::travel {
namespace {

template <class T>
T& at(void* p, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + offset);
}

template <class T>
T field(const void* p, size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const uint8_t*>(p) + offset, sizeof(T));
    return value;
}

// Verified Enshrouded build 1269579 ECS & terrain signatures
constexpr char kPlayerPattern[] =
    "48 89 4C 24 08 55 53 48 8D AC 24 E8 F5 FF FF 48 81 EC 18 0B 00 00 41 B8 B0 05 00 00 "
    "48 8D 55 F0 48 8B D9 E8 ?? ?? ?? ?? 41 B8 B0 05 00 00 48 8D 55 F0 48 8B CB E8 ?? ?? ?? ?? "
    "84 C0 0F 84 ?? ?? ?? ??";
constexpr char kApplyPattern[] =
    "40 55 56 48 8D AC 24 A8 FD FF FF 48 81 EC 58 03 00 00 41 B8 B0 01 00 00 48 8D 55 90 "
    "48 8B F1 E8 ?? ?? ?? ?? 41 B8 B0 01 00 00 48 8D 55 90 48 8B CE E8 ?? ?? ?? ?? 84 C0 "
    "0F 84 ?? ?? ?? ?? 48 89 9C 24 70 03 00 00";
constexpr char kNextPattern[] =
    "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 ?? 08 00 00 75 ?? 32 C0 "
    "48 83 C4 20 5F C3 41 0F B6 80 B0 04 00 00 49 8D 88 70 04 00 00 48 89 5C 24 38 45 33 DB "
    "41 0F B7 98 BA 04 00 00";
constexpr char kEntityPattern[] =
    "8B 41 08 4C 8B C2 85 C0 75 ?? 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 "
    "?? 08 00 00 8B 04 D1 41 89 00 49 8B C0 C3 CC CC CC CC CC CC 8B 05 ?? ?? ?? ?? 48 85 C9 "
    "74 ?? 8B 41 10 C3 CC";
constexpr char kRayPattern[] =
    "48 8B C4 48 89 50 10 55 53 57 41 54 48 8D A8 58 FF FF FF 48 81 EC 88 01 00 00 41 0F 28 08 "
    "48 8B F9 F2 0F 10 05 ?? ?? ?? ?? 49 8B D9 0F 29 70 A8 4D 8B E0 41 0F 28 31 0F 29 78 98 "
    "0F 28 FE 44 0F 29 40 88";
constexpr char kDirtyPattern[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 30 "
    "48 8B B1 90 02 00 00 49 8B D9 0F B7 6C 24 70 4D 8B F8 4C 8B D2 4C 8B F1 44 8B A6 DC 04 00 00 "
    "48 85 D2";
constexpr char kPhysicsRayPattern[] =
    "40 53 48 81 EC B0 00 00 00 48 8B D9 48 8B 8A A0 01 00 00 48 85 C9 74 6C";

using NextFn = bool (*)(void*, void*, size_t);
using EntityFn = uint32_t* (*)(void*, uint32_t*);
using DirtyFn = void (*)(void*, uint64_t, uint64_t, uint64_t, uint16_t, uint8_t);
using TerrainRayFn = void* (*)(void*, void*, const float*, const float*, const uint8_t*);
using PhysicsRayFn = void* (*)(void*, void*, const float*, uint32_t, uint32_t, uint32_t, uint8_t, void*);
using ReceiveFn = void (*)(void*);
using GroundQueryFn = void* (*)(void*, void*, void*);
using QueueCommandFn = void* (*)(void*,void*,uint8_t,uint32_t,uint32_t);
using NotifyTeleportFn = bool (*)(void*,uint32_t,uint32_t);
using GetComponentFn = void** (*)(void*,void*,void*,uint32_t);
using AppendFn = uint8_t* (*)(uint8_t*, void*, uint8_t, uint64_t*, uint32_t, uint32_t, uint32_t, uint64_t, uint64_t*);
using GateFn = uint8_t* (*)(uint8_t*, char, uint64_t);

NextFn g_playerFirst = nullptr;
NextFn g_playerMore = nullptr;
NextFn g_applyFirst = nullptr;
NextFn g_applyMore = nullptr;
EntityFn g_entityOf = nullptr;
DirtyFn g_dirty = nullptr;
TerrainRayFn g_ray = nullptr;
PhysicsRayFn g_physicsRay = nullptr;
ReceiveFn g_receive = nullptr;
GroundQueryFn g_groundQuery = nullptr;
QueueCommandFn g_queueTeleport=nullptr;
NotifyTeleportFn g_notifyTeleport=nullptr;
GetComponentFn g_getComponent=nullptr;
AppendFn g_append = nullptr;
GateFn g_gate = nullptr;

std::atomic<void*> g_client{nullptr};
std::atomic<void*> g_server{nullptr};
std::atomic<uint64_t> g_localAccount{0};
std::atomic<uint32_t> g_localEntity{0};
std::atomic<uint32_t> g_observedHostEntity{0};
std::atomic<bool> g_hostEntityConfirmed{false};
std::atomic<bool> g_hostAnswered{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_faulted{false};
// The native travel action needs no new signature beyond the player system; the travel gate
// override is optional (without it the game's own area restrictions refuse the action, and
// the request falls back to the direct path).
std::atomic<bool> g_nativeTravel{false};

std::mutex g_lock;
HostIdentity g_hostIdentity; // under g_lock; published through the two atomics above

void PublishHostIdentity() {
    g_observedHostEntity.store(g_hostIdentity.Current());
    g_hostEntityConfirmed.store(g_hostIdentity.Confirmed());
}

enum class Stage { none, resolving, injecting, armed, applying, cancelling };

struct PendingTeleport {
    Stage stage = Stage::none;
    uint32_t entity = 0;
    uint64_t account = 0;
    uint64_t nonce = 0;
    uint64_t expires = 0;
    void* storage = nullptr;
    FixedPos target{};
    uint64_t queuedAt = 0;
    // Native: the game's own travel action (cast animation, teleport effect, loading) moves
    // the player. Otherwise the Teleport component is queued directly (the fallback).
    bool native = false;
    bool applied = false;      // the game's Teleport component reached ApplyTick
    uint64_t armedAt = 0;      // when the game accepted the native travel action
    uint64_t terrainSince = 0; // first ApplyTick that waited for destination terrain
    uint64_t logAfter = 0;
    int64_t groundY = 0;
    bool groundRead = false;
};
uint64_t g_appliedRequests=0;
uint64_t g_lastApplyLatency=0;
uint64_t g_nativeApplied=0;
uint64_t g_directApplied=0;

PendingTeleport g_requests[16]{};
BaseSpotMsg g_baseSuggestion{}; // under g_lock
PortalInstance g_portals[kMaxPortals]{};

struct EcsPlayerSlot {
    uint32_t entity = 0;
    uint64_t seenTick = 0;
    float wx = 0.0f;
    float wy = 0.0f;
    float wz = 0.0f;
};
EcsPlayerSlot g_ecsPlayers[16]{};
// Where each player stands, for HostIdentity: the Apply iterator above only sees a player with a
// teleport pending, so these are read from the player tick.
EcsPlayerSlot g_hostSpots[16]{};

struct WorldState {
    void* storage = nullptr;
    void* terrain = nullptr;
    void* physics = nullptr;
    uint64_t seen = 0;
    uint8_t realm = 0;
};

WorldState g_hostWorld{};
WorldState g_terrainSources[16]{};

struct Injection {
    void* input = nullptr;
    void* control = nullptr;
    void* command = nullptr;
    void* storage = nullptr;
    uint32_t entity = 0;
    uint32_t consumed = 0;
    uint8_t saved[12]{};
    uint8_t refusal = 0;
    uint8_t mode = 0;
};
thread_local Injection t_injection;

struct Application {
    void* position = nullptr;
    void* storage = nullptr;
    uint32_t entity = 0;
    uint64_t nonce = 0;
    FixedPos destination{};
};
thread_local Application t_application;

void Log(const char* s) {
    if (auto* h = greeneye::Host(); h && h->log) {
        h->log(s);
    }
}

void* Storage(void* ctx) {
    return at<void*>(at<void*>(ctx, 0), 0x30);
}

bool Authority(void* ctx) {
    return at<uint8_t>(at<void*>(ctx, 0), 0xa1) == 0;
}

uint32_t Id(void* ctx) {
    uint32_t result = 0;
    return *g_entityOf(ctx, &result);
}

void MarkDirty(void* wrapper) {
    if (auto* tracker = at<void*>(wrapper, 0x28)) {
        g_dirty(tracker,
                at<uint64_t>(wrapper, 8),
                at<uint64_t>(wrapper, 16),
                at<uint64_t>(wrapper, 24),
                at<uint16_t>(wrapper, 32),
                at<uint8_t>(wrapper, 34));
    }
}

uint32_t SenderEntity(uint64_t account) {
    auto* s = static_cast<uint8_t*>(g_server.load());
    if (!account) return 0;
    if (account == g_localAccount.load()) {
        if (g_hostEntityConfirmed.load()) return g_observedHostEntity.load();
        return g_localEntity.load();
    }
    for (int i = 0; s && i < 16; ++i) {
        auto* slot = s + 0x1e0 + i * 0x148;
        if (at<uint32_t>(slot, 0) && at<uint64_t>(slot, 0x48) == account) {
            return at<uint32_t>(slot, 0x18);
        }
    }
    return 0;
}

bool ValidateTerrainGrid(void* terrain, uint8_t realm, float& outOx, float& outOy, float& outOz, double& outWidth, double& outHeight) {
    if (!terrain || !g_ray) return false;
    auto count = field<uint64_t>(terrain, 0x1c00);
    auto* realms = field<uint8_t*>(terrain, 0x1bf8);
    if (!realms || !count || count > 256 || realm >= count) return false;
    auto* grid = field<uint8_t*>(realms + static_cast<size_t>(realm) * 0x3870, 0);
    if (!grid) return false;
    uint32_t shift = field<uint32_t>(grid, 0x10);
    uint32_t nx = field<uint32_t>(grid, 0x14);
    uint32_t ny = field<uint32_t>(grid, 0x18);
    float ox = field<float>(grid, 0x70);
    float oy = field<float>(grid, 0x74);
    float oz = field<float>(grid, 0x78);
    if (shift > 12 || !nx || !ny || nx > 65536 || ny > 4096 ||
        !std::isfinite(ox) || !std::isfinite(oy) || !std::isfinite(oz)) {
        return false;
    }
    double cellSize = static_cast<double>(uint64_t(1) << shift);
    outOx = ox;
    outOy = oy;
    outOz = oz;
    outWidth = nx * cellSize;
    outHeight = ny * cellSize;
    return outHeight <= 32768.0;
}

void* FindActiveTerrain(uint8_t& outRealm, void** outPhysics = nullptr) {
    uint64_t now = GetTickCount64();
    if (g_hostWorld.terrain && now - g_hostWorld.seen < 2500) {
        outRealm = g_hostWorld.realm;
        if (outPhysics) *outPhysics = g_hostWorld.physics;
        return g_hostWorld.terrain;
    }
    uint64_t bestSeen = 0;
    void* bestTerrain = nullptr;
    void* bestPhysics = nullptr;
    uint8_t bestRealm = 0;
    for (const auto& src : g_terrainSources) {
        if (src.terrain && now >= src.seen && now - src.seen < 2500 && src.seen >= bestSeen) {
            bestSeen = src.seen;
            bestTerrain = src.terrain;
            bestPhysics = src.physics;
            bestRealm = src.realm;
        }
    }
    outRealm = bestRealm;
    if (outPhysics) {
        *outPhysics = bestPhysics;
    }
    return bestTerrain;
}

// Cast a short vertical ray [wy + upRange .. wy - downRange] first so indoor/cave/under-roof bases
// snap to the floor at the Altar/portal level rather than the roof above.
bool RaycastLocalVertical(void* terrain, uint8_t realm, float wx, float wyRef, float wz,
                          float upRange, float downRange, float& outY) {
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    double width = 0.0, height = 0.0;
    if (!ValidateTerrainGrid(terrain, realm, ox, oy, oz, width, height)) return false;
    if (wx < ox || wz < oz || wx >= ox + width || wz >= oz + width) return false;

    float topY = std::min(static_cast<float>(oy + height + 0.25), wyRef + upRange);
    float botY = std::max(oy, wyRef - downRange);
    if (topY <= botY) return false;

    alignas(16) float from[4] = {wx, topY, wz, 0.0f};
    alignas(16) float to[4] = {wx, botY, wz, 0.0f};
    alignas(16) uint8_t hit[64]{};
    const uint8_t options[4] = {realm, 0, 0, 1};
    g_ray(hit, terrain, from, to, options);
    if (!hit[0x13]) return false;
    float y = field<float>(hit, 4);
    if (!std::isfinite(y) || y < botY - 0.5f || y > topY + 0.5f) return false;
    outY = y;
    return true;
}

bool QueueInternal(uint32_t entity, uint64_t account, uint64_t nonce, const FixedPos& target) {
    if (g_paused || g_faulted || !entity || entity > 16) return false;
    std::lock_guard guard(g_lock);
    uint64_t now = GetTickCount64();
    for (auto& p : g_requests) {
        if (p.stage != Stage::none && (p.entity == entity || (account != 0 && p.account == account))) {
            // A native action the game accepted owns its Teleport component until ApplyTick
            // finishes or cancels it; replacing it would let it run unmodified.
            if (p.native && p.stage != Stage::resolving) return false;
            // If an older request for the same player timed out > 2.5s ago, replace it cleanly
            if (now + 12500 >= p.expires) {
                p = {};
            } else {
                return false;
            }
        }
    }
    for (auto& p : g_requests) {
        if (p.stage == Stage::none) {
            p = {Stage::resolving, entity, account, nonce, now + 15000, nullptr, target, now};
            p.native = g_nativeTravel.load();
            return true;
        }
    }
    return false;
}

void ObserveWorld(void* ctx) {
    auto realm = at<uint8_t>(at<void*>(ctx, 0), 0xbb);
    auto* storage = Storage(ctx);
    void* terrain = storage ? at<void*>(storage, 0x478) : nullptr;
    if (terrain && (!at<void*>(terrain, 0x1bf8) || at<uint64_t>(terrain, 0x1c00) > 256 ||
                    realm >= at<uint64_t>(terrain, 0x1c00))) {
        terrain = nullptr;
    }
    void* physics = storage ? at<void*>(storage, 0x480) : nullptr;
    if (physics && !(at<void*>(physics, 0x1a0) || at<void*>(physics, 0x118))) {
        physics = nullptr;
    }
    uint64_t now = GetTickCount64();
    std::lock_guard guard(g_lock);
    if (terrain || physics) {
        bool updated = false;
        for (auto& saved : g_terrainSources) {
            if (saved.storage == storage && saved.realm == realm) {
                if (terrain) saved.terrain = terrain;
                if (physics) saved.physics = physics;
                saved.seen = now;
                updated = true;
                break;
            }
        }
        if (!updated && terrain) {
            for (auto& saved : g_terrainSources) {
                if (!saved.storage || now - saved.seen >= 2000) {
                    saved = {storage, terrain, physics, now, realm};
                    break;
                }
            }
        }
    }
    if (!Authority(ctx)) return;
    for (auto& p : g_requests) {
        if (p.stage != Stage::none && p.storage && p.storage != storage) {
            p = {};
        }
    }
    void* prevPhysics = physics ? physics : ((g_hostWorld.storage == storage) ? g_hostWorld.physics : nullptr);
    void* prevTerrain = (g_hostWorld.storage == storage && !terrain) ? g_hostWorld.terrain : terrain;
    g_hostWorld = {storage, prevTerrain, prevPhysics, now, realm};
}

void CaptureTerrain(void* descriptors) {
    auto* ctx = at<void*>(descriptors, 0);
    if (!ctx) return;
    void* terrain = at<void*>(descriptors, 0xf8);
    if (!terrain) return;
    uint8_t realm = at<uint8_t>(at<void*>(ctx, 0), 0xbb);
    if (!at<void*>(terrain, 0x1bf8) || at<uint64_t>(terrain, 0x1c00) > 256 ||
        realm >= at<uint64_t>(terrain, 0x1c00)) {
        return;
    }
    void* storage = Storage(ctx);
    void* physics = storage ? at<void*>(storage, 0x480) : nullptr;
    if (physics && !(at<void*>(physics, 0x1a0) || at<void*>(physics, 0x118))) {
        physics = nullptr;
    }
    WorldState source{storage, terrain, physics, GetTickCount64(), realm};
    std::lock_guard guard(g_lock);
    if (Authority(ctx) && g_hostWorld.storage == source.storage) {
        g_hostWorld.terrain = terrain;
        if (physics) g_hostWorld.physics = physics;
    }
    for (auto& saved : g_terrainSources) {
        if (saved.storage == source.storage && saved.realm == source.realm) {
            if (!source.physics) source.physics = saved.physics;
            saved = source;
            return;
        }
    }
    for (auto& saved : g_terrainSources) {
        if (!saved.storage || source.seen - saved.seen >= 2000) {
            saved = source;
            return;
        }
    }
}

void SafeCaptureTerrain(void* descriptors) {
    __try {
        CaptureTerrain(descriptors);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* GroundHook(void* result, void* material, void* descriptors) {
    greeneye::Lease lease;
    if (lease) SafeCaptureTerrain(descriptors);
    return g_groundQuery(result, material, descriptors);
}

// The native travel gate (call at player system +0x10b8) answers 1 = travel disabled in this
// area, 2 = falling, 4 = under water. Portal travel goes anyway, so the game's own action,
// animation included, runs as usual. Only the action this mod injected is affected.
uint8_t* GateHook(uint8_t* out, char mode, uint64_t flags) {
    greeneye::Lease lease;
    auto* result = g_gate(out, mode, flags);
    if (lease && t_injection.input && *result) {
        t_injection.refusal = *result;
        t_injection.mode = static_cast<uint8_t>(mode);
        *result = 0;
    }
    return result;
}

void FinishInjection() {
    auto saved = t_injection;
    t_injection = {};
    if (!saved.input) return;
    bool taken = at<uint32_t>(saved.control, 0x54) != saved.consumed;
    // Restore the real input and its consumed version before the next player is processed,
    // so the temporary action never replays an older network input next frame.
    std::memcpy(static_cast<uint8_t*>(saved.input) + 0x174, saved.saved, 12);
    at<uint32_t>(saved.control, 0x54) = saved.consumed;
    bool accepted = at<uint32_t>(saved.command, 8) == signatures::travelAccepted;
    uint64_t now = GetTickCount64();
    std::lock_guard guard(g_lock);
    for (auto& p : g_requests) {
        if (p.entity == saved.entity && p.storage == saved.storage && p.stage == Stage::injecting) {
            if (saved.refusal) {
                char line[160]{};
                std::snprintf(line, sizeof(line),
                              "Town Portal: game travel gate refused (%s%s%s; area mode %u); travelling anyway",
                              (saved.refusal & 1) ? "travel disabled here " : "",
                              (saved.refusal & 2) ? "falling " : "",
                              (saved.refusal & 4) ? "under water" : "",
                              static_cast<unsigned>(saved.mode));
                Log(line);
            }
            if (accepted) {
                p.stage = Stage::armed;
                p.armedAt = now;
                p.expires = now + 30000;
                Log("Town Portal: native travel action accepted; waiting for the cast to finish");
            } else {
                // Character busy (or the action was refused after it was taken): move the
                // player with the direct path instead of failing the portal.
                p.native = false;
                p.stage = Stage::resolving;
                Log(taken ? "Town Portal: native travel action rejected after it was taken; using direct position"
                          : "Town Portal: native travel action not taken (character busy); using direct position");
            }
        }
    }
}

enum class DestinationTerrain { ready, loading, unknown };

// A short vertical ray around the destination ([wy+2.8m .. wy-3.5m]) that stops at the first
// cell not streamed in yet (options byte 2); the native query queues that cell for loading.
// Hit +0x12 says every crossed cell was available, +0x13 that a surface was hit.
DestinationTerrain ProbeDestination(float wx, float wy, float wz, float& outY) {
    uint8_t realm = g_hostWorld.realm;
    void* terrain = FindActiveTerrain(realm);
    if (!terrain) return DestinationTerrain::loading;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    double width = 0.0, height = 0.0;
    if (!ValidateTerrainGrid(terrain, realm, ox, oy, oz, width, height)) return DestinationTerrain::unknown;
    if (wx < ox || wz < oz || wx >= ox + width || wz >= oz + width) return DestinationTerrain::unknown;
    float topY = std::min(static_cast<float>(oy + height + 0.25), wy + 2.8f);
    float botY = std::max(oy, wy - 3.5f);
    if (topY <= botY) return DestinationTerrain::unknown;
    alignas(16) float from[4] = {wx, topY, wz, 0.0f};
    alignas(16) float to[4] = {wx, botY, wz, 0.0f};
    alignas(16) uint8_t hit[64]{};
    const uint8_t options[4] = {realm, 0, 1, 1};
    g_ray(hit, terrain, from, to, options);
    if (!hit[0x12]) return DestinationTerrain::loading;
    if (hit[0x13]) {
        float y = field<float>(hit, 4);
        if (std::isfinite(y) && y >= botY - 0.5f && y <= topY + 0.5f) outY = y;
    }
    return DestinationTerrain::ready;
}

// Terrain cells get this long to stream in; after that the portal's own height is used.
constexpr uint64_t kTerrainWaitMs = 12000;
// A native cast takes about four seconds before the game applies its Teleport. Jumping or
// dodging out of it cancels it and no Teleport ever arrives: let the player go after seven.
constexpr uint64_t kCastTimeoutMs = 7000;

void PlayerTick(void* ctx, void* descriptors) {
    if (g_faulted) return;
    void* physics = at<void*>(descriptors, 0x3f0);
    if (physics && (at<void*>(physics, 0x1a0) || at<void*>(physics, 0x118))) {
        std::lock_guard guard(g_lock);
        if (Authority(ctx)) g_hostWorld.physics = physics;
        for (auto& source:g_terrainSources) {
            if (source.storage==Storage(ctx)) source.physics=physics;
        }
    }
    if (!Authority(ctx)) return;
    uint32_t id = Id(ctx);
    void* storage = Storage(ctx);
    uint64_t now = GetTickCount64();
    std::lock_guard guard(g_lock);

    // Hosting a world, every player's tick passes here, the friends' too. Taking whoever ticked
    // last moved the host's travel onto a friend's character and refused the host's portals
    // until a fast travel. Tick timing is only a first guess: see HostIdentity.
    static uint64_t tickedAt[17]{};
    if (g_hostWorld.storage && g_hostWorld.storage != storage) {
        for (auto& p : g_requests) p = {};
        g_hostWorld.terrain = nullptr;
        g_hostIdentity.Reset();
        PublishHostIdentity();
        for (auto& t : tickedAt) t = 0;
    }
    g_hostWorld.storage = storage;
    g_hostWorld.seen = now;
    bool alone = true;
    if (id != 0 && id <= 16) {
        tickedAt[id] = now;
        for (uint32_t other = 1; other <= 16; ++other)
            if (other != id && tickedAt[other] && now - tickedAt[other] < 1500) alone = false;
        g_hostIdentity.OnTick(id, alone, g_localEntity.load());
        if (g_getComponent && now - g_hostSpots[id - 1].seenTick >= 100) {
            uint16_t transformType = 133;
            void* transformRef[2]{};
            g_getComponent(transformRef, storage, &transformType, id);
            if (const auto* transform = static_cast<const uint8_t*>(transformRef[0])) {
                float norm = 0;
                for (int i = 0; i < 4; ++i) { float q = field<float>(transform, 0x18 + i * 4); norm += q * q; }
                const float px = static_cast<float>(static_cast<double>(field<int64_t>(transform, 0x00)) * kInvFixedScale);
                const float py = static_cast<float>(static_cast<double>(field<int64_t>(transform, 0x08)) * kInvFixedScale);
                const float pz = static_cast<float>(static_cast<double>(field<int64_t>(transform, 0x10)) * kInvFixedScale);
                if (std::isfinite(norm) && std::fabs(norm - 1) <= 0.05f && px > 100.0f && px < 12000.0f && pz > 100.0f && pz < 12000.0f)
                    g_hostSpots[id - 1] = {id, now, px, py, pz};
            }
        }
        PublishHostIdentity();
    }

    uint64_t localAcc = g_localAccount.load();
    for (auto& p : g_requests) {
        if (p.stage == Stage::none) continue;
        // Match either exact entity ID or local host player account when world reassigned entity ID (e.g. entity=2)
        if (p.entity != id) {
            if (p.account != 0 && p.account == localAcc && !g_server.load() && g_hostIdentity.MayBeOwn(id, alone)) {
                p.entity = id;
            } else {
                continue;
            }
        }
        if (p.storage && p.storage != storage) {
            p = {};
            continue;
        }
        if (p.native && (p.stage == Stage::armed || p.stage == Stage::cancelling) && !p.applied &&
            p.armedAt && now - p.armedAt >= kCastTimeoutMs) {
            Log("Town Portal: native travel cast interrupted before the teleport; request dropped");
            p = {};
            continue;
        }
        if (p.expires <= now) {
            if (p.stage == Stage::resolving || (p.stage==Stage::cancelling&&now>p.expires+500)) p = {};
            else p.stage = Stage::cancelling;
            continue;
        }
        if (p.stage != Stage::resolving) continue;
        p.storage = storage;
        if (p.native) {
            // Read/write accesses from the verified player-system descriptor layout (shared
            // with Embervale Teleport): network input, its consumed version, command result.
            void* input = at<void*>(descriptors, 0x80);
            void* control = at<void*>(descriptors, 0x2f8);
            void* command = at<void*>(descriptors, 0xe8);
            if (!input || !control || !command) {
                p.native = false;
                Log("Town Portal: player input unavailable for the native travel action; using direct position");
            } else if (at<uint32_t>(input, 0x17c) != at<uint32_t>(control, 0x54)) {
                continue; // respect a travel request the player already queued; retry next tick
            } else {
                t_injection = {input, control, command, storage, id, at<uint32_t>(control, 0x54)};
                std::memcpy(t_injection.saved, static_cast<uint8_t*>(input) + 0x174, 12);
                at<uint32_t>(input, 0x174) = id; // valid entity target; the destination is supplied in ApplyTick
                // TeleporterType::Normal selects the native spell effect, cast animation and
                // loading; None would start the actor animation without the teleport visuals.
                at<uint8_t>(input, 0x178) = 1;
                at<uint32_t>(input, 0x17c) = t_injection.consumed + 1;
                p.stage = Stage::injecting;
                Log("Town Portal: portal travel via native travel action");
                return;
            }
        }
        // Fallback. Native TeleportToTarget queues this same 0x50-byte Teleport component
        // (2707f0). Queue it at the ECS safe point, without an actor sequence.
        // ApplyTeleport still owns replication, velocity reset and streaming.
        void* queue=at<void*>(at<void*>(ctx,0),8);
        uint16_t transformType=133;
        void* transformRef[2]{};
        if(!queue||!g_queueTeleport||!g_notifyTeleport||!g_getComponent)continue;
        g_getComponent(transformRef,storage,&transformType,id);
        if(!transformRef[0])continue;
        const auto* transform=static_cast<const uint8_t*>(transformRef[0]);
        float norm=0;for(int i=0;i<4;++i){float q=field<float>(transform,0x18+i*4);norm+=q*q;}
        if(!std::isfinite(norm)||std::fabs(norm-1)>0.05f)continue;
        if(!g_notifyTeleport(queue,id,id))continue;
        auto* command=static_cast<uint8_t*>(g_queueTeleport(queue,static_cast<uint8_t*>(queue)+0x10,3,0xbc,id));
        if(!command)continue;
        at<uint32_t>(command,0xc)=0x23384d40;
        at<uint16_t>(command,0x4c)=0x50;
        std::memset(command+0x6c,0,0x50);
        std::memcpy(command+0x6c,transform,0x38);
        std::memcpy(command+0x6c,&p.target,sizeof(p.target));
        p.stage = Stage::armed;
        Log("Town Portal: portal travel via direct position (native travel action unavailable)");
        return;
    }
}

// A Teleport the game queued for a player by itself, not for a portal trip, logged with where
// it leads: players were sent back to their altar on jumping, long after a trip, with nothing
// in the log (aerospace, jamesleafe, 2026-10/11). The game queues these to correct a player
// who drifted from an object they are linked to (14044f9c0, from the use state at 140233630).
// Under g_lock. Small corrections (under 3 m) are left out.
void LogGameTeleport(void* descriptors, uint32_t id, void* storage) {
    void* component = at<void*>(static_cast<uint8_t*>(descriptors) + 0x100, 0);
    void* posComp = at<void*>(descriptors, 0x10);
    if (!component || !posComp || at<uint8_t>(component, 0x48)) return;
    for (const auto& p : g_requests)
        if (p.stage != Stage::none && p.entity == id && p.storage == storage) return;
    const double fx = field<int64_t>(posComp, 0x00) * kInvFixedScale, fy = field<int64_t>(posComp, 0x08) * kInvFixedScale,
                 fz = field<int64_t>(posComp, 0x10) * kInvFixedScale;
    const double tx = at<int64_t>(component, 0x00) * kInvFixedScale, ty = at<int64_t>(component, 0x08) * kInvFixedScale,
                 tz = at<int64_t>(component, 0x10) * kInvFixedScale;
    const uint32_t relative = at<uint32_t>(component, 0x38);
    const double distance = std::sqrt((tx - fx) * (tx - fx) + (ty - fy) * (ty - fy) + (tz - fz) * (tz - fz));
    if (!relative && distance < 3.0) return;
    static uint64_t lastLogged = 0;
    const uint64_t now = GetTickCount64();
    if (now - lastLogged < (relative ? 10000u : 500u)) return;
    lastLogged = now;
    char line[256]{};
    if (relative)
        std::snprintf(line, sizeof(line), "Town Portal: the game moves player %u (at %.1f, %.1f, %.1f) to entity %u, offset %.1f, %.1f, %.1f (ground snap %u)",
                      id, fx, fy, fz, relative, tx, ty, tz, at<uint8_t>(component, 0x49));
    else
        std::snprintf(line, sizeof(line), "Town Portal: the game moves player %u from %.1f, %.1f, %.1f to %.1f, %.1f, %.1f (%.0f m, ground snap %u); not a portal trip",
                      id, fx, fy, fz, tx, ty, tz, distance, at<uint8_t>(component, 0x49));
    Log(line);
}

bool ApplyTick(void* ctx, void* descriptors) {
    if (g_faulted) return true;
    uint32_t id = Id(ctx);
    // Capture live 3D coordinates of all player entities in the ECS world for the see-through portal window!
    if (id != 0 && id <= 16) {
        if (void* posComp = at<void*>(descriptors, 0x10)) {
            int64_t rx = field<int64_t>(posComp, 0x00);
            int64_t ry = field<int64_t>(posComp, 0x08);
            int64_t rz = field<int64_t>(posComp, 0x10);
            float px = static_cast<float>(static_cast<double>(rx) * kInvFixedScale);
            float py = static_cast<float>(static_cast<double>(ry) * kInvFixedScale);
            float pz = static_cast<float>(static_cast<double>(rz) * kInvFixedScale);
            if (px > 100.0f && px < 12000.0f && pz > 100.0f && pz < 12000.0f) {
                std::lock_guard guard(g_lock);
                g_ecsPlayers[id - 1] = {id, GetTickCount64(), px, py, pz};
            }
        }
    }
    if (!Authority(ctx)) return true;
    void* storage = Storage(ctx);
    std::lock_guard guard(g_lock);
    if (id != 0 && id <= 16) LogGameTeleport(descriptors, id, storage);

    for (auto& p : g_requests) {
        if ((p.stage != Stage::armed && p.stage != Stage::cancelling) ||
            p.entity != id || p.storage != storage) {
            continue;
        }
        void* wrapper = static_cast<uint8_t*>(descriptors) + 0x100;
        void* component = at<void*>(wrapper, 0);
        if (!component || at<uint8_t>(component, 0x48)) return true;
        p.applied = true;
        // The native action targets the player itself; any other target is not this request's.
        uint32_t relative = at<uint32_t>(component, 0x38);
        if (p.native && relative && relative != id) {
            Log("Town Portal: native Teleport targets another entity; request dropped");
            p = {};
            return true;
        }
        uint64_t now = GetTickCount64();
        if (p.stage == Stage::cancelling || p.expires <= now) {
            MarkDirty(wrapper);
            at<uint8_t>(component,0x48)=1;
            p = {};
            Log("Town Portal: travel request expired; movement cancelled");
            return true;
        }

        // Hold the Teleport (returning false defers this entity, as native spawn queries do)
        // until the destination's terrain cells are resident, read the same ground on two
        // consecutive ticks, then refine Y with it; the portal's own Y is kept when the short
        // ray finds no floor. After kTerrainWaitMs the portal's Y is used regardless.
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        ToFloat(p.target, wx, wy, wz);
        if (!p.terrainSince) p.terrainSince = now;
        float groundY = wy;
        auto terrain = wy > 1.0f ? ProbeDestination(wx, wy, wz, groundY) : DestinationTerrain::unknown;
        bool waitedOut = now - p.terrainSince >= kTerrainWaitMs;
        if (terrain == DestinationTerrain::loading && !waitedOut) {
            if (now >= p.logAfter) {
                Log("Town Portal: waiting for destination terrain to load");
                p.logAfter = now + 3000;
            }
            return false;
        }
        if (terrain == DestinationTerrain::ready) {
            int64_t y = static_cast<int64_t>(static_cast<double>(groundY) * kFixedScale);
            constexpr int64_t kSameGround = static_cast<int64_t>(0.05 * kFixedScale);
            if (!waitedOut && (!p.groundRead || std::llabs(p.groundY - y) > kSameGround)) {
                p.groundRead = true;
                p.groundY = y;
                return false; // confirm the ground on the next tick
            }
            // Up to the ground found, but never more than 25 cm below the height the traveller's
            // game sent: under water this ray found a surface 1 m under the floor and put the
            // player inside the ground beside their altar (2026-11-08).
            constexpr int64_t kMaxSink = static_cast<int64_t>(0.25 * kFixedScale);
            if (y < p.target.y - kMaxSink) {
                char line[160]{};
                std::snprintf(line, sizeof(line), "Town Portal: ground found at %.1f, below the destination's %.1f; keeping the destination's",
                              static_cast<double>(y) * kInvFixedScale, static_cast<double>(p.target.y) * kInvFixedScale);
                Log(line);
            } else {
                p.target.y = y;
            }
        } else if (terrain == DestinationTerrain::loading) {
            Log("Town Portal: destination terrain still loading; using the portal's height");
        }

        MarkDirty(wrapper);
        std::memcpy(component, &p.target, sizeof(p.target));
        at<uint32_t>(component, 0x38) = 0;
        at<uint8_t>(component, 0x48) = 0;
        at<uint8_t>(component, 0x49) = 0;
        t_application = {at<void*>(descriptors, 0x10), storage, id, p.nonce, p.target};
        p.stage = Stage::applying;
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: applying destination %.1f, %.1f, %.1f (%s, terrain waited %llu ms)",
                      wx, static_cast<double>(p.target.y) * kInvFixedScale, wz,
                      p.native ? "native travel action" : "direct position",
                      static_cast<unsigned long long>(now - p.terrainSince));
        Log(line);
        return true;
    }
    return true;
}

void FinishApplication() {
    auto done = t_application;
    t_application = {};
    if (!done.entity) return;
    std::lock_guard guard(g_lock);
    for (auto& p : g_requests) {
        if (p.stage == Stage::applying && p.entity == done.entity &&
            p.storage == done.storage && p.nonce == done.nonce) {
            ++g_appliedRequests;
            ++(p.native ? g_nativeApplied : g_directApplied);
            g_lastApplyLatency=GetTickCount64()-p.queuedAt;
            p = {};
        }
    }
}

void UpsertPortalLocked(const PortalInstance& instance) {
    for (auto& slot : g_portals) {
        if (slot.active && slot.ownerAccount == instance.ownerAccount) {
            bool samePortalId = (slot.portalId == instance.portalId);
            uint64_t origOpenedTick = slot.openedTick;
            PortalEndpoint oldField = slot.fieldEnd;
            PortalEndpoint oldBase = slot.baseEnd;
            slot = instance;
            if (samePortalId) {
                // Preserve openedTick so periodic multiplayer sync broadcasts never replay the opening animation!
                if (origOpenedTick != 0) {
                    slot.openedTick = origOpenedTick;
                }
                if (oldField.hasTerrainGrid && !slot.fieldEnd.hasTerrainGrid) {
                    slot.fieldEnd.hasTerrainGrid = true;
                    std::memcpy(slot.fieldEnd.terrainRelY, oldField.terrainRelY, sizeof(oldField.terrainRelY));
                }
                // Each end's move, from any source (the base end of a portal was seen high in the
                // air with no log of it, 2026-11-01).
                for (auto [before, after, which] : {std::tuple{&oldField, &slot.fieldEnd, "field"}, std::tuple{&oldBase, &slot.baseEnd, "base"}}) {
                    const float dx = after->wx - before->wx, dy = after->wy - before->wy, dz = after->wz - before->wz;
                    if (dx * dx + dy * dy + dz * dz > 1.0f) {
                        char line[192]{};
                        std::snprintf(line, sizeof(line), "Town Portal: portal %llu %s end moved %.1f m, from %.1f, %.1f, %.1f to %.1f, %.1f, %.1f",
                                      static_cast<unsigned long long>(slot.portalId), which, std::sqrt(dx * dx + dy * dy + dz * dz),
                                      before->wx, before->wy, before->wz, after->wx, after->wy, after->wz);
                        Log(line);
                    }
                }
                if (oldBase.hasTerrainGrid && !slot.baseEnd.hasTerrainGrid) {
                    slot.baseEnd.hasTerrainGrid = true;
                    std::memcpy(slot.baseEnd.terrainRelY, oldBase.terrainRelY, sizeof(oldBase.terrainRelY));
                }
            }
            return;
        }
    }
    for (auto& slot : g_portals) {
        if (!slot.active) {
            slot = instance;
            return;
        }
    }
    // Overwrite oldest portal if all 8 slots are full
    size_t oldestIdx = 0;
    uint64_t oldestTick = ~uint64_t(0);
    for (size_t i = 0; i < kMaxPortals; ++i) {
        if (g_portals[i].openedTick < oldestTick) {
            oldestTick = g_portals[i].openedTick;
            oldestIdx = i;
        }
    }
    g_portals[oldestIdx] = instance;
}

// The far side of a portal, for a player who is looking through one. The game's server sends a
// player the entities within 100 m (some kinds 200 m) of the player OR of the player's "hot
// point", a second place it keeps for each player (16 of 12 bytes at ecs +0x7ddc10: x and z
// in whole metres and a set flag; cleared each tick, FUN_140a23b90, and looked at by the
// relevance test FUN_140a23f40 and the scene cell loader FUN_140a2b3d0). Farm animals at the
// base were missing from a far portal's view because of that 100 m rule (2026-11-10). So a
// client asks the host ("/tpv1 <player id> <x> <z>", every 2 s while it shows a live view)
// and the host sets that player's hot point each tick, before the game lists its observers
// (ObserversHook), for 6 s after the last request. Only a place near an end of an open portal
// is taken. A hot point the game set itself that tick is left alone.
// Open portal ends for the server's worker threads (the despawn check runs on several at once;
// a try_lock on g_lock skipped the portal ends on every thread but one, and enemies 10 m from a
// portal were despawned, 2026-11-10). Written once a tick by ObserversHook into the spare of two
// buffers, then published by index; readers copy without a lock.
struct EndList {uint32_t count;float xyz[32][3];};
EndList g_endLists[2]{};
std::atomic<uint32_t> g_endCurrent{0};
std::atomic<uint64_t> g_endTick{0};
void PublishEnds() {
    if(!g_lock.try_lock())return; // keep the last list
    std::lock_guard guard(g_lock,std::adopt_lock);
    EndList& next=g_endLists[g_endCurrent.load()^1u];
    next.count=0;
    for(const auto& slot:g_portals){
        if(!slot.active||slot.closing)continue;
        for(const auto* end:{&slot.fieldEnd,&slot.baseEnd}){
            if((!end->pos.x&&!end->pos.z)||next.count>=32)continue;
            next.xyz[next.count][0]=end->wx;next.xyz[next.count][1]=end->wy;next.xyz[next.count][2]=end->wz;++next.count;
        }
    }
    g_endCurrent.store(g_endCurrent.load()^1u);
    g_endTick=GetTickCount64();
}
EndList CurrentEnds() {
    EndList copy=g_endLists[g_endCurrent.load()];
    if(GetTickCount64()-g_endTick.load()>5000)copy.count=0; // stale (no server tick)
    if(copy.count>32)copy.count=0;
    return copy;
}
struct HotPoint {std::atomic<int32_t> x{},z{};std::atomic<uint64_t> until{};};
HotPoint g_hot[16];
using ObserversFn=void*(*)(void* block,uint8_t* ecs,uint8_t simd);
ObserversFn g_observers{};
void* ObserversHook(void* block,uint8_t* ecs,uint8_t simd) {
    greeneye::Lease lease;
    if(lease&&ecs&&ecs[8]==0){ // the server's entities
        PublishEnds();
        const uint64_t now=GetTickCount64();
        // Without a request, a player's hot point stays on the field end of their own open
        // portal: the area round it then stays loaded and its creatures alive while they are
        // away. Only while they looked through the portal was too late: an enemy by the far
        // end was gone in the few seconds between stepping home and looking back (2026-11-10).
        int32_t own[16][2]{};bool hasOwn[16]{};
        if(g_lock.try_lock()){
            std::lock_guard guard(g_lock,std::adopt_lock);
            for(const auto& slot:g_portals){
                if(!slot.active||slot.closing||slot.ownerPlayerId<1||slot.ownerPlayerId>16)continue;
                if(!slot.fieldEnd.pos.x&&!slot.fieldEnd.pos.z)continue;
                const int i=int(slot.ownerPlayerId)-1;
                own[i][0]=int32_t(std::floor(slot.fieldEnd.wx));own[i][1]=int32_t(std::floor(slot.fieldEnd.wz));hasOwn[i]=true;
            }
        }
        for(int i=0;i<16;++i){
            const bool asked=g_hot[i].until.load()>now;
            if(!asked&&!hasOwn[i])continue;
            uint8_t* point=ecs+0x7ddc10+size_t(i)*0xc;
            if(point[8])continue;
            if(!asked){
                static int32_t logged[16][2]{};
                if(logged[i][0]!=own[i][0]||logged[i][1]!=own[i][1]){
                    logged[i][0]=own[i][0];logged[i][1]=own[i][1];
                    char line[120]{};
                    std::snprintf(line,sizeof(line),"Town Portal: player %d's portal end at %d, %d stays loaded",i+1,own[i][0],own[i][1]);
                    Log(line);
                }
            }
            *reinterpret_cast<int32_t*>(point)=asked?g_hot[i].x.load():own[i][0];
            *reinterpret_cast<int32_t*>(point+4)=asked?g_hot[i].z.load():own[i][1];
            point[8]=1;
        }
    }
    return g_observers(block,ecs,simd);
}
// Enemies by a portal's ends. The game's conditional_despawn (FUN_140332610) destroys a spawned
// creature once no player stands within its despawn range, every 1-2 s; the positions it
// compares come from a gatherer (FUN_1409df5f0 via FUN_1409df210) that lists each player's own
// position only. So the scavenger standing by a portal's far end was gone by the time its
// owner looked back through the portal (2026-11-10). Here, only for that caller (its return
// address), each end of every open portal is added to that list as if a player stood there.
// The list: {data, count, capacity (16 inline), grow callback}; 0x40-byte entries, entity id at
// +0, x, y, z as 32.32 fixed point at +8, +0x10, +0x18, then rotation and scale. The despawn
// test reads only x, y, z. Other callers (discovery of places, ...) are left alone.
using GatherFn=uint64_t(*)(int64_t* list,void* query);
GatherFn g_gather{};
const uint8_t* g_despawnReturn{};
std::atomic<uint32_t> g_keptAlive{};
std::atomic<int> g_transformType{-1};
// CurrentTransform's type number this session (the gatherer reads player positions with it).
void NoteTransformType(void* query) {
    __try{ if(query)g_transformType=*reinterpret_cast<uint16_t*>(*reinterpret_cast<uint8_t**>(query)+300); }__except(EXCEPTION_EXECUTE_HANDLER){}
}
uint64_t GatherHook(int64_t* list,void* query) {
    greeneye::Lease lease;
    const void* from=_ReturnAddress();
    NoteTransformType(query);
    const uint64_t result=g_gather(list,query);
    if(!lease||!list||from!=g_despawnReturn)return result;
    const EndList ends=CurrentEnds();
    uint32_t added=0;
    for(uint32_t k=0;k<ends.count;++k){
        if(list[1]>=list[2]||!list[0])break;
        auto* entry=reinterpret_cast<uint8_t*>(list[0])+list[1]*0x40;
        std::memset(entry,0,0x40);
        const auto fixed=[](float v){return int64_t(double(v)*4294967296.0);};
        *reinterpret_cast<int64_t*>(entry+0x08)=fixed(ends.xyz[k][0]);
        *reinterpret_cast<int64_t*>(entry+0x10)=fixed(ends.xyz[k][1]+1.0f);
        *reinterpret_cast<int64_t*>(entry+0x18)=fixed(ends.xyz[k][2]);
        for(int q=0;q<4;++q)*reinterpret_cast<float*>(entry+0x2c+4*q)=1.0f;
        ++list[1];++added;
    }
    if(added&&!g_keptAlive.exchange(added)){
        char line[120]{};
        std::snprintf(line,sizeof(line),"Town Portal: creatures near %u open portal end(s) are kept as if a player stood there",added);
        Log(line);
    }
    if(!added)g_keptAlive=0;
    return result;
}

// Dev: every server entity destroyed within 150 m of an open portal end, with the way it went
// (mode 0 queued command, 1 the tick's deferred list, 2 scene cell unload), its id (bit 31: a
// scene entity) and where it stood: to see what removes enemies by a portal (2026-11-10).
// FUN_1409a2a10(ecs, id, mode, flag); the entity still exists on entry. Read only.
using DestroyFn=void(*)(uint8_t* ecs,uint32_t id,uint32_t mode,uint32_t flag);
DestroyFn g_destroy{};
using IndexFn=uint64_t(*)(void* table,const uint32_t* id);
IndexFn g_entityIndex{};
bool EntityPosition(uint8_t* ecs,uint32_t id,double out[3]) {
    const int t=g_transformType.load();
    if(t<0||t>=1024||!g_entityIndex)return false;
    __try{
        const uint64_t index=g_entityIndex(ecs+0x148,&id);
        if(index>=*reinterpret_cast<uint64_t*>(ecs+0x158))return false;
        auto* record=reinterpret_cast<uint8_t**>(*reinterpret_cast<uint8_t**>(ecs+0x188))[index];
        if(!record)return false;
        auto* arch=*reinterpret_cast<uint8_t**>(record+0x18);
        auto* storage=*reinterpret_cast<uint8_t**>(record+0x20);
        const uint32_t row=*reinterpret_cast<uint32_t*>(record+0x30);
        if(!arch||!storage)return false;
        if(!((reinterpret_cast<uint64_t*>(arch)[t>>6]>>(t&63))&1))return false;
        auto* c=storage+size_t(row)*(*reinterpret_cast<uint16_t*>(arch+0xa84+2*t))+*reinterpret_cast<uint16_t*>(arch+0x84+2*t);
        for(int k=0;k<3;++k)out[k]=double(reinterpret_cast<int64_t*>(c)[k])/4294967296.0;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
// Who queued each destroy (FUN_1409602d0(commands, owner, target), the only queuer of command 1):
// a small ring of {target, caller offset}, read back when the destroy runs.
using QueueDestroyFn=void*(*)(void* commands,uint32_t owner,uint32_t target);
QueueDestroyFn g_queueDestroy{};
struct QueuedBy {std::atomic<uint32_t> target{};std::atomic<uint32_t> caller{};};
QueuedBy g_queuedBy[256];
std::atomic<uint32_t> g_queuedNext{};
void* QueueDestroyHook(void* commands,uint32_t owner,uint32_t target) {
    greeneye::Lease lease;
    if(lease){
        const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        auto& slot=g_queuedBy[g_queuedNext.fetch_add(1)&255];
        slot.caller=uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress())-base);
        slot.target=target;
    }
    return g_queueDestroy(commands,owner,target);
}
uint32_t QueuedCaller(uint32_t id) {
    for(auto& slot:g_queuedBy)if(slot.target.load()==id)return slot.caller.load();
    return 0;
}

void DestroyHook(uint8_t* ecs,uint32_t id,uint32_t mode,uint32_t flag) {
    greeneye::Lease lease;
    if(lease&&ecs&&ecs[8]==0){
        double p[3];
        if(EntityPosition(ecs,id,p)&&g_lock.try_lock()){
            float best=1e30f;
            {
                std::lock_guard guard(g_lock,std::adopt_lock);
                for(const auto& slot:g_portals){
                    if(!slot.active)continue;
                    for(const auto* end:{&slot.fieldEnd,&slot.baseEnd}){
                        const float dx=end->wx-float(p[0]),dz=end->wz-float(p[2]);
                        best=std::min(best,dx*dx+dz*dz);
                    }
                }
            }
            static uint64_t windowStart=0;static int inWindow=0;
            const uint64_t now=GetTickCount64();
            if(now-windowStart>5000){windowStart=now;inWindow=0;}
            if(best<80.f*80.f||(best<150.f*150.f&&++inWindow<=25)){ // within 80 m: always
                const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                char line[200]{};
                std::snprintf(line,sizeof(line),"Town Portal (dev): destroyed %08x (%s) mode %u at %.1f, %.1f, %.1f, %.0f m from a portal end, from exe+%llx, queued by exe+%x",
                              id,(id>>31)?"scene":"spawned",mode,p[0],p[1],p[2],std::sqrt(best),
                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress())-base),QueuedCaller(id));
                Log(line);
            }
        }
    }
    g_destroy(ecs,id,mode,flag);
}

// Areas by a portal's ends stay loaded. The server keeps 256 m scene cells loaded near players and
// hot points (FUN_140a2b3d0; unload when every corner is over 258 m from every point). On the
// tick a player teleports, the game's teleport system takes that player's hot point for the
// destination, so for one tick the far end counts for nothing and a cell next to it unloads at
// once, with the spawners in it and so the enemies they made (2026-11-10: a camp 40-80 m from the
// portal, across a cell border). The unload request FUN_140995310(ecs, cell) is refused here for
// a cell within 120 m of an open portal end; the cell stays loaded and wanted, and the loader
// asks again each tick (so it unloads as usual once the portal closes). Server entities only.
using UnloadFn=uint64_t(*)(uint8_t* ecs,uint8_t* cell);
UnloadFn g_unload{};
bool CellRect(uint8_t* ecs,uint8_t* cell,float& x0,float& z0) {
    __try{
        auto* grid=*reinterpret_cast<uint8_t**>(ecs+0x7bc028);
        if(!grid)return false;
        const uint32_t cols=*reinterpret_cast<uint32_t*>(grid+0x10),index=*reinterpret_cast<uint32_t*>(cell+0x30);
        if(!cols||cols>4096)return false;
        x0=float(index%cols)*256.f;z0=float(index/cols)*256.f;
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
uint64_t UnloadHook(uint8_t* ecs,uint8_t* cell) {
    greeneye::Lease lease;
    float x0=0,z0=0;
    if(lease&&ecs&&cell&&ecs[8]==0&&CellRect(ecs,cell,x0,z0)){
        bool keep=false;float px=0,pz=0;
        const EndList ends=CurrentEnds();
        for(uint32_t k=0;k<ends.count&&!keep;++k){
            const float ex=ends.xyz[k][0],ez=ends.xyz[k][2];
            const float dx=std::max({x0-ex,0.f,ex-(x0+256.f)});
            const float dz=std::max({z0-ez,0.f,ez-(z0+256.f)});
            if(dx*dx+dz*dz<120.f*120.f){keep=true;px=ex;pz=ez;}
        }
        if(keep){
            static float lastX=-1,lastZ=-1;
            if(lastX!=x0||lastZ!=z0){
                lastX=x0;lastZ=z0;
                char line[160]{};
                std::snprintf(line,sizeof(line),"Town Portal: the area %.0f, %.0f stays loaded for the portal end at %.0f, %.0f",x0,z0,px,pz);
                Log(line);
            }
            return 0;
        }
    }
    return g_unload(ecs,cell);
}

void SetFarView(uint32_t playerId,int x,int z) {
    if(playerId<1||playerId>16||!g_observers)return;
    bool close=false;
    {
        std::lock_guard guard(g_lock);
        for(const auto& slot:g_portals){
            if(!slot.active)continue;
            for(const auto* end:{&slot.fieldEnd,&slot.baseEnd}){
                const float dx=end->wx-float(x),dz=end->wz-float(z);
                if(dx*dx+dz*dz<60.0f*60.0f)close=true;
            }
        }
    }
    if(!close)return;
    auto& hot=g_hot[playerId-1];
    const uint64_t now=GetTickCount64();
    if(hot.until.load()<=now){
        char line[120]{};
        std::snprintf(line,sizeof(line),"Town Portal: player %u looks through a portal; entities round %d, %d are sent to them too",playerId,x,z);
        Log(line);
    }
    hot.x.store(x);hot.z.store(z);hot.until.store(now+6000);
}

// "/tpw1 ..." for the open portal with an end nearest (x, z), within 60 m; 0 when none or no
// level is known (this process does not host).
int FarWaterAnswer(int x, int z, char* out, size_t capacity) {
    uint64_t best = 0; float bestDistance = 60.f * 60.f;
    {
        std::lock_guard guard(g_lock);
        for (const auto& slot : g_portals) {
            if (!slot.active) continue;
            for (const auto* end : {&slot.fieldEnd, &slot.baseEnd}) {
                const float dx = end->wx - float(x), dz = end->wz - float(z), d = dx * dx + dz * dz;
                if (d < bestDistance) { bestDistance = d; best = slot.portalId; }
            }
        }
    }
    float field = -1, base = -1;
    if (!best || !portal::water::Levels(best, field, base)) return 0;
    auto cm = [](float m) { return m < 0 ? -100 : int(m * 100.f + 0.5f); };
    return std::snprintf(out, capacity, "/tpw1 %llu %d %d", static_cast<unsigned long long>(best), cm(field), cm(base));
}

// 0: ordinary chat, 1: replace on host, 2: consume on client/host.
int HandleProtocolChat(void* store, uint8_t type, uint64_t* text, uint32_t senderHandle,
                       uint64_t account, char* answer, size_t capacity) {
    (void)capacity;
    if (type != kChatType || !text || !text[0] || text[1] > 240) return 0;
    std::string_view line(reinterpret_cast<const char*>(text[0]), static_cast<size_t>(text[1]));
    if (!line.starts_with("/tp")) return 0;

    auto* cl = g_client.load();
    const bool isClientStore = (cl && store == at<void*>(cl, 0xf8));
    // Native chat supplies the account HASH. Discovery v2 carries a SteamID64
    // and verifies its native hash before using Steam's authenticated transport.
    // Older companions relay these hidden lines; image data stays client-only.
    if(line.starts_with("/tpi1 ")||line.starts_with("/tpi2 ")){
        if(isClientStore)sharing::Control(account,line);
        return isClientStore?2:0;
    }

    if (line.starts_with("/tpv1 ")) {
        if (!isClientStore) {
            char text[64]{};
            std::memcpy(text, line.data() + 6, std::min<size_t>(line.size() - 6, sizeof(text) - 1));
            unsigned id = 0; int x = 0, z = 0;
            if (std::sscanf(text, "%u %d %d", &id, &x, &z) == 3) {
                SetFarView(id, x, z);
                // The answer, relayed to everyone: how high the water stands at that portal's ends.
                if (int n = FarWaterAnswer(x, z, answer, capacity); n > 0 && size_t(n) < capacity) return 1;
            }
        }
        return 2;
    }
    // "/tpw1 <portalId> <field cm> <base cm>": water at a portal's ends (-100: dry), from the host.
    if (line.starts_with("/tpw1 ")) {
        if (isClientStore && line.size() < 80) {
            char text[80]{};
            std::memcpy(text, line.data() + 6, line.size() - 6);
            unsigned long long id = 0; int field = 0, base = 0;
            if (std::sscanf(text, "%llu %d %d", &id, &field, &base) == 3)
                portal::water::SetHeardLevels(id, field < 0 ? -1.f : field * 0.01f, base < 0 ? -1.f : base * 0.01f);
        }
        return 2;
    }

    if (line == kHelloReply) {
        if (isClientStore) {
            auto ownHandle = at<uint32_t>(cl, 0x466b44);
            if (ownHandle && senderHandle == ownHandle && account) {
                g_hostAnswered.store(true);
            }
        }
        return 2;
    }

    // Server rules: a client asks (/tph2), the host answers with its own (/tps2 <bits>).
    if (line == rules::kAsk) {
        if (!isClientStore) {
            int n = rules::FormatAnswer(answer, capacity, rules::Local());
            return n > 0 && size_t(n) < capacity ? 1 : 2;
        }
        return 2;
    }
    if (rules::Rules hostRules{}; rules::ParseAnswer(line, hostRules)) {
        if (isClientStore) {
            auto ownHandle = at<uint32_t>(cl, 0x466b44);
            if (ownHandle && senderHandle == ownHandle) rules::SetHost(hostRules);
        }
        return 2;
    }

    if (line == kHello) {
        if (!isClientStore) {
            std::memcpy(answer, kHelloReply.data(), kHelloReply.size());
            answer[kHelloReply.size()] = 0;
            return 1;
        }
        return 2;
    }

    // Portal Open / Sync broadcast: "/tpo2 ..." (or "/tpo1 ..." from older clients)
    PortalSyncMsg syncMsg{};
    if (ParsePortalSync(line, syncMsg)) {
        uint64_t effAccount = syncMsg.ownerAccount ? syncMsg.ownerAccount : account;
        uint64_t localAcc = g_localAccount.load();
        // If this is our own portal echoed back to our client chat store, keep our exact local instance untouched
        if (isClientStore && localAcc != 0 && effAccount == localAcc) {
            return 2;
        }
        PortalInstance inst{};
        inst.active = true;
        inst.closing = false;
        inst.ownerVisitedBase = (syncMsg.visitedBase != 0);
        inst.portalId = syncMsg.portalId;
        inst.ownerAccount = effAccount;
        inst.ownerPlayerId = syncMsg.ownerPlayerId;
        inst.openedTick = GetTickCount64();
        inst.lastSyncTick = inst.openedTick;
        inst.fieldEnd.pos = syncMsg.fieldPos;
        inst.fieldEnd.SyncFloatFromFixed();
        inst.fieldEnd.SetNormalFromYaw(syncMsg.fieldYaw);
        inst.baseEnd.pos = syncMsg.basePos;
        inst.baseEnd.SyncFloatFromFixed();
        inst.baseEnd.SetNormalFromYaw(syncMsg.baseYaw);
        inst.altarPos = syncMsg.altarPos;
        ToFloat(inst.altarPos, inst.altarX, inst.altarY, inst.altarZ);
        // The host enforces its rules: a portal opened where they forbid it is neither kept
        // nor relayed (clients refuse to cast there; this covers old or altered ones).
        if (!isClientStore) {
            const auto hostRules = rules::Local();
            const auto& f = inst.fieldEnd;
            if (!hostRules.allowInHollowHalls && names::InHollowHalls(f.wx, f.wy + 0.5f, f.wz)) {
                Log("Town Portal: refused a portal opened in the Hollow Halls (town-portal.ini)");
                return 2;
            }
        }
        {
            std::lock_guard guard(g_lock);
            // The base end in the same place: keep the height this game refined from its own
            // terrain. The owner, far from the base, sends a guessed height every 12 s, and the
            // portal blinked into the ground and back out (2026-11-02).
            for (const auto& slot : g_portals) {
                if (!slot.active || slot.portalId != inst.portalId) continue;
                const float dx = slot.baseEnd.wx - inst.baseEnd.wx, dz = slot.baseEnd.wz - inst.baseEnd.wz;
                if (dx * dx + dz * dz < 0.3f * 0.3f && std::fabs(slot.baseEnd.wy - inst.baseEnd.wy) < 3.0f) {
                    inst.baseEnd.pos.y = slot.baseEnd.pos.y;
                    inst.baseEnd.wy = slot.baseEnd.wy;
                }
                break;
            }
            UpsertPortalLocked(inst);
        }
        // On the host store (not client store), let the server relay the broadcast to all connected clients,
        // and consume it on the client store so it never prints in the chat UI.
        return isClientStore ? 2 : 0;
    }

    // A clearer base spot for a portal: "/tpb1 ...", for its owner.
    BaseSpotMsg spotMsg{};
    if (ParseBaseSpot(line, spotMsg)) {
        if (isClientStore) {
            if (spotMsg.ownerAccount == g_localAccount.load()) {
                std::lock_guard guard(g_lock);
                g_baseSuggestion = spotMsg;
            }
            return 2;
        }
        return 0; // the host relays it to the owner
    }

    // A player's preferred portal colour: "/tpk1 ..."; the host relays it.
    uint32_t colourPlayer = 0;
    int colourSlot = -1;
    if (ParseColour(line, colourPlayer, colourSlot)) {
        if (isClientStore) colors::SetPreference(colourPlayer, colourSlot);
        return isClientStore ? 2 : 0;
    }

    // The easter egg: "/tpx1 ...", played by everyone who sees the portal; the host relays it.
    EasterEggMsg eggMsg{};
    if (ParseEasterEgg(line, eggMsg)) {
        if (isClientStore && !rules::NoFun()) easter::Start(eggMsg.portalId, int(eggMsg.place), GetTickCount64());
        return isClientStore ? 2 : 0;
    }

    // Portal Close broadcast: "/tpc1 ..."
    PortalCloseMsg closeMsg{};
    if (ParsePortalClose(line, closeMsg)) {
        uint64_t effAccount = closeMsg.ownerAccount ? closeMsg.ownerAccount : account;
        {
            std::lock_guard guard(g_lock);
            for (auto& slot : g_portals) {
                if (slot.active && slot.ownerAccount == effAccount &&
                    (!closeMsg.portalId || slot.portalId == closeMsg.portalId)) {
                    slot.closing = true;
                    slot.closedTick = GetTickCount64();
                    slot.active = false;
                }
            }
        }
        return isClientStore ? 2 : 0;
    }

    // Remote player teleport request: "/tpg1 <nonce> <x> <y> <z>"
    TeleportRequestMsg tpReq{};
    if (ParseTeleportRequest(line, tpReq)) {
        if (!isClientStore) {
            uint32_t entity = SenderEntity(account);
            if (entity) {
                QueueInternal(entity, account, tpReq.nonce, tpReq.target);
            }
        }
        return 2;
    }

    return 0;
}

int SafeChat(void* s, uint8_t t, uint64_t* text, uint32_t handle, uint64_t a, char* answer, size_t capacity) {
    __try {
        return HandleProtocolChat(s, t, text, handle, a, answer, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint8_t* AppendHook(uint8_t* result, void* store, uint8_t type, uint64_t* text,
                    uint32_t a, uint32_t b, uint32_t c, uint64_t account, uint64_t* time) {
    greeneye::Lease lease;
    char answer[160]{};
    int action = lease ? SafeChat(store, type, text, c, account, answer, sizeof(answer)) : 0;
    if (action == 2) {
        std::memset(result, 0, 8);
        return result;
    }
    if (action == 1) {
        uint64_t replacement[] = {reinterpret_cast<uint64_t>(answer), std::strlen(answer)};
        return g_append(result, store, type, replacement, a, b, c, account, time);
    }
    return g_append(result, store, type, text, a, b, c, account, time);
}

void SafeObserve(void* c) {
    __try { ObserveWorld(c); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; }
}
void SafeFinishInjection() {
    __try { FinishInjection(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; }
}
void SafePlayer(void* c, void* d) {
    __try { PlayerTick(c, d); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; }
}
bool SafeApply(void* c, void* d) {
    __try { return ApplyTick(c, d); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; return true; }
}
void SafeFinishApplication() {
    __try { FinishApplication(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_faulted = true; }
}

bool PlayerFirst(void* c, void* d, size_t n) {
    greeneye::Lease lease;
    if (lease && !g_faulted) SafeObserve(c);
    bool ok = g_playerFirst(c, d, n);
    if (lease && ok) SafePlayer(c, d);
    return ok;
}

bool PlayerMore(void* c, void* d, size_t n) {
    greeneye::Lease lease;
    if (lease) SafeFinishInjection();
    bool ok = g_playerMore(c, d, n);
    if (lease && ok) SafePlayer(c, d);
    return ok;
}

bool ApplyFirst(void* c, void* d, size_t n) {
    greeneye::Lease lease;
    if (lease && !g_faulted) SafeObserve(c);
    bool ok = g_applyFirst(c, d, n);
    while (lease && ok && !SafeApply(c, d)) ok = g_applyFirst(c, d, n);
    return ok;
}

bool ApplyMore(void* c, void* d, size_t n) {
    greeneye::Lease lease;
    if (lease) SafeFinishApplication();
    bool ok = g_applyMore(c, d, n);
    while (lease && ok && !SafeApply(c, d)) ok = g_applyMore(c, d, n);
    return ok;
}

void ReceiveHook(void* s) {
    greeneye::Lease lease;
    if (lease) g_server = s;
    g_receive(s);
}

bool CallMatches(const GreeneyeHost* host, uint8_t* site, uint8_t* expected) {
    uint8_t bytes[5]{};
    host->read(site, bytes, 5);
    int32_t rel = 0;
    std::memcpy(&rel, bytes + 1, 4);
    return bytes[0] == 0xe8 && site + 5 + rel == expected;
}

} // namespace

uint64_t LocalSteamAccount() {
    auto steam = GetModuleHandleW(L"steam_api64.dll");
    if (steam) {
        using UserFn = void* (*)();
        using IdFn = uint64_t (*)(void*);
        auto user = reinterpret_cast<UserFn>(GetProcAddress(steam, "SteamAPI_SteamUser_v023"));
        auto id = reinterpret_cast<IdFn>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID"));
        if (user && id) {
            if (void* instance = user()) {
                uint64_t sid = id(instance);
                if (sid != 0) return sid;
            }
        }
    }
    return 0x7072746c00000001ull;
}

// Open portals carried over a hot reload (the loader keeps this block): saved when the old
// generation is told to stop, taken back by the next one within ten seconds.
struct KeptPortals {
    uint64_t savedTick;
    uint32_t size;
    PortalInstance portals[kMaxPortals];
};
KeptPortals* KeptPortalBlock() {
    auto* host = greeneye::Host();
    return host ? static_cast<KeptPortals*>(host->keep("town-portal-open-portals-v1", sizeof(KeptPortals))) : nullptr;
}

bool Init(const GreeneyeHost* host) {
    auto* p = host->find(kPlayerPattern, nullptr);
    auto* a = host->find(kApplyPattern, nullptr);
    auto* n = host->find(kNextPattern, nullptr);
    g_entityOf = reinterpret_cast<EntityFn>(host->find(kEntityPattern, nullptr));
    g_dirty = reinterpret_cast<DirtyFn>(host->find(kDirtyPattern, nullptr));
    g_ray = reinterpret_cast<TerrainRayFn>(host->find(kRayPattern, nullptr));
    g_physicsRay = reinterpret_cast<PhysicsRayFn>(host->find(kPhysicsRayPattern, nullptr));
    g_queueTeleport=reinterpret_cast<QueueCommandFn>(host->find(
        "48 89 74 24 10 57 48 83 EC 20 44 8B 52 10 48 8B FA 41 8B C2 83 E0 0F 74 ?? 44 2B D0 41 83 C2 10 43 8D 34 0A 8B C6 48 3B 42 08 76 ?? 33 C0 48 8B 74 24 38 48 83 C4 20 5F C3 41 8B D2 48 89 5C 24 30",nullptr));
    g_notifyTeleport=reinterpret_cast<NotifyTeleportFn>(host->find(
        "40 53 48 83 EC 30 8B C2 41 B9 10 00 00 00 41 8B D8 89 44 24 20 45 0F B6 C1 48 8D 51 10 E8 ?? ?? ?? ?? 48 85 C0 75 ?? 48 83 C4 30 5B C3 89 58 0C B0 01 48 83 C4 30 5B C3 CC CC CC CC CC CC CC CC",nullptr));
    g_getComponent=reinterpret_cast<GetComponentFn>(host->find(
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 4C 8B D1",nullptr));
    auto* r = host->find("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D8 F6 FF FF 48 81 EC 28 0A 00 00 4C 8B E9", nullptr);
    if (!r) {
        r = host->find("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 98 F9 FF FF 48 81 EC 68 07 00 00 4C 8B F9", nullptr);
    }
    auto* chat = host->find("40 55 56 41 54 41 55 41 57 48 83 EC 30 49 8B 41 08 33 ED", nullptr);
    auto* ground = host->find(
        "4C 8B DC 49 89 5B 10 49 89 7B 20 41 56 48 81 EC B0 00 00 00 45 0F 29 43 C8 48 8B D9 "
        "49 8B 08 33 C0 F3 44 0F 10 05 ?? ?? ?? ?? 4D 8B F0 41 0F 29 73 E8 48 8B FA 41 0F 29 7B D8 "
        "66 41 89 43 19",
        nullptr);

    if (!p || !a || !n || !g_entityOf || !g_dirty || !g_ray || !r || !chat || !ground || !g_queueTeleport || !g_notifyTeleport || !g_getComponent) {
        Log("Town Portal: missing host travel signatures");
        return false;
    }
    if (!CallMatches(host, p + 0x35, n) || !CallMatches(host, p + 0x11c6, n) ||
        !CallMatches(host, a + 0x31, n) || !CallMatches(host, a + 0x7be, n)) {
        Log("Town Portal: iterator call sites did not match");
        return false;
    }

    g_playerFirst = reinterpret_cast<NextFn>(host->redirectCall(p + 0x35, reinterpret_cast<void*>(PlayerFirst)));
    g_playerMore = reinterpret_cast<NextFn>(host->redirectCall(p + 0x11c6, reinterpret_cast<void*>(PlayerMore)));
    g_applyFirst = reinterpret_cast<NextFn>(host->redirectCall(a + 0x31, reinterpret_cast<void*>(ApplyFirst)));
    g_applyMore = reinterpret_cast<NextFn>(host->redirectCall(a + 0x7be, reinterpret_cast<void*>(ApplyMore)));
    g_receive = reinterpret_cast<ReceiveFn>(host->hook(r, reinterpret_cast<void*>(ReceiveHook), 13, nullptr, 0));
    g_append = reinterpret_cast<AppendFn>(host->hook(chat, reinterpret_cast<void*>(AppendHook), 13, nullptr, 0));
    g_groundQuery = reinterpret_cast<GroundQueryFn>(host->hook(ground, reinterpret_cast<void*>(GroundHook), 13, nullptr, 0));

    // Optional (far creatures in a portal's live view): where the server lists its observers.
    // 15 bytes: a stack copy and three saves through it, no relative operands.
    if (auto* observers = host->find("48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 41 55 41 56 41 57 48 81 EC 60 01 00 00 4C 8B 2A 45 0F B6 E0", nullptr))
        g_observers = reinterpret_cast<ObserversFn>(host->hook(observers, reinterpret_cast<void*>(ObserversHook), 15, nullptr, 0));
    Log(g_observers ? "Town Portal: far creatures: the host can send a portal's far side" : "Town Portal: far creatures: observer list not found");
    // Optional: creatures by a portal's ends are not despawned (GatherHook). The call to the
    // gatherer sits at +0x6b in conditional_despawn; the gatherer's first 15 bytes are three
    // saves through rsp (no relative operands).
    {
        auto* despawn=static_cast<const uint8_t*>(host->find("40 55 41 56 48 8D AC 24 B8 FB FF FF 48 81 EC 48 05 00 00",nullptr));
        auto* gather=host->find("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 02 4C 8D 92 88 01 00 00",nullptr);
        uint8_t op=0;
        if(despawn)host->read(const_cast<uint8_t*>(despawn)+0x6b,&op,1);
        if(despawn&&gather&&op==0xE8){
            g_despawnReturn=despawn+0x70;
            g_gather=reinterpret_cast<GatherFn>(host->hook(gather,reinterpret_cast<void*>(GatherHook),15,nullptr,0));
        }
        Log(g_gather?"Town Portal: creatures near open portals are kept":"Town Portal: creature despawn check not found; creatures near portals are not kept");
    }
    // Optional: areas next to a portal's ends are not unloaded (UnloadHook). 14 bytes: a save
    // through rsp, push, sub and a compare whose flags the trampoline's jump back keeps.
    if(auto* unload=host->find("48 89 5C 24 10 56 48 83 EC 30 80 7A 01 00",nullptr))
        g_unload=reinterpret_cast<UnloadFn>(host->hook(unload,reinterpret_cast<void*>(UnloadHook),14,nullptr,0));
    Log(g_unload?"Town Portal: areas next to open portals stay loaded":"Town Portal: area unload not found; areas next to portals may unload");
#ifdef TOWN_PORTAL_DEV
    if(auto* destroy=static_cast<uint8_t*>(host->find("48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 55",nullptr))){
        uint8_t call[5]{};host->read(destroy+0x45,call,5);
        if(call[0]==0xE8){
            int32_t rel;std::memcpy(&rel,call+1,4);
            g_entityIndex=reinterpret_cast<IndexFn>(destroy+0x4a+rel);
            g_destroy=reinterpret_cast<DestroyFn>(host->hook(destroy,reinterpret_cast<void*>(DestroyHook),14,nullptr,0));
        }
    }
    if(auto* queue=host->find("40 53 48 83 EC 30 41 8B D8 8B C2 45 85 C0 74 ?? 48 8D 51 10 89 44 24 20 41 B9 10 00 00 00 41 B0 01",nullptr))
        g_queueDestroy=reinterpret_cast<QueueDestroyFn>(host->hook(queue,reinterpret_cast<void*>(QueueDestroyHook),14,nullptr,0));
    Log(g_queueDestroy?"Town Portal (dev): logging who queues destroys":"Town Portal (dev): destroy queue not found");
    Log(g_destroy?"Town Portal (dev): logging entities destroyed near portals":"Town Portal (dev): entity destroy not found");
#endif

    bool ok = g_playerFirst && g_playerMore && g_applyFirst && g_applyMore &&
              g_receive && g_append && g_groundQuery;

    // Portal travel moves the player directly once the destination terrain is resident; the
    // native travel action (cast animation, teleport effect) is kept but switched off, so its
    // travel-gate hook is not installed.
    g_nativeTravel = false;
    if (auto* kept = KeptPortalBlock(); kept && kept->size == sizeof(KeptPortals) && kept->savedTick &&
        GetTickCount64() - kept->savedTick < 10000) {
        std::lock_guard guard(g_lock);
        const uint64_t now = GetTickCount64();
        std::memcpy(g_portals, kept->portals, sizeof(kept->portals));
        // Heard of just now, so a friend's portal is not closed as silent before its next sync.
        for (auto& slot : g_portals)
            if (slot.active) slot.lastSyncTick = now;
        kept->savedTick = 0;
        Log("Town Portal: open portals kept through the reload");
    }
    Log(ok ? "Town Portal: portal travel moves the player once destination terrain has loaded"
           : "Town Portal: host travel hooks unavailable");
    g_paused = !ok;
    return ok;
}

void SetClient(void* gameClient) {
    void* prev = g_client.exchange(gameClient);
    if (prev != gameClient) {
        g_hostAnswered = false;
    }
    if (gameClient) {
        g_localEntity = (at<uint32_t>(gameClient, 0x466b44) & 0x3fu) + 1u;
        if (prev != gameClient) {
            g_localAccount = LocalSteamAccount();
        }
    } else {
        g_localEntity = 0;
        g_localAccount = 0;
    }
}

void RequestFarView(uint32_t playerId, float x, float z) { SetFarView(playerId, int(x), int(z)); }
bool HostsWorld() {
    std::lock_guard guard(g_lock);
    return g_hostWorld.storage && GetTickCount64() - g_hostWorld.seen <= 2000;
}

void ObserveHostPosition(float wx, float wz) {
    if (g_faulted) return;
    std::lock_guard guard(g_lock);
    const uint64_t now = GetTickCount64();
    if (!g_hostWorld.storage || now - g_hostWorld.seen > 2000) return;
    PlayerSpot spots[16]{};
    size_t count = 0;
    for (const auto& s : g_hostSpots) {
        if (s.entity && now >= s.seenTick && now - s.seenTick < 1000) spots[count++] = {s.entity, s.wx, s.wz};
    }
    const uint32_t before = g_hostIdentity.Current();
    const bool wasConfirmed = g_hostIdentity.Confirmed();
    const bool changed = g_hostIdentity.Observe(wx, wz, spots, count, now);
    PublishHostIdentity();
    char line[128]{};
    if (changed && before) {
        std::snprintf(line, sizeof(line), "Town Portal: this host is player %u, not %u (position check)", g_hostIdentity.Current(), before);
    } else if (!wasConfirmed && g_hostIdentity.Confirmed()) {
        std::snprintf(line, sizeof(line), "Town Portal: this host is player %u (position check)", g_hostIdentity.Current());
    }
    if (line[0]) Log(line);
}

bool HostAnswered() {
    return g_hostAnswered.load();
}

Submission SubmitTeleport(uint32_t entity, const FixedPos& target) {
    {
        std::lock_guard guard(g_lock);
        if (!g_hostWorld.storage || GetTickCount64() - g_hostWorld.seen > 2000) {
            return Submission::remote;
        }
    }
    uint32_t obs = g_observedHostEntity.load();
    if ((g_hostEntityConfirmed.load() || !g_server.load()) && obs != 0 && obs <= 16) {
        entity = obs;
    }
    return QueueInternal(entity, g_localAccount.load(), GetTickCount64(), target)
               ? Submission::queued
               : Submission::rejected;
}

bool LocalTerrainReady(float wx, float wy, float wz) {
    std::lock_guard guard(g_lock);
    // The spot itself and rings at 12 m and 30 m: the surroundings must be resident too, not
    // just the ground underfoot. Unknown (no terrain grid) cannot be waited out; only
    // "loading" holds.
    float groundY = wy;
    if (ProbeDestination(wx, wy, wz, groundY) == DestinationTerrain::loading) return false;
    for (float radius : {12.0f, 30.0f}) {
        for (int i = 0; i < 8; ++i) {
            float angle = float(i) * 0.785398f;
            float y = wy;
            if (ProbeDestination(wx + std::cos(angle) * radius, wy, wz + std::sin(angle) * radius, y) ==
                DestinationTerrain::loading) return false;
        }
    }
    return true;
}

bool RefineLocalGroundY(float wx, float& wyInOut, float wz) {
    std::lock_guard guard(g_lock);
    uint8_t realm = g_hostWorld.realm;
    void* terrain = FindActiveTerrain(realm);
    if (!terrain) return false;
    float hitY = wyInOut;
    if (RaycastLocalVertical(terrain, realm, wx, wyInOut, wz, 2.5f, 2.2f, hitY)) {
        wyInOut = hitY;
        return true;
    }
    return false;
}

namespace {
math::Visibility SafeTraceVisibility(const VisibilityContext& context, math::Vec3 from, math::Vec3 to) {
    using math::Visibility;
    __try {
        if (!math::Finite(from)||!math::Finite(to)) return Visibility::unknown;
        math::Vec3 delta=to-from;
        float distance=std::sqrt(math::LengthSq(delta));
        if (distance<0.06f) return Visibility::unknown;
        // Tiny endpoint margins avoid self contact without allowing walls close
        // to the camera or portal to disappear from the segment.
        float margin=0.015f;
        math::Vec3 direction=delta*(1/distance);
        math::Vec3 a=from+direction*margin,b=to-direction*margin;
        alignas(16) float ray[8]={a.x,a.y,a.z,0,b.x,b.y,b.z,0};
        bool physicsChecked=false,terrainChecked=false;
        if (context.physics&&g_physicsRay&&
            (at<void*>(context.physics,0x1a0)||at<void*>(context.physics,0x118))) {
            alignas(16) uint8_t hit[0x80]{};
            hit[0]=0x1f; // verified native no-hit sentinel
            g_physicsRay(hit,context.physics,ray,context.ignoreEntity,context.ignoreEntity,0x829u,0,nullptr);
            physicsChecked=true;
            if (hit[0]==0) {
                math::Vec3 point{field<float>(hit,0x14),field<float>(hit,0x18),field<float>(hit,0x1c)};
                if (!math::Finite(point)) return Visibility::unknown;
                // Native hit/no-hit classification is authoritative; a hit at
                // the near endpoint must not be discarded by a distance epsilon.
                return Visibility::blocked;
            }
            if (hit[0]!=0x1f) return Visibility::unknown;
        }
        if (context.terrain&&g_ray) {
            float ox{},oy{},oz{};double width{},height{};
            if (ValidateTerrainGrid(context.terrain,context.realm,ox,oy,oz,width,height)&&
                a.x>=ox&&a.z>=oz&&a.x<ox+width&&a.z<oz+width&&
                b.x>=ox&&b.z>=oz&&b.x<ox+width&&b.z<oz+width&&
                a.y>=oy&&a.y<oy+height&&b.y>=oy&&b.y<oy+height) {
                alignas(16) uint8_t hit[64]{};
                const uint8_t options[4]={context.realm,0,0,1};
                g_ray(hit,context.terrain,ray,ray+4,options);
                terrainChecked=true;
                if (hit[0x13]) return Visibility::blocked;
            }
        }
        return physicsChecked&&terrainChecked?Visibility::clear:Visibility::unknown;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return math::Visibility::unknown;
    }
}
} // namespace

VisibilityContext GetVisibilityContext() {
    VisibilityContext context{};
    if (g_paused||g_faulted) return context;
    context.ignoreEntity=g_observedHostEntity.load();
    if (!context.ignoreEntity) context.ignoreEntity=g_localEntity.load();
    std::lock_guard guard(g_lock);
    context.terrain=FindActiveTerrain(context.realm,&context.physics);
    return context;
}

math::Visibility TraceVisibility(const VisibilityContext& context,math::Vec3 from,math::Vec3 to) {
    return SafeTraceVisibility(context,from,to);
}

bool IsRayOccluded(float fromX,float fromY,float fromZ,float toX,float toY,float toZ) {
    return !math::CanDraw(TraceVisibility(GetVisibilityContext(),{fromX,fromY,fromZ},{toX,toY,toZ}));
}

void GatherEcsRemotePlayers(std::vector<RemotePlayerPos>& outPlayers) {
    uint64_t now = GetTickCount64();
    uint32_t localEnt = g_observedHostEntity.load();
    if (!localEnt) localEnt = g_localEntity.load();
    std::lock_guard guard(g_lock);
    for (const auto& s : g_ecsPlayers) {
        if (s.entity != 0 && s.entity != localEnt && now >= s.seenTick && (now - s.seenTick) < 2500) {
            bool exists = false;
            for (const auto& ex : outPlayers) {
                float dx = ex.wx - s.wx;
                float dz = ex.wz - s.wz;
                if (dx * dx + dz * dz < 1.0f) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                RemotePlayerPos rp{};
                rp.wx = s.wx;
                rp.wy = s.wy;
                rp.wz = s.wz;
                outPlayers.push_back(rp);
            }
        }
    }
}

void SampleEndpointTerrain(PortalEndpoint& ep) {
    if (ep.hasTerrainGrid) return;
    std::lock_guard guard(g_lock);
    uint8_t realm = g_hostWorld.realm;
    void* terrain = FindActiveTerrain(realm);
    if (!terrain) return;

    int hits = 0;
    float grid[5][5]{};
    // Sample a 5x5 grid in local portal coordinates: u in [-6m..+6m] along Right, d in [1.5m..13.5m] along Normal
    for (int iz = 0; iz < 5; ++iz) {
        float dist = 2.0f + static_cast<float>(iz) * 3.0f;
        for (int ix = 0; ix < 5; ++ix) {
            float side = static_cast<float>(ix - 2) * 3.0f;
            float sx = ep.wx - side * ep.rx + dist * ep.nx;
            float sz = ep.wz - side * ep.rz + dist * ep.nz;
            float sy = ep.wy;
            if (RaycastLocalVertical(terrain, realm, sx, ep.wy, sz, 6.0f, 6.0f, sy)) {
                grid[iz][ix] = std::max(-4.5f, std::min(4.5f, sy - ep.wy));
                ++hits;
            } else {
                grid[iz][ix] = 0.0f;
            }
        }
    }
    if (hits >= 12) {
        std::memcpy(ep.terrainRelY, grid, sizeof(grid));
        ep.hasTerrainGrid = true;
    }
}

bool TakeBaseSuggestion(uint64_t portalId, BaseSpotMsg& out) {
    std::lock_guard guard(g_lock);
    if (!g_baseSuggestion.portalId) return false;
    const bool mine = g_baseSuggestion.portalId == portalId;
    if (mine) out = g_baseSuggestion;
    g_baseSuggestion = {};
    return mine;
}

void UpsertPortal(const PortalInstance& instance) {
    std::lock_guard guard(g_lock);
    UpsertPortalLocked(instance);
}

bool ClosePortalByOwner(uint64_t ownerAccount, uint64_t portalId) {
    std::lock_guard guard(g_lock);
    bool closedAny = false;
    for (auto& slot : g_portals) {
        if (slot.active && slot.ownerAccount == ownerAccount &&
            (!portalId || slot.portalId == portalId)) {
            slot.active = false;
            slot.closing = true;
            slot.closedTick = GetTickCount64();
            closedAny = true;
        }
    }
    return closedAny;
}

void CloseSilentPortals(uint64_t localAccount, uint64_t silentMs) {
    const uint64_t now = GetTickCount64();
    uint64_t closed[kMaxPortals]{};
    size_t count = 0;
    {
        std::lock_guard guard(g_lock);
        for (auto& slot : g_portals) {
            if (!slot.active || slot.ownerAccount == localAccount || !slot.lastSyncTick ||
                now - std::min(now, slot.lastSyncTick) < silentMs)
                continue;
            slot.active = false;
            slot.closing = true;
            slot.closedTick = now;
            closed[count++] = slot.portalId;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: closed portal %llu: its owner has not been heard from for %llu s (left the world)",
                      static_cast<unsigned long long>(closed[i]), static_cast<unsigned long long>(silentMs / 1000));
        Log(line);
    }
}

void ClearAllPortals() {
    // Leaving or switching worlds: the entity seen while hosting the last one is not this
    // player's in the next (a stale one made the field check trace into the player's own body).
    std::lock_guard guard(g_lock);
    g_hostIdentity.Reset();
    PublishHostIdentity();
    for (auto& slot : g_portals) {
        slot = {};
    }
}

void GetActivePortals(std::vector<PortalInstance>& outPortals) {
    outPortals.clear();
    uint64_t now = GetTickCount64();
    std::lock_guard guard(g_lock);
    for (auto& slot : g_portals) {
        if (slot.active) {
            outPortals.push_back(slot);
        } else if (slot.closing && now >= slot.closedTick && (now - slot.closedTick) < 550) {
            outPortals.push_back(slot);
        } else if (slot.closing && (now - slot.closedTick) >= 550) {
            slot.closing = false;
        }
    }
}

bool GetPortalByOwner(uint64_t ownerAccount, PortalInstance& outPortal) {
    std::lock_guard guard(g_lock);
    for (const auto& slot : g_portals) {
        if (slot.active && slot.ownerAccount == ownerAccount) {
            outPortal = slot;
            return true;
        }
    }
    return false;
}

bool MarkOwnerVisitedBase(uint64_t ownerAccount) {
    std::lock_guard guard(g_lock);
    for (auto& slot : g_portals) {
        if (slot.active && slot.ownerAccount == ownerAccount) {
            if (!slot.ownerVisitedBase) {
                slot.ownerVisitedBase = true;
                return true;
            }
            return false;
        }
    }
    return false;
}

bool Prepare() {
    g_paused = true;
    std::lock_guard guard(g_lock);
    for (const auto& p : g_requests) {
        if (p.stage != Stage::none) return false;
    }
    if (auto* kept = KeptPortalBlock()) {
        kept->size = sizeof(KeptPortals);
        std::memcpy(kept->portals, g_portals, sizeof(kept->portals));
        kept->savedTick = GetTickCount64();
    }
    return true;
}

void Resume() {
    g_paused = false;
}

void AppendDevStatus(char* json,uint32_t capacity) {
    auto n=std::strlen(json);if(n<2||capacity<n+320)return;
    std::lock_guard guard(g_lock);uint32_t pending=0,stage=0;
    for(const auto& p:g_requests)if(p.stage!=Stage::none){++pending;stage=static_cast<uint32_t>(p.stage);}
    std::snprintf(json+n-1,capacity-n+1,",\"travel\":{\"pending\":%u,\"stage\":%u,\"applied\":%llu,\"lastApplyLatencyMs\":%llu,\"actorSequence\":%s,\"nativeApplied\":%llu,\"directApplied\":%llu,\"gate\":%s}}",pending,stage,static_cast<unsigned long long>(g_appliedRequests),static_cast<unsigned long long>(g_lastApplyLatency),g_nativeTravel.load()?"true":"false",static_cast<unsigned long long>(g_nativeApplied),static_cast<unsigned long long>(g_directApplied),g_gate?"true":"false");
}

} // namespace portal::travel
