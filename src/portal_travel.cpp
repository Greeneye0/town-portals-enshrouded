#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "portal_travel.h"
#include "portal_protocol.h"
#include "portal_travel_signatures.h"
#include "portal_rules.h"
#include "portal_names.h"
#include "portal_snapshot_share.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
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
    "48 89 4C 24 08 55 41 56 48 8D AC 24 E8 F5 FF FF 48 81 EC 18 0B 00 00 41 B8 A8 05 00 00 "
    "48 8D 55 00 4C 8B F1 E8 ?? ?? ?? ?? 41 B8 A8 05 00 00 48 8D 55 00 49 8B CE E8 ?? ?? ?? ?? "
    "84 C0 0F 84 ?? ?? ?? ??";
constexpr char kApplyPattern[] =
    "40 55 56 48 8D AC 24 A8 FD FF FF 48 81 EC 58 03 00 00 41 B8 B0 01 00 00 48 8D 55 90 "
    "48 8B F1 E8 ?? ?? ?? ?? 41 B8 B0 01 00 00 48 8D 55 90 48 8B CE E8 ?? ?? ?? ?? 84 C0 "
    "0F 84 ?? ?? ?? ?? 48 89 9C 24 70 03 00 00";
constexpr char kNextPattern[] =
    "40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 30 08 00 00 75 ?? 32 C0 "
    "48 83 C4 20 5F C3 41 0F B6 80 B0 04 00 00 49 8D 88 70 04 00 00 48 89 5C 24 38 45 33 DB "
    "41 0F B7 98 BA 04 00 00";
constexpr char kEntityPattern[] =
    "8B 41 08 4C 8B C2 85 C0 75 ?? 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 "
    "28 08 00 00 8B 04 D1 41 89 00 49 8B C0 C3 CC CC CC CC CC CC 8B 05 ?? ?? ?? ?? 48 85 C9 "
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
std::atomic<bool> g_hostAnswered{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_faulted{false};
// The native travel action needs no new signature beyond the player system; the travel gate
// override is optional (without it the game's own area restrictions refuse the action, and
// the request falls back to the direct path).
std::atomic<bool> g_nativeTravel{false};

std::mutex g_lock;

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
PortalInstance g_portals[kMaxPortals]{};

struct EcsPlayerSlot {
    uint32_t entity = 0;
    uint64_t seenTick = 0;
    float wx = 0.0f;
    float wy = 0.0f;
    float wz = 0.0f;
};
EcsPlayerSlot g_ecsPlayers[16]{};

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
    if (account == g_localAccount.load()) return g_localEntity.load();
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

// The native travel gate (call at player system +0xfb2) answers 1 = travel disabled in this
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

    if (g_hostWorld.storage && g_hostWorld.storage != storage) {
        for (auto& p : g_requests) p = {};
        g_hostWorld.terrain = nullptr;
        g_observedHostEntity.store(0);
    }
    g_hostWorld.storage = storage;
    g_hostWorld.seen = now;
    if (id != 0 && id <= 16) {
        g_observedHostEntity.store(id);
    }

    uint64_t localAcc = g_localAccount.load();
    for (auto& p : g_requests) {
        if (p.stage == Stage::none) continue;
        // Match either exact entity ID or local host player account when world reassigned entity ID (e.g. entity=2)
        if (p.entity != id) {
            if (p.account != 0 && p.account == localAcc && !g_server.load()) {
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
            p.target.y = y;
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
            UpsertPortalLocked(inst);
        }
        // On the host store (not client store), let the server relay the broadcast to all connected clients,
        // and consume it on the client store so it never prints in the chat UI.
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
    if (!CallMatches(host, p + 0x36, n) || !CallMatches(host, p + 0x21f6, n) ||
        !CallMatches(host, a + 0x31, n) || !CallMatches(host, a + 0x7be, n)) {
        Log("Town Portal: iterator call sites did not match");
        return false;
    }

    g_playerFirst = reinterpret_cast<NextFn>(host->redirectCall(p + 0x36, reinterpret_cast<void*>(PlayerFirst)));
    g_playerMore = reinterpret_cast<NextFn>(host->redirectCall(p + 0x21f6, reinterpret_cast<void*>(PlayerMore)));
    g_applyFirst = reinterpret_cast<NextFn>(host->redirectCall(a + 0x31, reinterpret_cast<void*>(ApplyFirst)));
    g_applyMore = reinterpret_cast<NextFn>(host->redirectCall(a + 0x7be, reinterpret_cast<void*>(ApplyMore)));
    g_receive = reinterpret_cast<ReceiveFn>(host->hook(r, reinterpret_cast<void*>(ReceiveHook), 13, nullptr, 0));
    g_append = reinterpret_cast<AppendFn>(host->hook(chat, reinterpret_cast<void*>(AppendHook), 13, nullptr, 0));
    g_groundQuery = reinterpret_cast<GroundQueryFn>(host->hook(ground, reinterpret_cast<void*>(GroundHook), 13, nullptr, 0));

    bool ok = g_playerFirst && g_playerMore && g_applyFirst && g_applyMore &&
              g_receive && g_append && g_groundQuery;

    // Portal travel moves the player directly once the destination terrain is resident; the
    // native travel action (cast animation, teleport effect) is kept but switched off, so its
    // travel-gate hook is not installed.
    g_nativeTravel = false;
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

bool HostsWorld() {
    std::lock_guard guard(g_lock);
    return g_hostWorld.storage && GetTickCount64() - g_hostWorld.seen <= 2000;
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
    if (!g_server.load() && obs != 0 && obs <= 16) {
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

void ClearAllPortals() {
    // Leaving or switching worlds: the entity seen while hosting the last one is not this
    // player's in the next (a stale one made the field check trace into the player's own body).
    g_observedHostEntity.store(0);
    std::lock_guard guard(g_lock);
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
