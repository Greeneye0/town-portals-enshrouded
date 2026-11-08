#define NOMINMAX
#include "portal_water.h"
#include "portal_travel.h"
#include "portal_rules.h"
#include "greeneye.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

// The server's water simulation (keen WaterSimulationServer, server object +0x1a8):
// The server object holds a pointer to the WaterSimulationServer resource, whose first field is the
// simulation itself; the functions below take the resource.
//   register  FUN_14114f840 (water_dispenser_server_init): (&sim, desc) with desc = world x, y, z
//             floats, entity id, the WaterDispenser component's value, its kind byte, a spare
//             byte and the active flag (0x18 bytes). An id already registered is left as it is.
//   toggle    FUN_14116df10 (water_dispenser_server_toggle): (&sim, id, active), false when the
//             id is unknown.
//   in water  FUN_141134820 (is_in_water): (&sim, fixed-point position), true under the surface.
// Records: capacity sim+0x3e4e8, occupancy bits *(sim+0x3e4e0), 0x38-byte records *(sim+0x3e518):
// +0 x, y, z in half metres (ints), +0xc kind, +0x10 id, +0x14 value, +0x31 active, +0x32 spare.
// Addresses are from build 2026-11-04; everything is found by signature.

namespace portal::water {
namespace {

struct DispenserDesc {
    float x, y, z;
    uint32_t id;
    uint32_t value;
    uint8_t kind;
    uint8_t spare;
    uint8_t active;
    uint8_t pad;
};
static_assert(sizeof(DispenserDesc) == 0x18);

using RegisterFn = void (*)(void** sim, DispenserDesc* desc);
using ToggleFn = uint8_t (*)(void** sim, uint32_t id, uint8_t active);
using InWaterFn = uint8_t (*)(void** sim, const int64_t* fixedPos);

using UpdateFn = void (*)(void* server, void* arg);
UpdateFn g_nextUpdate = nullptr;

RegisterFn g_register = nullptr;
ToggleFn g_toggle = nullptr;
InWaterFn g_inWater = nullptr;
int32_t g_simOffset = 0;

// A Metal Water Dispenser I as the simulation holds it (read from a placed one, 2026-11-08): kind 1,
// flow 1024, spare 0 (the world's own sources have 1). Tier II flows 2048.
constexpr uint8_t kDispenserKind = 1;
constexpr uint32_t kDispenserValue = 1024;

constexpr float kHalfHeight = 1.62f;  // the portal's centre above its ground point
constexpr float kSpoutHeight = 1.0f;  // where the water comes out
constexpr int64_t kFixedOne = int64_t(1) << 32;

struct Spout {
    uint64_t portalId;
    uint32_t id;
    bool on;
    bool seen;
};

// Server thread only.
std::vector<Spout> g_spouts;
void* g_sim = nullptr;
uint64_t g_nextCheck = 0;
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_stopped{true};
std::atomic<uint64_t> g_lastTick{0};

void Log(const char* line) {
    if (auto* host = greeneye::Host()) host->log(line);
#ifdef TOWN_PORTAL_DEV
    // Also a file of its own: a long-running server's loader.log can stop being written.
    std::wstring path = rules::Path();
    path.replace(path.size() - wcslen(L"town-portal.ini"), std::wstring::npos, L"water-dev.log");
    if (FILE* f = _wfopen(path.c_str(), L"a")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(f, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, line);
        std::fclose(f);
    }
#endif
}

bool Enabled() {
    static bool on = true;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 1000) return on;
    readAt = now;
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"Portals", L"PortalWater", L"true", value, 16, rules::Path().c_str());
    on = !(!_wcsicmp(value, L"false") || !_wcsicmp(value, L"0") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off"));
    return on;
}

// The key in a file written before it existed.
void AddIniKey() {
    rules::AddPortalsKey(L"PortalWater",
                         "; Water through portals: while one end of a portal is at least half under water, the other\n"
                         "; end pours water like a Metal Water Dispenser I, until that end is half under water too.\n"
                         "; true or false.\n"
                         "PortalWater=true\n");
}

void* SimOf(void* server) {
    __try {
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(server) + g_simOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool Submerged(void* sim, const PortalEndpoint& end) {
    int64_t pos[3]{end.pos.x, end.pos.y + int64_t(kHalfHeight * double(kFixedOne)), end.pos.z};
    auto** holder = static_cast<void**>(sim); // the WaterSimulationServer resource: its first field is the simulation
    __try {
        return g_inWater(holder, pos) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Toggle(void* sim, uint32_t id, bool active) {
    auto** holder = static_cast<void**>(sim); // the WaterSimulationServer resource: its first field is the simulation
    __try {
        return g_toggle(holder, id, active ? 1 : 0) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Register(void* sim, DispenserDesc desc) {
    auto** holder = static_cast<void**>(sim); // the WaterSimulationServer resource: its first field is the simulation
    __try {
        g_register(holder, &desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The same portal end in the same place always gets the same id, so a reloaded generation finds
// its spouts again. Kept clear of the sign bit (the game masks negative ids).
uint32_t SpoutId(uint64_t portalId, int side, const FixedPos& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    mix(portalId);
    mix(uint64_t(side));
    mix(uint64_t(p.x >> 30)); // quarter metres
    mix(uint64_t(p.y >> 30));
    mix(uint64_t(p.z >> 30));
    return 0x7e000000u | uint32_t(h & 0x00ffffffu);
}

void AllOff(void* sim) {
    for (auto& s : g_spouts)
        if (s.on && sim) {
            Toggle(sim, s.id, false);
            s.on = false;
        }
}

#ifdef TOWN_PORTAL_DEV
struct RecordView {
    int32_t x, y, z;
    uint8_t kind, spare, active;
    uint32_t id, value;
};

int ReadRecords(void* sim, RecordView* out, int cap) {
    int n = 0;
    __try {
        auto* s = *static_cast<uint8_t**>(sim);
        const uint64_t capacity = *reinterpret_cast<uint64_t*>(s + 0x3e4e8);
        auto* bits = *reinterpret_cast<uint64_t**>(s + 0x3e4e0);
        auto* recs = *reinterpret_cast<uint8_t**>(s + 0x3e518);
        if (!bits || !recs || capacity > 100000) return 0;
        for (uint64_t i = 0; i < capacity && n < cap; ++i) {
            if (!((bits[i >> 6] >> (i & 63)) & 1)) continue;
            auto* r = recs + i * 0x38;
            out[n++] = {*reinterpret_cast<int32_t*>(r), *reinterpret_cast<int32_t*>(r + 4), *reinterpret_cast<int32_t*>(r + 8),
                        r[0xc], r[0x32], r[0x31], *reinterpret_cast<uint32_t*>(r + 0x10),
                        *reinterpret_cast<uint32_t*>(r + 0x14)};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// Every water dispenser the simulation knows, logged whenever the list changes.
void DevDumpRecords(void* sim, uint64_t now) {
    static uint64_t next = 0;
    static uint64_t lastHash = 0;
    if (now < next) return;
    next = now + 3000;
    RecordView recs[256]{};
    const int n = ReadRecords(sim, recs, 256);
    uint64_t h = 1469598103934665603ull ^ uint64_t(n);
    for (int i = 0; i < n; ++i) {
        const auto* b = reinterpret_cast<const uint8_t*>(&recs[i]);
        for (size_t k = 0; k < sizeof(RecordView); ++k) h = (h ^ b[k]) * 1099511628211ull;
    }
    if (h == lastHash) return;
    lastHash = h;
    char line[200];
    {
        uint64_t raw[4]{};
        int32_t origin[3]{};
        uint8_t flags[2]{};
        __try {
            auto* s = *static_cast<uint8_t**>(sim);
            raw[0] = *reinterpret_cast<uint64_t*>(s + 0x3e4d8);
            raw[1] = *reinterpret_cast<uint64_t*>(s + 0x3e4e0);
            raw[2] = *reinterpret_cast<uint64_t*>(s + 0x3e4e8);
            raw[3] = *reinterpret_cast<uint64_t*>(s + 0x3e518);
            std::memcpy(origin, s + 0x84, sizeof(origin));
            flags[0] = s[0x3e5d0];
            flags[1] = s[0x3e5e1];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        std::snprintf(line, sizeof(line),
                      "Town Portal water (dev): sim %p offset 0x%x map %llx bits %llx cap %llu recs %llx origin %d %d %d on %u busy %u",
                      sim, g_simOffset, raw[0], raw[1], raw[2], raw[3], origin[0], origin[1], origin[2], flags[0], flags[1]);
        Log(line);
    }
    std::snprintf(line, sizeof(line), "Town Portal water (dev): %d dispenser records", n);
    Log(line);
    for (int i = 0; i < n; ++i) {
        const auto& r = recs[i];
        std::snprintf(line, sizeof(line),
                      "  dispenser id %08x kind %u value %u (0x%08x) spare %u active %u at voxel %d %d %d",
                      r.id, r.kind, r.value, r.value, r.spare, r.active, r.x, r.y, r.z);
        Log(line);
    }
}
#endif

// The server's update (it reads the simulation first), on a dedicated server and in the game of a
// player who hosts: the water is checked after it.
void UpdateHook(void* server, void* arg) {
    g_nextUpdate(server, arg);
    greeneye::Lease lease;
    if (lease) ServerTick(server);
}

} // namespace

bool Init(const GreeneyeHost* host) {
    AddIniKey();
    g_register = reinterpret_cast<RegisterFn>(host->find(
        "48 83 EC 58 F2 0F 10 0A F3 0F 10 15 ?? ?? ?? ?? 48 8B 09 F2 0F 11 4C 24 30 F2 0F 10 81 84 00 00 00 44 8B 89 8C 00 00 00",
        nullptr));
    g_toggle = reinterpret_cast<ToggleFn>(host->find(
        "48 89 5C 24 08 57 48 83 EC 20 48 8B 19 41 0F B6 F8 89 54 24 38 48 8D 54 24 38 48 8D 8B D8 E4 03 00 E8 ?? ?? ?? ?? 48 3B 83 E8 E4 03 00",
        nullptr));
    g_inWater = reinterpret_cast<InWaterFn>(host->find(
        "48 8B C4 57 48 81 EC 90 00 00 00 48 8B 39 0F 57 DB F3 0F 10 25 ?? ?? ?? ?? 0F 29 70 E8 0F 57 F6 0F 29 78 D8 0F 57 FF F2 0F 10 87 84 00 00 00",
        nullptr));
    // The server's update reads the simulation first: mov rcx, [rcx+disp32].
    if (auto* update = static_cast<uint8_t*>(host->find(
            "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 4C 8B F9 48 8B FA 48 8B 89 ?? ?? ?? ?? E8",
            nullptr)))
    {
        host->read(update + 34, &g_simOffset, 4);
        if (g_simOffset <= 0 || g_simOffset > 0x10000) g_simOffset = 0;
        // mov [rsp+10h],rbx / mov [rsp+18h],rsi / push rbp / push rdi / push r12: 14 bytes.
        if (g_simOffset && g_register && g_toggle && g_inWater)
            g_nextUpdate = reinterpret_cast<UpdateFn>(host->hook(update, reinterpret_cast<void*>(UpdateHook), 14, nullptr, 0));
    }
    const bool ok = g_nextUpdate != nullptr;
    if (!ok) Log("Town Portal: water simulation not found; portals don't carry water");
    return ok;
}

void ServerTick(void* server) {
    if (!g_register || !server) return;
    const uint64_t now = GetTickCount64();
    g_lastTick = now;
    void* sim = SimOf(server);
#ifdef TOWN_PORTAL_DEV
    static void* seenServer = nullptr;
    if (server != seenServer) {
        seenServer = server;
        char line[120];
        std::snprintf(line, sizeof(line), "Town Portal water (dev): server tick %p, resource %p", server, sim);
        Log(line);
    }
#endif
    if (sim != g_sim) { // a new world: the old simulation and its spouts are gone
        g_spouts.clear();
        g_sim = sim;
    }
    if (!sim) return;
    if (g_stopping.load()) {
        AllOff(sim);
        g_stopped = true;
        return;
    }
#ifdef TOWN_PORTAL_DEV
    DevDumpRecords(sim, now);
#endif
    if (now < g_nextCheck) return;
    g_nextCheck = now + 500;

    const bool enabled = Enabled() && !rules::NoFun();
    std::vector<PortalInstance> portals;
    travel::GetActivePortals(portals);
    for (auto& s : g_spouts) s.seen = false;
    for (const auto& p : portals) {
        if (!p.active || p.closing) continue;
        for (int side = 0; side < 2; ++side) {
            const PortalEndpoint& from = side == 0 ? p.fieldEnd : p.baseEnd;
            const PortalEndpoint& to = side == 0 ? p.baseEnd : p.fieldEnd;
            if ((!from.pos.x && !from.pos.z) || (!to.pos.x && !to.pos.z)) continue;
            const uint32_t id = SpoutId(p.portalId, side, to.pos);
            Spout* spout = nullptr;
            for (auto& s : g_spouts)
                if (s.id == id) spout = &s;
            const bool want = enabled && Submerged(sim, from) && !Submerged(sim, to);
            if (spout) spout->seen = true;
            if (want && (!spout || !spout->on)) {
                if (!spout) {
                    Register(sim, {to.wx, to.wy + kSpoutHeight, to.wz, id, kDispenserValue, kDispenserKind, 0, 1, 0});
                    if (!Toggle(sim, id, true)) continue; // outside the simulated area, or it was busy: next time
                    g_spouts.push_back({p.portalId, id, true, true});
                } else if (Toggle(sim, id, true)) {
                    spout->on = true;
                }
                char line[160];
                std::snprintf(line, sizeof(line), "Town Portal: portal %llu pours water at its %s end",
                              static_cast<unsigned long long>(p.portalId), side == 0 ? "base" : "far");
                Log(line);
            } else if (!want && spout && spout->on) {
                Toggle(sim, id, false);
                spout->on = false;
                char line[160];
                std::snprintf(line, sizeof(line), "Town Portal: portal %llu stopped pouring at its %s end",
                              static_cast<unsigned long long>(p.portalId), side == 0 ? "base" : "far");
                Log(line);
            }
        }
    }
    // Closed or moved portals: their old spouts stop (the simulation keeps them, switched off,
    // until the world is unloaded).
    for (size_t i = 0; i < g_spouts.size();) {
        if (!g_spouts[i].seen) {
            if (g_spouts[i].on) Toggle(sim, g_spouts[i].id, false);
            g_spouts.erase(g_spouts.begin() + i);
        } else {
            ++i;
        }
    }
}

bool Prepare() {
    if (!g_stopping.exchange(true)) g_stopped = false;
    if (GetTickCount64() - g_lastTick.load() > 2000) return true;
    return g_stopped.load();
}

void Resume() {
    g_stopping = false;
}

} // namespace portal::water
