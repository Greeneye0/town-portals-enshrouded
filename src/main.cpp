#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#include <dxgi.h>
#pragma comment(lib, "advapi32.lib") // RegGetValueW: Steam's folder, for the game's volume settings
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "greeneye.h"
#include "portal_build.h"
#include "portal_protocol.h"
#include "portal_render.h"
#include "portal_colors.h"
#include "portal_names.h"
#include "portal_markers.h"
#include "portal_prompt_view.h"
#include "portal_travel.h"
#include "portal_types.h"
#include "portal_scene_probe.h"
#include "portal_snapshot_share.h"
#include "portal_snapshot_timing.h"
#include "portal_cast.h"
#include "portal_rules.h"
#include "portal_voicepack.h"
#include "portal_default_images.h"
#include "portal_default_view.h"

namespace {

template <class T>
T& at(void* p, size_t n) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + n);
}

using LoopFn = void (*)(void*);
using DispatchFn = void (*)(void*, void*);

LoopFn g_nextLoop = nullptr;
LoopFn g_nextPlay = nullptr;
DispatchFn g_dispatch = nullptr;
// Client offset of the server's "text chat allowed" flag; 0 when not found.
int32_t g_textChatAllowed = 0;
void** g_root = nullptr;
void* g_currentApp = nullptr;

uint64_t g_frame = 0;
uint64_t g_seenPlayFrame = 0;
void* g_lastWorld = nullptr;

bool g_paused = false;
bool g_tWasDown = false;
bool g_eWasDown = false;
uint64_t g_lastTeleportTick = 0;
uint64_t g_lastBroadcastTick = 0;
uint64_t g_castUntil = 0;
float g_promptAnchor[3]{}; // the portal end the prompt is for (world, about hand height)
// The character's facing. Enshrouded turns a character toward where it moves (it does not
// turn in place with the camera), so the last real movement direction is where it faces;
// the camera direction is only used until the player has moved.
struct Facing { bool known = false, anchored = false; float x = 0, z = 1, lastX = 0, lastZ = 0; } g_facing;

void TrackFacing(float px, float pz) {
    if (g_facing.anchored) {
        float dx = px - g_facing.lastX, dz = pz - g_facing.lastZ, d2 = dx * dx + dz * dz;
        if (d2 < 0.03f * 0.03f) return; // keep accumulating slow steps
        if (d2 < 3.0f * 3.0f) {
            float inv = 1.0f / std::sqrt(d2);
            g_facing.x = dx * inv;
            g_facing.z = dz * inv;
            g_facing.known = true;
        }
    }
    g_facing.lastX = px;
    g_facing.lastZ = pz;
    g_facing.anchored = true;
}

// The owner going back through from the base closes their portal: at once when they are
// alone in the world, else after a grace period so anyone who followed them can come back.
constexpr uint64_t kCloseGraceMs = 15000;
struct PendingClose { uint64_t portalId = 0, at = 0; } g_pendingClose;

bool OthersInWorld(uint8_t* uiClient) {
    __try {
        uint64_t count = *reinterpret_cast<uint64_t*>(uiClient + 0x4642c8);
        return count > 1 && count <= 64;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The portal being channelled and where the caster stood when it started.
struct Channel { uint64_t portalId = 0, until = 0; float x = 0, y = 0, z = 0; } g_channel;
// Altar snapshots stand just behind the portal's spot, looking through it at the altar (the
// view from the portal itself). The player's character is hidden while they are taken.
constexpr float kAltarCamera = -0.5f;
// The spell effect from the caster's hand to the forming portal (portal_composite.frag
// CastEffect): 1 ribbon, 2 motes, 3 arcs. Motes were chosen in game (2026-10-27).
constexpr int g_castEffect = 2;
#ifdef TOWN_PORTAL_DEV
bool g_devInteractRequested = false;
#endif

struct HostLink {
    void* client = nullptr;
    void* world = nullptr;
    uint64_t helloAt = 0;
    uint64_t deadline = 0;
    int asked = 0;
    bool absent = false;
} g_link;

// Travel is applied by the host, so a server without Town Portal cannot use
// portals. Until the host answers, T and E show a notice instead.
enum class HostMod { present, checking, absent };
HostMod g_hostMod = HostMod::checking;

void Log(const char* msg) {
    if (auto* h = greeneye::Host(); h && h->log) {
        h->log(msg);
    }
}

void* GetGameClient(void* app) {
    auto* ui = app ? at<void*>(app, 0x35280) : nullptr;
    return ui ? at<void*>(ui, 0x1b8) : nullptr;
}

uint8_t* g_cachedUiClient = nullptr;

uint8_t* GetUiClient() {
    void* g = g_root ? *g_root : nullptr;
    if (g) {
        auto** pD0 = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(g) + 0xd0);
        if (pD0 && *pD0) {
            if (auto* uic = *reinterpret_cast<uint8_t**>(*pD0 + 0x38)) {
                g_cachedUiClient = uic;
                return uic;
            }
        }
    }
    if (void* gc = GetGameClient(g_currentApp)) {
        return static_cast<uint8_t*>(gc) + 0x1f0;
    }
    return g_cachedUiClient;
}

bool IsGameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// While the portal's travel prompt shows, E is the portal's alone: the game would also use the
// press on whatever else is in reach (a grapple point, the altar, a workbench). The game reads
// keys as raw input (WM_INPUT); a message hook on its window thread turns E's key-down into
// WM_NULL there. Town Portal itself reads the key with GetAsyncKeyState, which still sees it.
// Releases pass, so no key sticks. Removed in HotPrepare before this generation unloads.
std::atomic<uint64_t> g_portalPromptUntil{0};
HHOOK g_keyHook = nullptr;
LRESULT CALLBACK PortalKeyHook(int code, WPARAM wp, LPARAM lp) {
    greeneye::Lease lease;
    if (lease && code == HC_ACTION && GetTickCount64() < g_portalPromptUntil.load()) {
        auto* msg = reinterpret_cast<MSG*>(lp);
        if (msg->message == WM_INPUT) {
            RAWINPUT input{};
            UINT size = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                input.header.dwType == RIM_TYPEKEYBOARD && input.data.keyboard.VKey == 'E' &&
                !(input.data.keyboard.Flags & RI_KEY_BREAK))
                msg->message = WM_NULL;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
void InstallKeyHook() {
    if (g_keyHook) return;
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    const DWORD thread = window ? GetWindowThreadProcessId(window, &pid) : 0;
    if (!thread || pid != GetCurrentProcessId()) return;
    g_keyHook = SetWindowsHookExW(WH_GETMESSAGE, PortalKeyHook, nullptr, thread);
    Log(g_keyHook ? "Town Portal: E is kept from other interactions while the portal prompt shows"
                  : "Town Portal: key hook unavailable; E may also reach nearby objects at a portal");
}
void RemoveKeyHook() {
    if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    g_keyHook = nullptr;
    g_portalPromptUntil = 0;
}

bool IsTextInputActive() {
    void* g = g_root ? *g_root : nullptr;
    if (!g) return false;
    auto* owner = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 0xd0);
    if (!owner || !*reinterpret_cast<void**>(owner)) return false;
    return owner[0x2fe70] != 0;
}

bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// A gamepad's Y, the button the portal prompt shows for the game's interact, through XInput
// (Steam Input and Proton provide it too). Asking about an empty slot can stall for a while,
// so slots without a pad are tried again only every 2 s.
bool PadInteractDown() {
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static GetStateFn getState = [] {
        HMODULE module = GetModuleHandleW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");
        return module ? reinterpret_cast<GetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    }();
    static uint64_t retryAt[XUSER_MAX_COUNT]{};
    if (!getState) return false;
    const uint64_t now = GetTickCount64();
    bool down = false;
    for (DWORD pad = 0; pad < XUSER_MAX_COUNT; ++pad) {
        if (now < retryAt[pad]) continue;
        XINPUT_STATE state{};
        if (getState(pad, &state) != ERROR_SUCCESS) { retryAt[pad] = now + 2000; continue; }
        down |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0;
    }
    return down;
}

void SendHiddenLine(void* gameClient, std::string_view text) {
    if (!gameClient || !g_dispatch || text.empty() || text.size() > 240) return;
    alignas(8) uint8_t command[0x410]{};
    command[0] = 0x74;
    command[1] = portal::kChatType;
    std::memcpy(command + 2, text.data(), text.size());
    at<uint16_t>(command, 0x402) = static_cast<uint16_t>(text.size());
    // The game sends chat only while the server allows text chat, or system lines (type 4).
    // Hidden protocol lines must also reach servers with text chat off, so allow this one send.
    if (!g_textChatAllowed) {
        g_dispatch(gameClient, command);
        return;
    }
    auto& allowed = at<uint8_t>(gameClient, size_t(g_textChatAllowed));
    uint8_t saved = allowed;
    allowed = 1;
    g_dispatch(gameClient, command);
    allowed = saved;
}

HostMod EnsureHostHandshake(void* gameClient, void* currentWorld) {
    if (portal::travel::HostsWorld() || portal::travel::HostAnswered()) return HostMod::present;
    uint64_t now = GetTickCount64();
    if (g_link.client != gameClient || g_link.world != currentWorld || !g_link.deadline) {
        g_link = {gameClient, currentWorld, now + 2500, now + 15000, 0, false};
    }
    // Ask twice (2.5 s and 8.5 s after joining), in case the first hello is lost while the world loads.
    if (g_link.asked < 2 && now >= g_link.helloAt) {
        ++g_link.asked;
        g_link.helloAt = now + 6000;
        SendHiddenLine(gameClient, portal::kHello);
        Log(g_link.asked == 1 ? "Town Portal: asking the host whether it runs Town Portal"
                              : "Town Portal: asking the host again");
    }
    if (now < g_link.deadline) return HostMod::checking;
    if (!g_link.absent) {
        g_link.absent = true;
        Log("Town Portal: host did not answer; portals are unavailable in this world");
    }
    return HostMod::absent;
}

// True when portals can be used here; otherwise shows why in the player's language.
bool PortalsAvailable() {
    if (g_hostMod == HostMod::present) return true;
    auto lang = portal::render::Language();
    portal::render::ShowNotice("Town Portal",
        portal::text::Get(lang, g_hostMod == HostMod::checking ? portal::text::StillChecking
                                                               : portal::text::ServerMissing),
        6000);
    return false;
}

// Reads the local player's live 3D coordinates (wx, wy, wz) and horizontal look direction (dirX, dirZ).
bool GetPlayerPosAndFacing(uint8_t* uiClient, float& outX, float& outY, float& outZ,
                           float& outDirX, float& outDirZ) {
    if (!uiClient) return false;

    // Extract camera 3D position and forward look vector from View Matrix (uiClient + 0x1d20)
    float v[16]{};
    std::memcpy(v, uiClient + 0x1d20, sizeof(v));
    float camX = -(v[0] * v[12] + v[1] * v[13] + v[2] * v[14]);
    float camY = -(v[4] * v[12] + v[5] * v[13] + v[6] * v[14]);
    float camZ = -(v[8] * v[12] + v[9] * v[13] + v[10] * v[14]);

    bool gotPos = false;
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;

    // 1. Primary live 64-bit fixed-point player world position from clientWorld (+0x47d2f0)
    if (uint8_t* clientWorld = *reinterpret_cast<uint8_t**>(uiClient + 0x47d2f0)) {
        int64_t rawX = *reinterpret_cast<int64_t*>(clientWorld + 0x2c08);
        int64_t rawY = *reinterpret_cast<int64_t*>(clientWorld + 0x2c10);
        int64_t rawZ = *reinterpret_cast<int64_t*>(clientWorld + 0x2c18);
        if (rawX != 0 || rawZ != 0) {
            float px = static_cast<float>(static_cast<double>(rawX) * portal::kInvFixedScale);
            float py = static_cast<float>(static_cast<double>(rawY) * portal::kInvFixedScale);
            float pz = static_cast<float>(static_cast<double>(rawZ) * portal::kInvFixedScale);
            if (px > 100.0f && px < 12000.0f && pz > 100.0f && pz < 12000.0f) {
                wx = px;
                wy = py;
                wz = pz;
                gotPos = true;
            }
        }
    }

    // 2. Fallback to UI player coordinates in d0 (+0x14030 / +0x14038 / +0x14040)
    if (!gotPos && g_root && *g_root) {
        if (auto** pD0 = *reinterpret_cast<uint8_t***>(static_cast<uint8_t*>(*g_root) + 0xd0)) {
            if (uint8_t* d0 = *pD0) {
                int64_t rawPx = *reinterpret_cast<int64_t*>(d0 + 0x14030);
                int64_t rawPy = *reinterpret_cast<int64_t*>(d0 + 0x14038);
                int64_t rawPz = *reinterpret_cast<int64_t*>(d0 + 0x14040);
                if (rawPx != 0 || rawPz != 0) {
                    float px = static_cast<float>(rawPx) / 65536.0f;
                    float py = static_cast<float>(rawPy) / 65536.0f;
                    float pz = static_cast<float>(rawPz) / 65536.0f;
                    if (px > 100.0f && px < 12000.0f && pz > 100.0f && pz < 12000.0f) {
                        wx = px;
                        wy = py;
                        wz = pz;
                        gotPos = true;
                    }
                }
            }
        }
    }

    // 3. Fallback to uiClient + 0x38 / +0x3c / +0x40
    if (!gotPos) {
        float cpx = *reinterpret_cast<float*>(uiClient + 0x38);
        float cpy = *reinterpret_cast<float*>(uiClient + 0x3c);
        float cpz = *reinterpret_cast<float*>(uiClient + 0x40);
        if (std::isfinite(cpx) && std::isfinite(cpz) &&
            cpx > 100.0f && cpx < 12000.0f && cpz > 100.0f && cpz < 12000.0f) {
            wx = cpx;
            wy = cpy;
            wz = cpz;
            gotPos = true;
        }
    }

    if (!gotPos) return false;

    // Validate wy against camera Y (in 3rd-person view, player feet are ~1.5m-2.5m below camera Y)
    if (!std::isfinite(wy) || wy <= 1.0f || wy > 5000.0f ||
        (std::isfinite(camY) && camY > 10.0f && std::fabs(wy - camY) > 25.0f)) {
        wy = (std::isfinite(camY) && camY > 10.0f) ? (camY - 1.75f) : 250.0f;
        portal::travel::RefineLocalGroundY(wx, wy, wz);
    }

    // Compute horizontal facing direction from camera forward vector (+v[2], +v[10]) or camera-to-player vector
    float fx = v[2];
    float fz = v[10];
    float fLenSq = fx * fx + fz * fz;
    if (std::isfinite(fLenSq) && fLenSq > 0.01f) {
        float inv = 1.0f / std::sqrt(fLenSq);
        outDirX = fx * inv;
        outDirZ = fz * inv;
    } else if (std::isfinite(camX) && std::isfinite(camZ)) {
        float dx = wx - camX;
        float dz = wz - camZ;
        float dLenSq = dx * dx + dz * dz;
        if (dLenSq > 0.01f) {
            float inv = 1.0f / std::sqrt(dLenSq);
            outDirX = dx * inv;
            outDirZ = dz * inv;
        } else {
            outDirX = 0.0f;
            outDirZ = 1.0f;
        }
    } else {
        outDirX = 0.0f;
        outDirZ = 1.0f;
    }

    outX = wx;
    outY = wy;
    outZ = wz;
    return true;
}

// The client's altar list, rebuilt by the game every update (evidence:
// C:\temp\enshrouded-re\out\preferred-altar\evidence.txt): records of 0xBC8 bytes at uiClient+0x44b810, each listing
// altars of 0x48 bytes (entity id +0x00, fixed-point position +0x28/+0x30/+0x38, and +0x40
// the preferred spawn flag copied from ClientBaseIdComponent.isPreferredSpawnAltar). The
// Flame Altar menu reads the same flag.
bool ReadPreferredAltar(uint8_t* uiClient, int64_t out[3]) {
    __try {
        auto* records = *reinterpret_cast<uint8_t**>(uiClient + 0x44b810);
        uint64_t count = *reinterpret_cast<uint64_t*>(uiClient + 0x44b818);
        if (!records || count == 0 || count > 64) return false;
        for (uint64_t r = 0; r < count; ++r) {
            uint8_t* record = records + r * 0xBC8;
            auto* altars = *reinterpret_cast<uint8_t**>(record + 0x28);
            uint64_t altarCount = *reinterpret_cast<uint64_t*>(record + 0x30);
            if (!altars || altarCount > 256) continue;
            for (uint64_t i = 0; i < altarCount; ++i) {
                uint8_t* altar = altars + i * 0x48;
                if (!altar[0x40]) continue;
                std::memcpy(out, altar + 0x28, 3 * sizeof(int64_t));
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// Resolves the Flame Altar the portal links to: the world's preferred spawn altar (the
// altar list's flag), else the nearest Flame Altar map marker, else a Cinder Vault.
// Marker icon hashes: every Flame Altar marker stores 0xa447cba3; 0x6a56c5b3 is only the
// icon drawn for the preferred one, kept here in case a marker ever stores it.
bool FindPreferredSpawnAltar(uint8_t* uiClient, float playerX, float playerZ,
                             portal::FixedPos& outAltarFixed,
                             float& outAltarX, float& outAltarY, float& outAltarZ) {
    if (!uiClient) return false;

    int64_t preferred[3]{};
    if (ReadPreferredAltar(uiClient, preferred)) {
        float wx = static_cast<float>(static_cast<double>(preferred[0]) * portal::kInvFixedScale);
        float wy = static_cast<float>(static_cast<double>(preferred[1]) * portal::kInvFixedScale);
        float wz = static_cast<float>(static_cast<double>(preferred[2]) * portal::kInvFixedScale);
        if (wx > 100.0f && wx < 12000.0f && wz > 100.0f && wz < 12000.0f) {
            outAltarFixed = {preferred[0], preferred[1], preferred[2]};
            outAltarX = wx;
            outAltarY = wy;
            outAltarZ = wz;
            return true;
        }
    }
    Log("Town Portal: no preferred spawn altar flagged; using the nearest Flame Altar");

    constexpr uint32_t kPreferredIconHash = 0x6a56c5b3u;
    constexpr uint32_t kAltarHash = 0xa447cba3u;
    constexpr uint32_t kCinderVaultHash = 0x813080bcu;
    constexpr size_t kArrays[2] = {0x36ea00, 0x3e6a78};

    int bestTier = 99;
    float bestDistSq = 1e18f;
    portal::FixedPos bestFixed{};
    float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;

    for (size_t a = 0; a < 2; ++a) {
        uint8_t* base = *reinterpret_cast<uint8_t**>(uiClient + kArrays[a]);
        uint64_t count = *reinterpret_cast<uint64_t*>(uiClient + kArrays[a] + 8);
        if (!base || count == 0 || count > 10000) continue;

        for (uint64_t i = 0; i < count; ++i) {
            uint8_t* m = base + i * 0xA0;
            uint32_t uid = *reinterpret_cast<uint32_t*>(m + 0x00);
            if ((uid & 0xFFFF0000u) == 0x77700000u) continue; // skip injected chest pins

            uint32_t curHash = *reinterpret_cast<uint32_t*>(m + 0x30);
            uint32_t savedOrigHash = *reinterpret_cast<uint32_t*>(m + 0x3c);
            uint32_t effHash = (curHash != 0u && curHash != 0xafce0c5fu) ? curHash : savedOrigHash;

            int tier = 99;
            if (curHash == kPreferredIconHash || effHash == kPreferredIconHash) {
                tier = 2; // preferred-altar icon
            } else if (curHash == kAltarHash || effHash == kAltarHash) {
                tier = 3; // Flame Altar
            } else if (curHash == kCinderVaultHash || effHash == kCinderVaultHash) {
                tier = 4; // Fallback to Cinder Vault if no Altar exists
            } else {
                continue;
            }

            int64_t rawX = *reinterpret_cast<int64_t*>(m + 0x10);
            int64_t rawY = *reinterpret_cast<int64_t*>(m + 0x18);
            int64_t rawZ = *reinterpret_cast<int64_t*>(m + 0x20);
            float wx = static_cast<float>(static_cast<double>(rawX) * portal::kInvFixedScale);
            float wy = static_cast<float>(static_cast<double>(rawY) * portal::kInvFixedScale);
            float wz = static_cast<float>(static_cast<double>(rawZ) * portal::kInvFixedScale);
            if (!(wx > 100.0f && wx < 12000.0f && wz > 100.0f && wz < 12000.0f)) continue;

            float dx = wx - playerX;
            float dz = wz - playerZ;
            float distSq = dx * dx + dz * dz;

            if (tier < bestTier || (tier == bestTier && distSq < bestDistSq)) {
                bestTier = tier;
                bestDistSq = distSq;
                bestFixed = {rawX, rawY, rawZ};
                bestX = wx;
                bestY = wy;
                bestZ = wz;
            }
        }
    }

    if (bestTier > 4) return false;
    outAltarFixed = bestFixed;
    outAltarX = bestX;
    outAltarY = bestY;
    outAltarZ = bestZ;
    return true;
}

// The host holds a remote player's move until the destination terrain is resident (up to
// 12 s); its request table refuses a second request meanwhile, so don't send one (or replay
// the portal sound) until it had time to finish.
constexpr uint64_t kRemoteTravelBusyMs = 4500;

// Arrival: a loading screen (with movement locked) from the moment the player steps through
// until the area around the destination has loaded on this machine, so the player never moves
// through a world that is still streaming in.
//
// The game's own signal is the client's "ready for gameplay" byte (evidence:
// C:\temp\enshrouded-re\out\area-ready\evidence.txt): it drops to 0 when the local player's
// teleport reaches this client and returns to 1 once every scene-render-finished flag is set
// and the other loading subsystems are idle; it also gates the game's join loading screen.
// Its offset is read from the instruction that stores it. Without it, or when a short hop
// never clears it, terrain residency around the player plus hitch-free frames decide.
constexpr uint64_t kArrivalTimeoutMs = 15000;
struct Arrival {
    uint64_t since = 0, arrivedAt = 0, readyAt = 0, smoothSince = 0, nativeReadyAt = 0;
    bool sawLoading = false;
    float x = 0, y = 0, z = 0;
    // Arriving at the altar: a snapshot of it is taken before the loading screen goes.
    portal::PortalEndpoint snapshotEnd{};
    float snapshotDistance = 0;
    uint64_t snapshotAt = 0;
    bool wantSnapshot = false;
    portal::homecache::Key home{}; // the owner arriving home: the snapshot is saved for logins
} g_arrival;
struct PendingPortalTravel {
    portal::snapshot::DepartureGate gate{};
    portal::PortalInstance portal{};
    portal::FixedPos target{};
    portal::PortalEndpoint snapshotEnd{};
    float snapshotDistance{};
    uint32_t tint{};
    bool field{}, shroud{};
    std::string destination;
} g_departure;
int32_t g_areaReady = 0; // client offset of the ready-for-gameplay byte
// UI client bytes the game keeps for the local player (evidence: C:\temp\enshrouded-re\out\shroud):
// its fog type (keen::FogType: 0 none, 1 in fog but safe, 2 Shroud, 3 deadly Shroud), which the
// Shroud timer bar reads, and its IsInBigDungeon state (inside the Hollow Halls).
int32_t g_fogType = 0, g_bigDungeon = 0;

int ReadAreaReady(void* gameClient) {
    if (!g_areaReady || !gameClient) return -1;
    __try {
        return at<uint8_t>(gameClient, size_t(g_areaReady)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// Streaming shows up as long frames; the area counts as settled once frames stay short.
// "Long" is against the game's usual frame time, so a low frame rate (a slow PC, or the
// game in the background) does not count every frame as a hitch: a frame is a hitch when
// it takes over 45 ms and over twice the usual time.
constexpr double kHitchMs = 45.0;
double g_typicalFrameMs = 0.0;

void ShowTravelCover(uint32_t tint, const char* destination) {
    std::string caption;
    if (destination && *destination) {
        char line[160]{};
        std::snprintf(line, sizeof(line), portal::text::Get(portal::render::Language(), portal::text::TravellingTo), destination);
        caption = line;
    }
    portal::render::SetLoading(true, tint, caption);
}
void BeginArrival(const portal::FixedPos& target, uint32_t tint, const char* destination) {
    uint64_t now = GetTickCount64();
    g_arrival = {now, 0, 0, 0, 0, false, float(double(target.x) / portal::kFixedScale),
                 float(double(target.y) / portal::kFixedScale), float(double(target.z) / portal::kFixedScale)};
    ShowTravelCover(tint, destination); // already opaque after a departure capture
    portal::casting::LockMovement(now + kArrivalTimeoutMs, true);
}

// The snapshot shown by the portal that leads here (portal_scene_probe RequestSnapshot), taken
// while the loading screen hides the frame. Keep the cover until the camera has
// returned, including after a failed capture; never reveal it at an arbitrary 1.5 s.
bool ArrivalSnapshotDone(uint64_t now) {
    if (!g_arrival.wantSnapshot) return true;
    if (!g_arrival.snapshotAt) {
        if (portal::sceneprobe::SnapshotBusy()) return false; // the departure snapshot first
        if (!portal::sceneprobe::RequestSnapshot(g_arrival.snapshotEnd, g_arrival.snapshotDistance, now)) return true;
        if (portal::homecache::Valid(g_arrival.home)) portal::sceneprobe::MarkHomeSnapshot(g_arrival.home);
        g_arrival.snapshotAt = now;
        return false;
    }
    if (now - g_arrival.snapshotAt > portal::snapshot::kCaptureLimitMs)
        portal::sceneprobe::CancelSnapshot();
    return !portal::sceneprobe::SnapshotBusy();
}

// Cover first, capture while the player still keeps this area resident, then
// submit travel. A far destination must never unload the source before capture.
void QueuePortalTravel(const portal::PortalInstance& p, bool field, const portal::FixedPos& target,
                       uint32_t tint, const char* destination, float px, float pz, bool shroud) {
    if (g_departure.gate.Active() || g_arrival.since || portal::sceneprobe::SnapshotBusy()) return;
    const uint64_t now = GetTickCount64();
    const auto& end = field ? p.fieldEnd : p.baseEnd;
    g_departure = {};
    g_departure.portal = p; g_departure.field = field; g_departure.target = target;
    g_departure.tint = tint; g_departure.destination = destination ? destination : "";
    g_departure.snapshotEnd = field ? end.Turned() : end;
    const float along = (px - end.wx) * end.nx + (pz - end.wz) * end.nz;
    g_departure.snapshotDistance = field ? 0.4f : portal::sceneprobe::HidesPlayer() ? kAltarCamera : std::clamp(along + 0.8f, 1.2f, 3.8f);
    g_departure.shroud = shroud;
    g_departure.gate.Begin(now);
    // A field end in a Hollow Halls dungeon shows at its entrance (the dungeon lies at the map's edge).
    float fieldX = p.fieldEnd.wx, fieldZ = p.fieldEnd.wz;
    portal::names::HollowHallsEntrance(p.fieldEnd.wx, p.fieldEnd.wy, p.fieldEnd.wz, fieldX, fieldZ);
    portal::render::SetLoadingRoute(fieldX, fieldZ, p.baseEnd.wx, p.baseEnd.wz, field);
    ShowTravelCover(tint, destination);
    portal::casting::LockMovement(now + portal::snapshot::kDepartureLimitMs + kArrivalTimeoutMs, true);
    Log("Town Portal: departure queued; keeping the source loaded until its snapshot finishes");
}

float GameSoundGain();

void CheckArrival(void* gameClient, float px, float py, float pz) {
    if (!g_arrival.since) return;
    uint64_t now = GetTickCount64();
    int areaReady = ReadAreaReady(gameClient);
    if (areaReady == 0) g_arrival.sawLoading = true;
    auto finish = [&](const char* line) {
        portal::render::SetLoading(false);
        // Still locked while the loading screen fades out (kLoadingOutMs in portal_render.cpp).
        portal::casting::LockMovement(GetTickCount64() + 650, true);
        g_arrival = {};
        Log(line);
    };
    if (now - g_arrival.since > kArrivalTimeoutMs) {
        portal::sceneprobe::CancelSnapshot();
        if (portal::sceneprobe::SnapshotBusy()) return;
        // What it was still waiting for, for bug reports.
        char line[256]{};
        float tx = px - g_arrival.x, tz = pz - g_arrival.z;
        std::snprintf(line, sizeof(line),
                      "Town Portal: arrival loading screen timed out (area flag %d, saw loading %d, %.1f m from target, "
                      "terrain ready %d, typical frame %.1f ms, smooth for %llu ms)",
                      areaReady, int(g_arrival.sawLoading), std::sqrt(tx * tx + tz * tz),
                      int(portal::travel::LocalTerrainReady(px, py, pz)), g_typicalFrameMs,
                      static_cast<unsigned long long>(g_arrival.smoothSince ? now - g_arrival.smoothSince : 0));
        finish(line);
        return;
    }
    float dx = px - g_arrival.x, dz = pz - g_arrival.z;
    if (dx * dx + dz * dz > 8.0f * 8.0f) return; // not moved yet
    if (!g_arrival.arrivedAt) g_arrival.arrivedAt = now;
    static LARGE_INTEGER frequency{}, last{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    double frameMs = last.QuadPart ? double(counter.QuadPart - last.QuadPart) * 1000.0 / double(frequency.QuadPart) : 0.0;
    last = counter;
    const bool hitch = frameMs > kHitchMs && frameMs > 2.0 * g_typicalFrameMs;
    if (frameMs > 0.0 && frameMs < 1000.0 && (!hitch || g_typicalFrameMs == 0.0))
        g_typicalFrameMs = g_typicalFrameMs == 0.0 ? frameMs : g_typicalFrameMs * 0.9 + frameMs * 0.1;
    if (hitch || !g_arrival.smoothSince) g_arrival.smoothSince = now;
    // The game's signal: it has dropped since stepping through and is back. A short pause lets
    // the first fully loaded frames reach the screen before the overlay fades.
    if (g_arrival.sawLoading) {
        if (areaReady != 1) { g_arrival.nativeReadyAt = 0; return; }
        if (!g_arrival.nativeReadyAt) g_arrival.nativeReadyAt = now;
        if (now - g_arrival.nativeReadyAt < 400 || !ArrivalSnapshotDone(now)) return;
        char line[112]{};
        std::snprintf(line, sizeof(line), "Town Portal: arrived; the game reports the area loaded after %llu ms",
                      static_cast<unsigned long long>(now - g_arrival.since));
        finish(line);
        return;
    }
    // The signal never dropped (not found, or a hop too short to reset it): give it two
    // seconds after arriving, then fall back to terrain residency and smooth frames.
    if (areaReady >= 0 && now - g_arrival.arrivedAt < 2000) return;
    if (!portal::travel::LocalTerrainReady(px, py, pz)) { g_arrival.readyAt = 0; return; }
    if (!g_arrival.readyAt) g_arrival.readyAt = now;
    // Terrain around the arrival stays resident for a second, frames have been smooth for a
    // second (no streaming hitches), and at least 1.5 s have passed since arriving.
    if (now - g_arrival.readyAt < 1000 || now - g_arrival.smoothSince < 1000 || now - g_arrival.arrivedAt < 1500) return;
    if (!ArrivalSnapshotDone(now)) return;
    char line[96]{};
    std::snprintf(line, sizeof(line), "Town Portal: arrived; loading screen held %llu ms",
                  static_cast<unsigned long long>(now - g_arrival.since));
    finish(line);
}

bool ExecutePortalTeleport(void* gameClient, const portal::FixedPos& targetFixed, uint32_t tint = 0,
                           const char* destination = nullptr) {
    if (!gameClient) return false;
    uint32_t entity = (at<uint32_t>(gameClient, 0x466b44) & 0x3fu) + 1u;
    static uint64_t remoteBusyUntil = 0;
    uint64_t now = GetTickCount64();
    if (now < remoteBusyUntil && !portal::travel::HostsWorld()) return false;
    // Local host: the travel module moves the player once the destination terrain is
    // resident. Remote: the server companion does the same on its side.
    auto sub = portal::travel::SubmitTeleport(entity, targetFixed);
    if (sub == portal::travel::Submission::rejected) return false;
    g_lastTeleportTick = now;
    portal::render::PlayPortalSound(GameSoundGain());
    BeginArrival(targetFixed, tint, destination);
    if (sub == portal::travel::Submission::remote) {
        remoteBusyUntil = now + kRemoteTravelBusyMs;
        char line[140]{};
        int len = portal::FormatTeleportRequest(line, sizeof(line), g_lastTeleportTick, targetFixed);
        if (len > 0) {
            SendHiddenLine(gameClient, {line, static_cast<size_t>(len)});
        }
    }
    return true;
}

void BroadcastPortalOpen(void* gameClient, const portal::PortalInstance& inst) {
    if (!gameClient) return;
    char line[240]{};
    int len = portal::FormatPortalSync(line, sizeof(line), inst);
    if (len > 0) {
        SendHiddenLine(gameClient, {line, static_cast<size_t>(len)});
    }
    g_lastBroadcastTick = GetTickCount64();
}

void BroadcastPortalClose(void* gameClient, uint64_t portalId, uint64_t ownerAccount) {
    if (!gameClient) return;
    char line[96]{};
    int len = portal::FormatPortalClose(line, sizeof(line), portalId, ownerAccount);
    if (len > 0) {
        SendHiddenLine(gameClient, {line, static_cast<size_t>(len)});
    }
}

// Movement is locked while the portal forms. Jump is the manual cancel;
// movement input and Escape must not collapse a forming portal.
bool PlayerInShroud(uint8_t* uiClient);

void CheckChannel(void* gameClient, float px, float py, float pz) {
    if (!g_channel.portalId) return;
    uint64_t now = GetTickCount64();
    if (now >= g_channel.until) {
        portal::render::FinishChannelSound();
        portal::sceneprobe::SetCast(0, 0, 0, 0, 0);
        g_channel = {};
        return;
    }
    // The spell runs from in front of the caster's chest, toward the portal.
    {
        portal::PortalInstance forming{};
        if (portal::travel::GetPortalByOwner(portal::travel::LocalSteamAccount(), forming) &&
            forming.portalId == g_channel.portalId) {
            float dx = forming.fieldEnd.wx - px, dz = forming.fieldEnd.wz - pz;
            const float length = std::sqrt(dx * dx + dz * dz);
            if (length > 0.1f) { dx /= length; dz /= length; }
            portal::sceneprobe::SetCast(g_channel.portalId, px + dx * 0.45f, py + 1.35f, pz + dz * 0.45f, g_castEffect);
        }
    }
    if (!portal::casting::TakeJumpAbort()) return;
    uint64_t account = portal::travel::LocalSteamAccount();
    portal::travel::ClosePortalByOwner(account, g_channel.portalId);
    BroadcastPortalClose(gameClient, g_channel.portalId, account);
    portal::casting::Cancel();
    portal::casting::UnlockMovement();
    portal::render::CancelChannelSound();
    portal::render::PlayFizzle(GameSoundGain());
    portal::sceneprobe::SetCast(0, 0, 0, 0, 0);
    g_castUntil = 0;
    g_channel = {};
    Log("Town Portal: channel aborted");
}

// Base portal placement. Every player's base portal stands around the same Flame Altar, so a
// spot is chosen from rings around it: away from the other portals' base ends and arrival
// points, with the portal's width and walk-through path clear of walls, chests and other
// props (physics and terrain rays), in sight of the altar and on ground level with it.
// Those rays only answer where this machine has the area loaded; elsewhere the spot is
// provisional and the owner re-checks it once they are near the base (CheckOwnBasePortal).
struct BaseSpot { float x = 0, y = 0, z = 0, dirX = 0, dirZ = 1; }; // dir: altar -> portal

enum class SpotCheck { blocked, unknown, clear };

SpotCheck CheckBaseSpot(float altarX, float altarY, float altarZ, BaseSpot& spot) {
    using portal::math::Visibility;
    auto context = portal::travel::GetVisibilityContext();
    bool unknown = false;
    auto clear = [&](float ax, float ay, float az, float bx, float by, float bz) {
        auto v = portal::travel::TraceVisibility(context, {ax, ay, az}, {bx, by, bz});
        if (v == Visibility::unknown) { unknown = true; return true; }
        return v == Visibility::clear;
    };
    // Ground under the portal and under the arrival point, level with the altar.
    float groundY = altarY + 0.5f;
    if (!portal::travel::RefineLocalGroundY(spot.x, groundY, spot.z)) unknown = true;
    else if (std::fabs(groundY - altarY) > 1.5f) return SpotCheck::blocked;
    else spot.y = std::max(groundY, altarY);
    const float nx = -spot.dirX, nz = -spot.dirZ, rx = -nz, rz = nx; // faces the altar
    const float arriveX = spot.x + nx * 1.9f, arriveZ = spot.z + nz * 1.9f;
    float arriveY = altarY + 0.5f;
    if (portal::travel::RefineLocalGroundY(arriveX, arriveY, arriveZ) && std::fabs(arriveY - altarY) > 1.5f)
        return SpotCheck::blocked;
    // The portal's width, and the path through it to the arrival point.
    for (float h : {0.5f, 1.4f, 2.5f})
        if (!clear(spot.x - rx * 0.95f, spot.y + h, spot.z - rz * 0.95f, spot.x + rx * 0.95f, spot.y + h, spot.z + rz * 0.95f))
            return SpotCheck::blocked;
    for (float h : {0.6f, 1.6f})
        if (!clear(spot.x - nx * 0.7f, spot.y + h, spot.z - nz * 0.7f, arriveX + nx * 0.5f, spot.y + h, arriveZ + nz * 0.5f))
            return SpotCheck::blocked;
    // In sight of the altar (from just outside the altar's own collider).
    if (!clear(altarX + spot.dirX * 1.2f, altarY + 1.2f, altarZ + spot.dirZ * 1.2f, spot.x, spot.y + 1.2f, spot.z))
        return SpotCheck::blocked;
    return unknown ? SpotCheck::unknown : SpotCheck::clear;
}

// How crowded a spot is: rays out from the portal's middle in 8 directions at knee and chest
// height, each costing 3 when something is within 1.25 m and 1 when within 2.5 m. Chests,
// workbenches and beds standing there would compete with the portal for E. 0 is open ground;
// unloaded areas count as open.
int SpotCrowding(const BaseSpot& spot) {
    using portal::math::Visibility;
    auto context = portal::travel::GetVisibilityContext();
    auto hit = [&](float h, float dx, float dz, float reach) {
        return portal::travel::TraceVisibility(context, {spot.x, spot.y + h, spot.z},
                                               {spot.x + dx * reach, spot.y + h, spot.z + dz * reach}) == Visibility::blocked;
    };
    int cost = 0;
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.7853982f, dx = std::cos(a), dz = std::sin(a);
        for (float h : {0.5f, 1.3f})
            if (hit(h, dx, dz, 1.25f)) cost += 3;
            else if (hit(h, dx, dz, 2.5f)) cost += 1;
    }
    return cost;
}

// Rings at 4.4, 5.6, 7 and then 3.2 m (further out first: the arrival snapshot's camera stands
// 2.6 m in front of the portal, and nearer than that to the altar it sees only the altar up
// close), every 30 degrees, nearest the preferred direction first. Of the spots proven clear,
// the least crowded wins (the first of equals, so open ground in the preferred direction is
// kept); otherwise the first one that is only unconfirmed. False when every spot is blocked or
// crowded by portals (the caller keeps the preferred spot). crowding: the chosen spot's.
bool ChooseBaseSpot(float altarX, float altarY, float altarZ, float preferX, float preferZ,
                    float fieldX, float fieldZ, uint64_t ownerAccount, BaseSpot& out, bool needClear,
                    int* crowding = nullptr) {
    std::vector<portal::PortalInstance> portals;
    portal::travel::GetActivePortals(portals);
    BaseSpot fallback{}, best{};
    bool haveFallback = false;
    int bestCrowding = -1;
    const float preferAngle = std::atan2(preferZ, preferX);
    for (int step = 0; step < 12; ++step) {
        // 0, +30, -30, +60, -60 ... degrees from the preferred direction.
        float offset = float((step + 1) / 2) * 0.5235988f * ((step & 1) ? 1.0f : -1.0f);
        for (float radius : {4.4f, 5.6f, 7.0f, 3.2f}) {
            BaseSpot spot{};
            spot.dirX = std::cos(preferAngle + offset);
            spot.dirZ = std::sin(preferAngle + offset);
            spot.x = altarX + spot.dirX * radius;
            spot.z = altarZ + spot.dirZ * radius;
            spot.y = altarY;
            // Clear of the owner's own field end too (a portal opened next to the altar).
            const float ownDx = spot.x - fieldX, ownDz = spot.z - fieldZ;
            bool crowded = ownDx * ownDx + ownDz * ownDz < 6.0f * 6.0f;
            for (const auto& p : portals) {
                if (p.closing || p.ownerAccount == ownerAccount) continue;
                const auto& e = p.baseEnd;
                const float ax = e.wx + e.nx * 1.9f, az = e.wz + e.nz * 1.9f;
                const float myArriveX = spot.x - spot.dirX * 1.9f, myArriveZ = spot.z - spot.dirZ * 1.9f;
                auto tooClose = [](float x0, float z0, float x1, float z1) { return (x0 - x1) * (x0 - x1) + (z0 - z1) * (z0 - z1) < 2.8f * 2.8f; };
                if (tooClose(spot.x, spot.z, e.wx, e.wz) || tooClose(spot.x, spot.z, ax, az) || tooClose(myArriveX, myArriveZ, e.wx, e.wz)) {
                    crowded = true;
                    break;
                }
            }
            if (crowded) continue;
            auto check = CheckBaseSpot(altarX, altarY, altarZ, spot);
            if (check == SpotCheck::clear) {
                const int cost = SpotCrowding(spot);
                if (bestCrowding < 0 || cost < bestCrowding) { best = spot; bestCrowding = cost; }
                if (cost == 0) break;
            }
            if (check == SpotCheck::unknown && !haveFallback) { fallback = spot; haveFallback = true; }
        }
        if (bestCrowding == 0) break;
    }
    if (bestCrowding >= 0) {
        out = best;
        if (crowding) *crowding = bestCrowding;
        return true;
    }
    if (needClear || !haveFallback) return false;
    out = fallback;
    if (crowding) *crowding = 0;
    return true;
}

// Once the owner is near the base, the spot can be judged: keep it if clear and open, move it
// if it turned out to be in a wall, a chest or another player's portal, or crowded by objects
// when a more open spot by the altar exists; wait while still unknown.
uint64_t g_baseChecked = 0;

void CheckOwnBasePortal(void* gameClient, portal::PortalInstance& p) {
    BaseSpot spot{p.baseEnd.wx, p.baseEnd.wy, p.baseEnd.wz, -p.baseEnd.nx, -p.baseEnd.nz};
    auto check = CheckBaseSpot(p.altarX, p.altarY, p.altarZ, spot);
    if (check == SpotCheck::unknown) return;
    g_baseChecked = p.portalId;
    const int crowding = check == SpotCheck::clear ? SpotCrowding(spot) : -1;
    if (crowding == 0) return;
    BaseSpot better{};
    int betterCrowding = 0;
    if (!ChooseBaseSpot(p.altarX, p.altarY, p.altarZ, spot.dirX, spot.dirZ, p.fieldEnd.wx, p.fieldEnd.wz, p.ownerAccount, better, true, &betterCrowding)) {
        if (crowding < 0) Log("Town Portal: base portal spot is blocked and no clear spot was found around the altar");
        return;
    }
    if (crowding >= 0 && betterCrowding >= crowding) return; // already the most open spot
    p.baseEnd.wx = better.x;
    p.baseEnd.wy = better.y;
    p.baseEnd.wz = better.z;
    p.baseEnd.SyncFixedFromFloat();
    p.baseEnd.SetNormal(-better.dirX, -better.dirZ);
    portal::travel::UpsertPortal(p);
    BroadcastPortalOpen(gameClient, p);
    char line[128];
    if (crowding < 0) std::snprintf(line, sizeof(line), "Town Portal: base portal moved to a clear spot by the altar (crowding %d)", betterCrowding);
    else std::snprintf(line, sizeof(line), "Town Portal: base portal moved away from nearby objects (crowding %d -> %d)", crowding, betterCrowding);
    Log(line);
}

// Field portal placement: the portal stands `distance` ahead of the player, facing them. It must
// not stand in a wall or stick through one: clear from the player to just past the portal,
// across its width, up to its top, and out of its back face at both edges (the lens and halo
// stick out behind it). Unknown (no collision data there) counts as clear: the player is there.
SpotCheck CheckFieldSpot(float px, float py, float pz, float dirX, float dirZ, float distance, float& outY,
                         const char** why = nullptr) {
    using portal::math::Visibility;
    auto context = portal::travel::GetVisibilityContext();
    bool unknown = false;
    auto clear = [&](float ax, float ay, float az, float bx, float by, float bz) {
        auto v = portal::travel::TraceVisibility(context, {ax, ay, az}, {bx, by, bz});
        if (v == Visibility::unknown) { unknown = true; return true; }
        return v == Visibility::clear;
    };
    const float cx = px + dirX * distance, cz = pz + dirZ * distance, rx = -dirZ, rz = dirX;
    float y = py;
    // Without the ground there (the terrain lookup sometimes fails for a moment), its height
    // is a guess: a rise of half a metre would meet the shin-height tests, so they are skipped.
    const bool ground = portal::travel::RefineLocalGroundY(cx, y, cz);
    if (!ground) unknown = true;
    else if (std::fabs(y - py) > 1.5f) { if (why) *why = "ground height"; return SpotCheck::blocked; }
    outY = y;
    // From just outside the player's own collider to just behind the portal.
    for (float h : {0.6f, 1.6f})
        if ((ground || h > 1.0f) && !clear(px + dirX * 0.5f, py + h, pz + dirZ * 0.5f, cx + dirX * 0.45f, y + h, cz + dirZ * 0.45f))
            { if (why) *why = h < 1.0f ? "path low" : "path high"; return SpotCheck::blocked; }
    // Its width and height: an upright oval 1.9 m across the middle and about 3.2 m tall, so
    // near the ground and the top it is narrower (a full-width test at shin height refused
    // portals on paths between grass banks and on side slopes).
    for (auto [h, half] : {std::pair{0.5f, 0.7f}, std::pair{1.6f, 0.95f}, std::pair{2.6f, 0.75f}})
        if ((ground || h > 1.0f) && !clear(cx - rx * half, y + h, cz - rz * half, cx + rx * half, y + h, cz + rz * half))
            { if (why) *why = h < 1.0f ? "width low" : h < 2.0f ? "width middle" : "width top"; return SpotCheck::blocked; }
    if (!clear(cx, y + 0.3f, cz, cx, y + 3.1f, cz)) { if (why) *why = "height"; return SpotCheck::blocked; }
    for (float side : {-0.8f, 0.8f})
        if (!clear(cx + rx * side, y + 1.6f, cz + rz * side, cx + rx * side + dirX * 0.45f, y + 1.6f, cz + rz * side + dirZ * 0.45f))
            { if (why) *why = side < 0.0f ? "back left" : "back right"; return SpotCheck::blocked; }
    return unknown ? SpotCheck::unknown : SpotCheck::clear;
}

// The server rules in force (portal_rules.h): this machine's town-portal.ini when it hosts
// the world, the host's otherwise.
portal::rules::Rules CurrentRules() { return portal::rules::Current(portal::travel::HostsWorld()); }

// Ask the host for its rules after joining and every 20 s after (edits to its file apply
// without rejoining). A host that predates them never answers; the defaults stay.
void AskHostRules(void* gameClient, void* currentWorld) {
    static void* world = nullptr;
    static uint64_t askAt = 0;
    const uint64_t now = GetTickCount64();
    if (world != currentWorld) {
        world = currentWorld;
        askAt = now + 3000;
        portal::rules::ForgetHost();
    }
    if (!currentWorld || portal::travel::HostsWorld() || now < askAt) return;
    askAt = now + 20000;
    SendHiddenLine(gameClient, portal::rules::kAsk);
}

// The game's own volumes, from its settings file (Steam\userdata\<account>\1203620\remote\
// user_settings.json, 0..1; the game rewrites it when the settings change): Master Volume
// times Player Voice for the character's voice, times Game for sound effects. Read again at
// most once a second; 1 when it cannot be read.
struct GameVolumes { float voice = 1.0f, game = 1.0f; };
GameVolumes ReadGameVolumes() {
    static GameVolumes volumes;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 1000) return volumes;
    readAt = now;
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS)
        return volumes;
    const uint64_t account = portal::travel::LocalSteamAccount() & 0xffffffffull;
    if (!account) return volumes;
    wchar_t path[MAX_PATH * 2]{};
    swprintf_s(path, L"%s\\userdata\\%llu\\1203620\\remote\\user_settings.json", steam, static_cast<unsigned long long>(account));
    FILE* f = _wfopen(path, L"rb");
    if (!f) return volumes;
    std::string text(32768, '\0');
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    // Values are quoted: "0" and "1" as numbers, anything else as the float's bits in hex
    // ("masterVolume": "3e9bacf9" = 0.304).
    auto value = [&](const char* key) {
        const size_t at = text.find(key);
        if (at == std::string::npos) return 1.0f;
        const size_t open = text.find('"', text.find(':', at + std::strlen(key)));
        if (open == std::string::npos) return 1.0f;
        const size_t close = text.find('"', open + 1);
        if (close == std::string::npos) return 1.0f;
        const std::string token = text.substr(open + 1, close - open - 1);
        float v = 1.0f;
        if (token.size() == 8 && token.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
            const uint32_t bits = uint32_t(std::strtoul(token.c_str(), nullptr, 16));
            std::memcpy(&v, &bits, sizeof(v));
        } else {
            v = float(std::atof(token.c_str()));
        }
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
    };
    const float master = value("\"masterVolume\"");
    volumes = {master * value("\"voiceVolume\""), master * value("\"gameVolume\"")};
    return volumes;
}
float PlayerVoiceGain() { return ReadGameVolumes().voice; }

// The voice the local character speaks with (portal_voicepack.h): from the newest
// characterN.ksd beside user_settings.json, the character being played. Read again at most
// every 10 s; nullptr when unknown.
const char* PlayerVoiceName() {
    static int voice = -1;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 10000) return portal::voicepack::ShippedVoice(voice);
    readAt = now;
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    const uint64_t account = portal::travel::LocalSteamAccount() & 0xffffffffull;
    if (!account || RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size) != ERROR_SUCCESS)
        return portal::voicepack::ShippedVoice(voice);
    wchar_t folder[MAX_PATH * 2]{}, pattern[MAX_PATH * 2]{};
    swprintf_s(folder, L"%s\\userdata\\%llu\\1203620\\remote\\", steam, static_cast<unsigned long long>(account));
    swprintf_s(pattern, L"%scharacter*.ksd", folder);
    WIN32_FIND_DATAW found{};
    std::wstring newest;
    FILETIME newestTime{};
    if (HANDLE find = FindFirstFileW(pattern, &found); find != INVALID_HANDLE_VALUE) {
        do {
            if (newest.empty() || CompareFileTime(&found.ftLastWriteTime, &newestTime) > 0) {
                newest = found.cFileName;
                newestTime = found.ftLastWriteTime;
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (newest.empty()) return portal::voicepack::ShippedVoice(voice);
    if (FILE* f = _wfopen((std::wstring(folder) + newest).c_str(), L"rb")) {
        std::vector<uint8_t> save(1 << 16);
        save.resize(std::fread(save.data(), 1, save.size(), f));
        std::fclose(f);
        const int read = portal::voicepack::VoiceIndex(portal::voicepack::FromSave(save.data(), save.size()));
        if (read >= 0 && read != voice) {
            char line[96]{};
            std::snprintf(line, sizeof(line), "Town Portal: the character speaks with voice %s (lines in %s)",
                          portal::voicepack::kNames[read], portal::voicepack::ShippedVoice(read));
            Log(line);
        }
        if (read >= 0) voice = read;
    }
    return portal::voicepack::ShippedVoice(voice);
}

// Refusals in a row: each within a minute of the last, with no portal opened in between.
struct RefusalStreak { int count = 0; uint64_t lastAt = 0; };
RefusalStreak g_refusals;

// The character refusing a cast: notice, voice line (VoiceFeedback in town-portal.ini) and shrug.
// The 5th, 10th and 15th refusal in a row get a joke instead of the reason.
void RefuseCast(void* gameClient, const char* line, const char* notice, uint64_t durationMs) {
    const uint64_t now = GetTickCount64();
    g_refusals.count = g_refusals.lastAt && now - g_refusals.lastAt <= 60000 ? g_refusals.count + 1 : 1;
    g_refusals.lastAt = now;
    static constexpr struct { int count; const char* line; portal::text::Id text; } kJokes[] = {
        {5, "nag_5", portal::text::Nag5}, {10, "nag_10", portal::text::Nag10}, {15, "nag_15", portal::text::Nag15}};
    for (const auto& joke : kJokes) {
        if (g_refusals.count != joke.count) continue;
        line = joke.line;
        notice = portal::text::Get(portal::render::Language(), joke.text);
        durationMs = 5000;
    }
    portal::render::ShowNotice("Town Portal", notice, durationMs);
    if (portal::rules::VoiceFeedback()) portal::render::PlayVoiceLine(line, PlayerVoiceName(), PlayerVoiceGain());
    portal::casting::Shrug(gameClient);
}
float GameSoundGain() { return ReadGameVolumes().game; }

// The game's own state for the local player (see g_fogType, g_bigDungeon); the Hollow Halls
// fall back to their area boxes when the flag was not found.
uint8_t UiByte(uint8_t* uiClient, int32_t offset) {
    __try {
        return uiClient && offset ? uiClient[offset] : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool PlayerInShroud(uint8_t* uiClient) {
    const uint8_t fog = UiByte(uiClient, g_fogType);
    return fog == 2 || fog == 3;
}
bool PlayerInHollowHalls(uint8_t* uiClient, float px, float py, float pz) {
    return (g_bigDungeon && UiByte(uiClient, g_bigDungeon)) || portal::names::InHollowHalls(px, py, pz);
}

// The home spot: where this player's base portal stands by the altar, set at login (or by
// their first cast in a world) and reused while clear, so one snapshot of the altar serves
// every portal.
struct Home {
    void* world = nullptr;
    float dirX = 0, dirZ = 0;
    bool set = false;
} g_home;


// The home snapshot cache's key: this player and their marked respawn altar (invalid when
// either is not known yet).
portal::homecache::Key HomeCacheKey(uint8_t* uiClient) {
    portal::homecache::Key key{};
    int64_t altar[3]{};
    if (!uiClient || !ReadPreferredAltar(uiClient, altar)) return key;
    key.account = portal::travel::LocalSteamAccount();
    std::memcpy(key.altar, altar, sizeof(altar));
    return key;
}

// The login snapshot: every login is at the altar, so as the world first appears a dark cover
// carries on from the game's loading screen, the area finishes loading, the home spot is
// chosen and its snapshot taken (the character hidden), then the cover fades. Field portals
// show the altar from the first cast. Also wait for spawning/particle settling;
// the cover remains bounded if readiness never arrives.
struct LoginShot {
    void* world = nullptr;
    uint64_t coverAt = 0, snapAt = 0;
    bool done = false;
    const char* waiting = nullptr; // what it was still waiting for (logged if it gives up)
    portal::snapshot::LoginSettle settle{};
    uint64_t lastActorStates = UINT64_MAX;
    // The home snapshot saved at an earlier login or trip home (portal_snapshot_cache.h):
    // when there is one for this altar it is shown instead, with no cover and no capture.
    uint64_t hudAt = 0;
    portal::homecache::Key cacheKey{};
    bool cacheChecked = false, fromCache = false, uploading = false;
    float altarX = 0, altarZ = 0;
    std::shared_ptr<portal::sharing::Image> cached;
} g_login;

// How long the play HUD may show before the cache check settles (the altar data arrives a
// moment after the world): the cover waits for it, the game is visible meanwhile.
constexpr uint64_t kLoginCacheWaitMs = 1500;

// Shows the saved home snapshot. True while it is in hand (pending or shown); false when it
// cannot be used, and the login snapshot is taken as before.
bool LoginFromCache(uint64_t now) {
    auto fail = [&](const char* why) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: saved home snapshot not used (%s); taking a new one", why);
        Log(line);
        g_login.fromCache = false;
        g_login.cached.reset();
        return false;
    };
    if (now - g_login.hudAt > portal::snapshot::kLoginLimitMs) return fail("timed out");
    if (!g_login.uploading) {
        if (!g_login.cached) {
            const int loaded = portal::homecache::TakeLoaded(g_login.cacheKey, g_login.cached);
            if (loaded == 0) return true;
            if (loaded < 0 || !g_login.cached) return fail("unreadable file");
        }
        const auto& m = g_login.cached->meta;
        const float dx = m.x - g_login.altarX, dz = m.z - g_login.altarZ;
        if (dx * dx + dz * dz > 40.f * 40.f) return fail("taken at another altar");
        if (!portal::sceneprobe::SnapshotsReady()) return true;
        g_home.world = g_lastWorld;
        g_home.dirX = -m.nx;
        g_home.dirZ = -m.nz;
        portal::sceneprobe::UploadSnapshot(std::move(g_login.cached), "saved home snapshot");
        g_login.uploading = true;
        return true;
    }
    const int shown = portal::sceneprobe::UploadSnapshot(nullptr);
    if (shown == 0) return true;
    if (shown < 0) { g_login.uploading = false; return fail("upload failed"); }
    g_home.set = true;
    g_login.done = true;
    Log("Town Portal: home spot and its snapshot restored from the last session");
    return true;
}

void CheckLoginSnapshot(void* gameClient, uint8_t* uiClient, float px, float py, float pz, bool inPlayHud) {
    const uint64_t now = GetTickCount64();
    if (g_login.world != g_lastWorld) g_login = {g_lastWorld};
    if (g_login.done) return;
    if (!g_login.cacheChecked) {
        const auto key = HomeCacheKey(uiClient);
        if (portal::homecache::Valid(key)) {
            g_login.cacheChecked = true;
            g_login.cacheKey = key;
            g_login.altarX = static_cast<float>(static_cast<double>(key.altar[0]) * portal::kInvFixedScale);
            g_login.altarZ = static_cast<float>(static_cast<double>(key.altar[2]) * portal::kInvFixedScale);
            if (portal::homecache::Exists(key)) {
                portal::homecache::Load(key);
                g_login.fromCache = true;
                Log("Town Portal: loading the saved home snapshot for this altar");
            }
        }
    }
    if (!inPlayHud) { g_login.settle = {}; return; }
    auto finish = [&] { portal::render::SetBlink(false); g_login.done = true; };
    if (!portal::sceneprobe::HidesPlayer()) { g_login.done = true; return; }
    if (!g_login.hudAt) g_login.hudAt = now;
    if (!g_login.coverAt) {
        if (!g_login.cacheChecked && now - g_login.hudAt < kLoginCacheWaitMs) return;
        if (g_login.fromCache && LoginFromCache(now)) return;
        if (g_login.done) return;
        g_login.coverAt = now;
        portal::render::SetBlink(true, 0xff100805u, true);
        return;
    }
    if (now - g_login.coverAt > portal::snapshot::kLoginLimitMs) {
        portal::sceneprobe::CancelSnapshot();
        if (portal::sceneprobe::SnapshotBusy()) return;
        finish();
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: login snapshot skipped (%s)", g_login.waiting ? g_login.waiting : "timed out");
        Log(line);
        return;
    }
    if (g_login.snapAt) {
        if (!portal::sceneprobe::SnapshotBusy()) { finish(); Log("Town Portal: home spot chosen; login snapshot attempt finished"); }
        return;
    }
    // The area loaded (the game's own signal, else terrain around the player) and settled.
    // Until each of these is ready, wait within the login deadline: the world, images and
    // the altar data all arrive a moment after the world first shows.
    const int areaReady = ReadAreaReady(gameClient);
    uint64_t actorStates{};
    const bool actorRead = portal::casting::SnapshotActorStates(gameClient, actorStates);
    const bool actorReady = actorRead && portal::snapshot::ActorReadyForSnapshot(actorStates);
    const bool terrainReady = areaReady >= 0 ? areaReady == 1 : portal::travel::LocalTerrainReady(px, py, pz);
    const bool settled = g_login.settle.Frame(now, terrainReady && actorReady);
    if (actorStates != g_login.lastActorStates) {
        g_login.lastActorStates = actorStates;
        char line[160]{};
        std::snprintf(line,sizeof(line),"Town Portal: login snapshot actor states=0x%llx, readable=%d, spawn-ready=%d",
            static_cast<unsigned long long>(actorStates),actorRead,actorReady);
        Log(line);
    }
    g_login.waiting = "area loading";
    if (!terrainReady) return;
    g_login.waiting = actorReady ? "spawn effects settling" : "player spawning";
    if (!settled) return;
    g_login.waiting = "snapshot images";
    if (!portal::sceneprobe::SnapshotsReady()) return;
    portal::FixedPos altarFixed{};
    float ax = 0, ay = 0, az = 0;
    g_login.waiting = "Flame Altar not found";
    if (!FindPreferredSpawnAltar(uiClient, px, pz, altarFixed, ax, ay, az)) return;
    float dx = px - ax, dz = pz - az, distance = std::sqrt(dx * dx + dz * dz);
    g_login.waiting = "not at the Flame Altar";
    if (distance > 40.0f || std::fabs(py - ay) > 10.0f) return;
    if (distance < 0.5f) { dx = 0; dz = 1; distance = 1; }
    BaseSpot spot{};
    if (!ChooseBaseSpot(ax, ay, az, dx / distance, dz / distance, 1e6f, 1e6f, portal::travel::LocalSteamAccount(), spot, false)) {
        spot.dirX = dx / distance;
        spot.dirZ = dz / distance;
        spot.x = ax + spot.dirX * 4.4f;
        spot.z = az + spot.dirZ * 4.4f;
        spot.y = ay;
    }
    float baseY = spot.y;
    portal::travel::RefineLocalGroundY(spot.x, baseY, spot.z);
    if (baseY < ay) baseY = ay;
    portal::PortalEndpoint end{};
    end.wx = spot.x;
    end.wy = baseY;
    end.wz = spot.z;
    end.SyncFixedFromFloat();
    end.SetNormal(-spot.dirX, -spot.dirZ);
    if (!portal::sceneprobe::RequestSnapshot(end, kAltarCamera, now, PlayerInShroud(uiClient))) { finish(); return; }
    portal::sceneprobe::MarkHomeSnapshot(HomeCacheKey(uiClient));
    g_home.world = g_lastWorld;
    g_home.dirX = spot.dirX;
    g_home.dirZ = spot.dirZ;
    g_home.set = true;
    g_login.snapAt = now;
}

// Day or night by the game's clock (the map mod's minimap clock: the UI client's daytime
// state, time of day and sunrise / sunset in nanoseconds). Day when unreadable.
bool IsDaytime(uint8_t* uiClient) {
    __try {
        auto* daytime = *reinterpret_cast<uint8_t**>(uiClient + 0x466ff0);
        if (!daytime) return true;
        constexpr int64_t kDay = 86400000000000LL;
        int64_t now = *reinterpret_cast<int64_t*>(daytime + 0x48);
        int64_t sunrise = *reinterpret_cast<int64_t*>(daytime + 0x28);
        int64_t sunset = *reinterpret_cast<int64_t*>(daytime + 0x30);
        if (now < 0 || now > kDay * 2) return true;
        now %= kDay;
        if (sunrise <= 0 || sunrise >= kDay) sunrise = 6LL * 3600 * 1000000000LL;
        if (sunset <= 0 || sunset >= kDay) sunset = 20LL * 3600 * 1000000000LL;
        return now >= sunrise && now < sunset;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// Default pictures (portal_default_images.h): an altar-side portal with nothing captured of
// its field end gets a landmark of that end's region, by day or night, one at a time. Once
// shown it is the portal's snapshot until a real capture of that place replaces it.
struct DefaultPicture {
    int stage = 0; // 0 idle, 1 loading the picture, 2 uploading it
    portal::PortalEndpoint destination{};
    int region = -1;
    bool night = false;
    uint64_t since = 0, retryAt = 0, shownAt = 0;
    char what[64]{};
} g_defaultPicture;

void CheckDefaultPicture(uint8_t* uiClient, uint64_t now) {
    auto& d = g_defaultPicture;
    auto retry = [&](uint64_t ms) { d.stage = 0; d.retryAt = now + ms; };
    if (d.stage == 1) {
        std::shared_ptr<portal::sharing::Image> image;
        const int got = portal::defaults::Take(d.region, d.night, image);
        if (got == 0 && now - d.since < 5000) return;
        const float exposure = portal::sceneprobe::FrameExposure();
        if (got != 1 || !image || !(exposure > 0)) { retry(30000); return; }
        portal::defaultview::Place(d.destination.wx, d.destination.wy, d.destination.wz,
                                   d.destination.nx, d.destination.nz, image->meta);
        // As if captured in this frame: the portal scales a snapshot by its exposure over the
        // frame's, so the picture shows at its own brightness by day and by night.
        image->meta.exposure = exposure;
        portal::sceneprobe::UploadSnapshot(std::move(image), d.what);
        d.stage = 2;
        d.since = now;
        return;
    }
    if (d.stage == 2) {
        const int shown = portal::sceneprobe::UploadSnapshot(nullptr);
        if (shown == 0 && now - d.since < 5000) return;
        // Portals drawn before it was shown still report it missing: only later frames count.
        if (shown != 1) retry(10000); else { d.stage = 0; d.shownAt = now; }
        return;
    }
    // The saved home snapshot uses the same upload at login.
    if (now < d.retryAt || (g_login.fromCache && !g_login.done)) return;
    uint64_t portalId = 0;
    portal::PortalEndpoint destination{};
    if (!portal::sceneprobe::MissingSnapshot(portalId, destination, d.shownAt)) return;
    // In a Hollow Halls dungeon: the region of its entrance.
    float regionX = destination.wx, regionZ = destination.wz;
    portal::names::HollowHallsEntrance(destination.wx, destination.wy, destination.wz, regionX, regionZ);
    const int region = portal::names::RegionAt(regionX, regionZ);
    if (region < 0) { retry(30000); return; }
    d.destination = destination;
    d.region = region;
    d.night = !IsDaytime(uiClient);
    d.since = now;
    d.stage = 1;
    static constexpr const char* kNames[portal::defaults::kRegions] = {"Springlands", "Revelwood", "Blackmire",
        "Nomad Highlands", "Kindlewastes", "Albaneve Summits", "Veilwater Basin", "Cinder's Cradle"};
    std::snprintf(d.what, sizeof(d.what), "default %s %s picture", kNames[region], d.night ? "night" : "day");
    portal::defaults::Request(region, d.night);
}

// The loading screen's percentage: an estimate from the trip's stages, each eased toward its
// end so it keeps moving: the departure snapshot (to 15%), the destination loading (to 85%,
// slowing as it goes), the game reporting the area loaded (88%), the arrival snapshot (95%);
// leaving the screen shows 100%.
void UpdateLoadingProgress(uint64_t now) {
    auto ease = [](uint64_t ms, float scale) { return 1.0f - std::exp(-float(ms) / scale); };
    float progress = 0;
    if (g_departure.gate.Active())
        progress = 0.15f * ease(now - g_departure.gate.started, 700.0f);
    else if (g_arrival.since) {
        progress = 0.15f + 0.70f * ease(now - g_arrival.since, 4000.0f);
        if (g_arrival.nativeReadyAt) progress = std::max(progress, 0.88f);
        if (g_arrival.snapshotAt) progress = std::max(progress, 0.95f);
    } else return;
    portal::render::SetLoadingProgress(progress);
}

void TryOpenPortalAtPlayer(void* gameClient, uint8_t* uiClient,
                           float px, float py, float pz, float dirX, float dirZ) {
    if(GetTickCount64()<g_castUntil)return;
    if (!portal::casting::MovementLockReady()) {
        Log("Town Portal: cast refused because the movement lock is unavailable");
        return;
    }
    const auto rules = CurrentRules();
    if (!rules.allowInHollowHalls) {
        if (PlayerInHollowHalls(uiClient, px, py, pz)) {
            char name[96]{}, line[200]{};
            portal::names::HollowHallsName(px, py, pz, name, sizeof(name));
            std::snprintf(line, sizeof(line), portal::text::Get(portal::render::Language(), portal::text::NoPortalHere), name);
            RefuseCast(gameClient, "hollow_halls", line, 3000);
            Log("Town Portal: portals are not allowed in the Hollow Halls (town-portal.ini)");
            return;
        }
    }
    if (!rules.allowInShroud && PlayerInShroud(uiClient)) {
        RefuseCast(gameClient, "shroud", portal::text::Get(portal::render::Language(), portal::text::NoPortalShroud), 3000);
        Log("Town Portal: portals are not allowed in the Shroud (town-portal.ini)");
        return;
    }
    portal::FixedPos altarFixed{};
    float altarX = 0.0f, altarY = 0.0f, altarZ = 0.0f;
    if (!FindPreferredSpawnAltar(uiClient, px, pz, altarFixed, altarX, altarY, altarZ)) {
        portal::render::ShowNotice("Town Portal", "Could not locate a Preferred Spawn Flame Altar.", 3500);
        Log("Town Portal: no preferred spawn Flame Altar found");
        return;
    }

    // A new portal replaces the caster's old one, wherever it was (UpsertPortal by owner).
    uint64_t localAccount = portal::travel::LocalSteamAccount();

    // 1. Place Field Portal 2.8m in front of the player, facing back toward the player; nearer
    // (down to 2.2 m, where travelling back still arrives between the portal and here) when a
    // wall is in the way, and not at all when there is no room.
    float fieldDistance = 0.0f, fieldY = py;
    for (float distance : {2.8f, 2.5f, 2.2f}) {
        float y = py;
        const char* why = "";
        if (CheckFieldSpot(px, py, pz, dirX, dirZ, distance, y, &why) == SpotCheck::blocked) {
            char line[160];
            std::snprintf(line, sizeof(line), "Town Portal: no room %.1f m ahead (%s; ground %.2f%s, player %.2f)", distance, why, y,
                          y == py ? " (not found)" : "", py);
            Log(line);
            continue;
        }
        fieldDistance = distance;
        fieldY = y;
        break;
    }
    if (fieldDistance <= 0.0f) {
        RefuseCast(gameClient, "no_room", portal::text::Get(portal::render::Language(), portal::text::NoRoom), 3000);
        Log("Town Portal: no room for a portal in front of the player");
        return;
    }
    g_refusals = {};
    float fieldX = px + dirX * fieldDistance;
    float fieldZ = pz + dirZ * fieldDistance;

    // 2. Place Base Portal 3.2m from the Preferred Spawn Flame Altar, facing the Flame Altar!
    // If the Field Portal was opened close to the Altar (< 9m), place the Base Portal on the opposite
    // side of the Altar so the two portals are well separated and never overlap.
    float toFieldX = fieldX - altarX;
    float toFieldZ = fieldZ - altarZ;
    float toFieldLen = std::sqrt(toFieldX * toFieldX + toFieldZ * toFieldZ);
    if (toFieldLen > 1e-3f) {
        toFieldX /= toFieldLen;
        toFieldZ /= toFieldLen;
    } else {
        toFieldX = 0.0f;
        toFieldZ = 1.0f;
    }
    float baseSideSign = (toFieldLen < 9.0f) ? -1.0f : 1.0f;
    float baseDirX = toFieldX * baseSideSign;
    float baseDirZ = toFieldZ * baseSideSign;
    // The home spot, where its snapshot was taken (CheckHomeSnapshot), while it is clear.
    if (g_home.set && g_home.world == g_lastWorld) {
        baseDirX = g_home.dirX;
        baseDirZ = g_home.dirZ;
    }
    float baseX = altarX + baseDirX * 3.2f;
    float baseY = altarY;
    float baseZ = altarZ + baseDirZ * 3.2f;
    BaseSpot spot{};
    int crowding = 0;
    if (ChooseBaseSpot(altarX, altarY, altarZ, baseDirX, baseDirZ, fieldX, fieldZ, localAccount, spot, false, &crowding)) {
        baseX = spot.x; baseY = spot.y; baseZ = spot.z;
        baseDirX = spot.dirX; baseDirZ = spot.dirZ;
        char line[128];
        std::snprintf(line, sizeof(line), "Town Portal: altar-side spot %.1f m from the altar, crowding %d",
                      std::hypot(baseX - altarX, baseZ - altarZ), crowding);
        Log(line);
    }
    // The first cast in this world sets the home spot: later casts use it while it is clear,
    // so one trip's snapshot of it serves every portal after.
    if (!g_home.set && g_home.world == g_lastWorld) {
        g_home.dirX = baseDirX;
        g_home.dirZ = baseDirZ;
        g_home.set = true;
    }
    portal::travel::RefineLocalGroundY(baseX, baseY, baseZ);
    if (baseY < altarY) {
        baseY = altarY;
    }
    g_baseChecked = 0; // re-checked once the owner is near the base

    uint64_t now = GetTickCount64();
    portal::PortalInstance inst{};
    inst.active = true;
    inst.closing = false;
    inst.ownerVisitedBase = false;
    inst.portalId = now;
    inst.ownerAccount = localAccount;
    inst.ownerPlayerId = *reinterpret_cast<uint32_t*>(uiClient + 0x18);
    inst.openedTick = now;
    inst.lastSyncTick = now;

    inst.fieldEnd.wx = fieldX;
    inst.fieldEnd.wy = fieldY;
    inst.fieldEnd.wz = fieldZ;
    inst.fieldEnd.SyncFixedFromFloat();
    inst.fieldEnd.SetNormal(-dirX, -dirZ); // Faces the player who opened it

    inst.baseEnd.wx = baseX;
    inst.baseEnd.wy = baseY;
    inst.baseEnd.wz = baseZ;
    inst.baseEnd.SyncFixedFromFloat();
    inst.baseEnd.SetNormal(-baseDirX, -baseDirZ); // Faces the Flame Altar!

    inst.altarPos = altarFixed;
    inst.altarX = altarX;
    inst.altarY = altarY;
    inst.altarZ = altarZ;


    if(portal::casting::Start(gameClient))g_castUntil=now+portal::casting::DurationMs();
    portal::travel::UpsertPortal(inst);
    g_channel = {inst.portalId, now + portal::kPortalFormMs, px, py, pz};
    portal::casting::LockMovement(g_channel.until);
    portal::render::StartChannelSound(GameSoundGain());
    BroadcastPortalOpen(gameClient, inst);

    char logBuf[192]{};
    std::snprintf(logBuf, sizeof(logBuf),
                  "Town Portal: opened field=(%.1f, %.1f, %.1f) <-> altar=(%.1f, %.1f, %.1f)",
                  fieldX, fieldY, fieldZ, baseX, baseY, baseZ);
    Log(logBuf);
}

void CheckDeparture(void* gameClient, uint8_t* uiClient, bool areaReady, float px, float py, float pz) {
    if (!g_departure.gate.Active()) return;
    const uint64_t now = GetTickCount64();
    const auto action = g_departure.gate.Poll(now, areaReady, portal::sceneprobe::SnapshotBusy(),
                                             portal::sceneprobe::SnapshotsReady());
    using Action = portal::snapshot::DepartureAction;
    if (action == Action::capture) {
        if (!portal::sceneprobe::RequestSnapshot(g_departure.snapshotEnd, g_departure.snapshotDistance, now, g_departure.shroud))
            Log("Town Portal: departure capture unavailable; keeping the previous image");
        return;
    }
    if (action == Action::cancelCapture) {
        portal::sceneprobe::CancelSnapshot();
        Log("Town Portal: departure capture timed out; waiting for the camera to return");
        return;
    }
    if (action != Action::travel) return;
    const auto trip = g_departure;
    g_departure = {};
    const auto& source = trip.field ? trip.portal.fieldEnd : trip.portal.baseEnd;
    const float dx = px - source.wx, dy = py - source.wy, dz = pz - source.wz;
    portal::PortalInstance current{};
    if (dx*dx + dz*dz > 8.f*8.f || std::fabs(dy) > 8.f ||
        !portal::travel::GetPortalByOwner(trip.portal.ownerAccount, current) ||
        !current.active || current.closing || current.portalId != trip.portal.portalId ||
        !ExecutePortalTeleport(gameClient, trip.target, trip.tint, trip.destination.c_str())) {
        portal::render::SetLoading(false);
        portal::casting::LockMovement(now + 650, true);
        Log("Town Portal: queued travel cancelled; portal changed, player moved, or travel was refused");
        return;
    }
    Log("Town Portal: departure camera restored; travel submitted");
    const uint64_t localAccount = portal::travel::LocalSteamAccount();
    const bool isOwner = trip.portal.ownerAccount == localAccount;
    if (trip.field) {
        g_arrival.snapshotEnd = trip.portal.baseEnd;
        g_arrival.snapshotDistance = portal::sceneprobe::HidesPlayer() ? kAltarCamera : 2.6f;
        g_arrival.wantSnapshot = true;
        if (isOwner) g_arrival.home = HomeCacheKey(uiClient);
        if (isOwner) {
            portal::travel::MarkOwnerVisitedBase(localAccount);
            portal::PortalInstance updated{};
            if (portal::travel::GetPortalByOwner(localAccount, updated)) BroadcastPortalOpen(gameClient, updated);
        }
        Log("Town Portal: traveling from Field Portal to Base Flame Altar");
    } else if (isOwner && CurrentRules().disableAutoClose) {
        Log("Town Portal: owner returned to the field; portal stays open (DisableAutoClose)");
    } else if (isOwner && OthersInWorld(uiClient)) {
        g_pendingClose = {trip.portal.portalId, now + kCloseGraceMs};
        char line[200]{};
        std::snprintf(line, sizeof(line), portal::text::Get(portal::render::Language(), portal::text::ClosesSoon), int(kCloseGraceMs / 1000));
        portal::render::ShowNotice("Town Portal", line, 5000);
        Log("Town Portal: owner returned to the field; portal closes in 15 s");
    } else if (isOwner) {
        portal::travel::ClosePortalByOwner(localAccount, trip.portal.portalId);
        BroadcastPortalClose(gameClient, trip.portal.portalId, localAccount);
        Log("Town Portal: owner returned from Base to Field; portal closed");
    } else {
        Log("Town Portal: guest player traveled from Base Portal to Field; portal stays open");
    }
}

void ResetTravelPresentation() {
    g_departure = {};
    g_arrival = {};
    g_login = {};
    g_home = {};
    portal::sceneprobe::CancelSnapshot();
    portal::sceneprobe::SnapshotAreaReady(false, GetTickCount64());
    portal::render::SetLoading(false);
    portal::render::SetBlink(false);
    portal::casting::UnlockMovement();
}

void RunGameplayTick(void* app, std::vector<portal::PortalInstance>& outPortals,
                     float& outPx, float& outPy, float& outPz,
                     char* outPrompt, size_t promptCap) {
    outPortals.clear();
    outPrompt[0] = '\0';
    if (g_paused) return;

    void* gameClient = GetGameClient(app);
    void* currentWorld = gameClient ? at<void*>(gameClient, 0x47d4e0) : nullptr;
    portal::travel::SetClient(currentWorld ? gameClient : nullptr);
    portal::casting::Bind(currentWorld ? gameClient : nullptr);
    if(portal::sharing::Enabled()){
        std::vector<portal::PortalInstance> sharedPortals;
        if(currentWorld)portal::travel::GetActivePortals(sharedPortals);
        portal::sharing::Tick(currentWorld,gameClient,currentWorld?portal::travel::LocalSteamAccount():0,
                              sharedPortals,[](void* client,std::string_view line){SendHiddenLine(client,line);});
    }

    uint8_t* uiClient = GetUiClient();
    float px = 0.0f, py = 0.0f, pz = 0.0f, dirX = 0.0f, dirZ = 1.0f;
    bool gotPos = uiClient && GetPlayerPosAndFacing(uiClient, px, py, pz, dirX, dirZ);

    if (!currentWorld) {
        // Rejoining can reuse the same client/world objects; start the host
        // check over so an old deadline cannot mark the new session "absent".
        g_link = {};
        g_hostMod = HostMod::checking;
        g_facing = {};
        ResetTravelPresentation();
        if (g_lastWorld != nullptr) {
            portal::travel::ClearAllPortals();
            portal::render::ClearNativeEffects();
            g_lastWorld = nullptr;
        }
        return;
    }
    if (g_lastWorld && g_lastWorld != currentWorld) {
        ResetTravelPresentation();
        g_link = {};
        portal::travel::ClearAllPortals();
        portal::render::ClearNativeEffects();
    }
    g_lastWorld = currentWorld;

    g_hostMod = EnsureHostHandshake(gameClient, currentWorld);
    if (g_hostMod == HostMod::present) AskHostRules(gameClient, currentWorld);

    if (!uiClient || !gotPos) return;
    outPx = px;
    outPy = py;
    outPz = pz;
    const int areaFlag = ReadAreaReady(gameClient);
    const bool areaReady = areaFlag >= 0 ? areaFlag == 1 : portal::travel::LocalTerrainReady(px, py, pz);
    portal::sceneprobe::SnapshotAreaReady(areaReady, GetTickCount64());
    CheckChannel(gameClient, px, py, pz);
    CheckDeparture(gameClient, uiClient, areaReady, px, py, pz);
    CheckArrival(gameClient, px, py, pz);
    UpdateLoadingProgress(GetTickCount64());
    if (g_home.world != g_lastWorld) g_home = {g_lastWorld};
    if (!g_departure.gate.Active() && !g_arrival.since)
        CheckLoginSnapshot(gameClient, uiClient, px, py, pz, uiClient[0x1cd8] == 1 && (g_seenPlayFrame + 2 >= g_frame));
    CheckDefaultPicture(uiClient, GetTickCount64());

    bool inPlayHud = uiClient[0x1cd8] == 1 && (g_seenPlayFrame + 2 >= g_frame);
    bool focused = IsGameFocused();
    bool textActive = IsTextInputActive();
    bool travelling = g_departure.gate.Active() || g_arrival.since || portal::sceneprobe::SnapshotBusy();
    bool canAcceptHotkeys = inPlayHud && focused && !textActive && !travelling;
    if (focused) InstallKeyHook();

    // The cast key (OpenPortalKey in town-portal.ini, T by default).
    bool tDown = canAcceptHotkeys && KeyDown(portal::rules::OpenPortalKey()) &&
                 !KeyDown(VK_CONTROL) && !KeyDown(VK_MENU) && !KeyDown(VK_SHIFT);
    bool tPressed = tDown && !g_tWasDown;
    g_tWasDown = tDown;

    bool ePhysical = KeyDown('E') || PadInteractDown();
    bool eDown = canAcceptHotkeys && ePhysical &&
                 !KeyDown(VK_CONTROL) && !KeyDown(VK_MENU);
    bool ePressed = eDown && !g_eWasDown;
    g_eWasDown = ePhysical;
#ifdef TOWN_PORTAL_DEV
    if(g_devInteractRequested&&inPlayHud&&!textActive&&!travelling) {
        ePressed=true;
        g_devInteractRequested=false;
    }
#endif

    TrackFacing(px, pz);
    const float lookX = dirX, lookZ = dirZ; // the camera's look, before a cast swaps in the walking direction
    if (tPressed && PortalsAvailable()) {
        if (g_facing.known) { dirX = g_facing.x; dirZ = g_facing.z; }
        TryOpenPortalAtPlayer(gameClient, uiClient, px, py, pz, dirX, dirZ);
    }

    portal::travel::GetActivePortals(outPortals);
    portal::markers::Publish(uiClient, outPortals, GetTickCount64());
    if (outPortals.empty()) return;

    uint64_t localAccount = portal::travel::LocalSteamAccount();
    uint64_t now = GetTickCount64();

    // Periodically rebroadcast our own open portal every 4s in multiplayer so newly joining players see it
    for (auto& p : outPortals) {
        if (!p.closing) {
            float dbSq = (px - p.baseEnd.wx) * (px - p.baseEnd.wx) + (pz - p.baseEnd.wz) * (pz - p.baseEnd.wz);
            // Ground refinement is for arrival safety, not a per-frame visual effect.
            static uint64_t lastGroundTick[portal::kMaxPortals]{};
            size_t groundSlot=size_t(&p-outPortals.data()) % portal::kMaxPortals;
            if (dbSq < 40.0f * 40.0f && now >= lastGroundTick[groundSlot] + 500) {
                lastGroundTick[groundSlot] = now;
                float refinedBaseY = std::max(p.baseEnd.wy, p.altarY);
                if (portal::travel::RefineLocalGroundY(p.baseEnd.wx, refinedBaseY, p.baseEnd.wz)) {
                    refinedBaseY = std::max(refinedBaseY, p.altarY);
                    if (std::fabs(refinedBaseY - p.baseEnd.wy) > 0.08f) {
                        p.baseEnd.wy = refinedBaseY;
                        p.baseEnd.SyncFixedFromFloat();
                        portal::travel::UpsertPortal(p);
                    }
                }
            }
            // The owner re-checks a provisional base spot once its area has loaded around them.
            if (p.ownerAccount == localAccount && g_baseChecked != p.portalId && dbSq < 60.0f * 60.0f &&
                now - std::min(now, p.openedTick) > portal::kPortalFormMs)
                CheckOwnBasePortal(gameClient, p);
            // If owner is within 24m of the Base Altar, mark ownerVisitedBase = true
            if (p.ownerAccount == localAccount && !p.ownerVisitedBase && dbSq < 24.0f * 24.0f) {
                if (portal::travel::MarkOwnerVisitedBase(localAccount)) {
                    p.ownerVisitedBase = true;
                    BroadcastPortalOpen(gameClient, p);
                }
            }
            if (p.ownerAccount == localAccount && now >= g_lastBroadcastTick + 12000) {
                BroadcastPortalOpen(gameClient, p);
            }
        }
    }

    // The grace period after the owner went back is over: close the portal for everyone.
    if (g_pendingClose.portalId && now >= g_pendingClose.at) {
        bool open = false;
        for (const auto& p : outPortals)
            open |= p.portalId == g_pendingClose.portalId && p.ownerAccount == localAccount && !p.closing;
        if (open) {
            portal::travel::ClosePortalByOwner(localAccount, g_pendingClose.portalId);
            BroadcastPortalClose(gameClient, g_pendingClose.portalId, localAccount);
            Log("Town Portal: portal closed after the grace period");
        }
        g_pendingClose = {};
    }

    // Only a fresh E press activates a nearby portal.
    float minPortalDist = 1e9f;
    const portal::PortalInstance* nearestPortal = nullptr;
    bool nearestIsFieldSide = true;

    for (const auto& p : outPortals) {
        if (p.closing) continue;

        // Check Field Endpoint
        {
            float dx = px - p.fieldEnd.wx;
            float dy = py - p.fieldEnd.wy;
            float dz = pz - p.fieldEnd.wz;
            float hDist = std::sqrt(dx * dx + dz * dz);
            if (std::fabs(dy) < 3.0f && hDist < minPortalDist) {
                minPortalDist = hDist;
                nearestPortal = &p;
                nearestIsFieldSide = true;
            }
        }
        // Check Base Endpoint
        {
            float dx = px - p.baseEnd.wx;
            float dy = py - p.baseEnd.wy;
            float dz = pz - p.baseEnd.wz;
            float hDist = std::sqrt(dx * dx + dz * dz);
            if (std::fabs(dy) < 3.0f && hDist < minPortalDist) {
                minPortalDist = hDist;
                nearestPortal = &p;
                nearestIsFieldSide = false;
            }
        }
    }

    if (!nearestPortal || minPortalDist > 2.95f || !inPlayHud || textActive || travelling) return;
    // Still forming (its caster is channeling): no prompt, no travel.
    if (nearestPortal->closing || now - std::min(now, nearestPortal->openedTick) < portal::kPortalFormMs) return;
    const auto& nearestEnd = nearestIsFieldSide ? nearestPortal->fieldEnd : nearestPortal->baseEnd;
    if (portal::travel::IsRayOccluded(px,py+1.1f,pz,nearestEnd.wx,nearestEnd.wy+1.1f,nearestEnd.wz)) return;
    // Turned away (to a chest beside it, say): no prompt, and E stays the game's.
    if (!portal::promptview::Facing(px, pz, lookX, lookZ, nearestEnd.wx, nearestEnd.wz, nearestEnd.rx, nearestEnd.rz)) return;

    bool isOwner = (nearestPortal->ownerAccount == localAccount);
    const char* destination = portal::names::Destination(uiClient, *nearestPortal, nearestIsFieldSide);
    // Home through the owner's own portal: say it is their altar; where it stands goes on the
    // prompt's second line (after "  /  ") and in brackets on the loading screen.
    char altarDestination[192]{};
    const char* place = nullptr;
    if (nearestIsFieldSide && isOwner) {
        const char* altar = portal::text::Get(portal::render::Language(), portal::text::YourAltar);
        if (std::strcmp(destination, altar) != 0) place = destination;
        std::snprintf(altarDestination, sizeof(altarDestination), place ? "%s (%s)" : "%s", altar, place);
        destination = altarDestination;
    }
    // The owner's "closes your portal" only when it will (see DisableAutoClose).
    auto prompt = nearestIsFieldSide ? portal::text::TravelTo
                : !isOwner ? portal::text::SharedTo
                : CurrentRules().disableAutoClose ? portal::text::TravelTo : portal::text::ReturnTo;
    if (place) {
        const int n = std::snprintf(outPrompt, promptCap, portal::text::Get(portal::render::Language(), prompt),
                                    portal::text::Get(portal::render::Language(), portal::text::YourAltar));
        if (n > 0 && size_t(n) < promptCap) std::snprintf(outPrompt + n, promptCap - n, "  /  %s", place);
    } else
        std::snprintf(outPrompt, promptCap, portal::text::Get(portal::render::Language(), prompt), destination);
    g_portalPromptUntil = GetTickCount64() + 250; // E belongs to the portal while this shows
    g_promptAnchor[0] = nearestEnd.wx + nearestEnd.rx * 1.25f;
    g_promptAnchor[1] = nearestEnd.wy + 1.3f;
    g_promptAnchor[2] = nearestEnd.wz + nearestEnd.rz * 1.25f;
    const uint32_t tint = portal::colors::ForPlayer(nearestPortal->ownerPlayerId, *reinterpret_cast<uint32_t*>(uiClient + 0x18));

    // Passing through the plane never initiates travel. Holding E cannot retrigger it.
    bool keyTriggered = (ePressed && minPortalDist < 2.85f && now >= g_lastTeleportTick + 900 && now>=g_castUntil);

    if (keyTriggered && PortalsAvailable()) {
        if (nearestIsFieldSide) {
            // Travel: Field Portal -> Base Portal (emerge 1.9m in front of Base Portal, facing the Flame Altar)
            float destX = nearestPortal->baseEnd.wx + nearestPortal->baseEnd.nx * 1.90f;
            float destY = nearestPortal->baseEnd.wy;
            float destZ = nearestPortal->baseEnd.wz + nearestPortal->baseEnd.nz * 1.90f;
            portal::travel::RefineLocalGroundY(destX, destY, destZ);
            QueuePortalTravel(*nearestPortal, true, portal::ToFixed(destX, destY, destZ), tint, destination, px, pz, PlayerInShroud(uiClient));
        } else {
            // Travel: Base Portal -> Field Portal (emerge 2.2m in front of Field Portal)
            float destX = nearestPortal->fieldEnd.wx + nearestPortal->fieldEnd.nx * 2.20f;
            float destY = nearestPortal->fieldEnd.wy;
            float destZ = nearestPortal->fieldEnd.wz + nearestPortal->fieldEnd.nz * 2.20f;
            portal::travel::RefineLocalGroundY(destX, destY, destZ);
            QueuePortalTravel(*nearestPortal, false, portal::ToFixed(destX, destY, destZ), tint, destination, px, pz, PlayerInShroud(uiClient));
        }
    }
}

std::vector<portal::PortalInstance> g_framePortals;
float g_framePx = 0.0f, g_framePy = 0.0f, g_framePz = 0.0f;
char g_framePrompt[160]{};

// Where a gameplay-tick exception came from: the code address as an offset into this DLL
// (look it up in bin\town-portal.map or with the .pdb) and, for an access violation, the
// address read or written. Each fault site is logged once.
int TickFault(EXCEPTION_POINTERS* info) {
    static uintptr_t seen[16]{};
    static int seenCount = 0;
    const auto* record = info->ExceptionRecord;
    const auto at = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    for (int i = 0; i < seenCount; ++i)
        if (seen[i] == at) return EXCEPTION_EXECUTE_HANDLER;
    if (seenCount < 16) seen[seenCount++] = at;
    HMODULE self{}, ours{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(at), &self);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&TickFault), &ours);
    char line[200]{};
    std::snprintf(line, sizeof(line), "Town Portal: gameplay tick fault %08lx at %s+0x%llx (data 0x%llx)",
                  record->ExceptionCode, self == ours ? "town-portal" : (self ? "module" : "?"),
                  static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(self)),
                  static_cast<unsigned long long>(record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0));
    Log(line);
    return EXCEPTION_EXECUTE_HANDLER;
}

void SafeLoopTick(void* app) {
    __try {
        RunGameplayTick(app, g_framePortals, g_framePx, g_framePy, g_framePz,
                        g_framePrompt, sizeof(g_framePrompt));
    } __except (TickFault(GetExceptionInformation())) {
        Log("Town Portal: exception in gameplay tick");
    }
}

void LoopHook(void* app) {
    greeneye::Lease lease;
    if (!lease) {
        g_nextLoop(app);
        return;
    }
    g_currentApp = app;
    g_nextLoop(app);
    SafeLoopTick(app);
    ++g_frame;
}

void SafePlayDraw() {
    __try {
        g_seenPlayFrame = g_frame;
        uint8_t* uiClient = GetUiClient();
        if (uiClient) {
            portal::render::DrawHudFrame(
                uiClient,
                g_framePortals,
                portal::travel::LocalSteamAccount(),
                g_framePx, g_framePy, g_framePz,
                g_framePrompt,
                g_promptAnchor[0], g_promptAnchor[1], g_promptAnchor[2], g_framePrompt[0] != 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Town Portal: exception in HUD portal renderer");
    }
}

void PlayHook(void* self) {
    g_nextPlay(self);
    greeneye::Lease lease;
    if (lease && !g_paused) {
        SafePlayDraw();
    }
}

bool HotPrepare() {
    // Never retire a generation while its temporary snapshot view or queued
    // departure is in use. The caller can retry after travel has completed.
    if (g_departure.gate.Active() || g_arrival.since || portal::sceneprobe::SnapshotBusy()) return false;
    g_paused = true;
    RemoveKeyHook();
    portal::sharing::Stop();
    portal::homecache::Stop();
    portal::defaults::Stop();
    g_defaultPicture = {};
    portal::casting::Stop();
    portal::render::ClearNotice();
    portal::render::ClearNativeEffects();
    return portal::travel::Prepare();
}

bool HotReady() {
    return true;
}

void HotResume() {
    g_paused = false;
    g_tWasDown = true;
    g_eWasDown = true;
    portal::travel::Resume();
}

} // namespace

#ifdef TOWN_PORTAL_DEV
extern "C" __declspec(dllexport) bool TownPortalSceneProbe(char* out,uint32_t capacity) {
    greeneye::Lease lease;
    bool ok=lease&&portal::sceneprobe::Json(out,capacity);
    if(ok)portal::render::AppendPresentationStatus(out,capacity);
    if(ok)portal::casting::AppendStatus(out,capacity);
    if(ok)portal::travel::AppendDevStatus(out,capacity);
    if(ok)portal::sharing::AppendStatus(out,capacity);
    return ok;
}
extern "C" __declspec(dllexport) int TownPortalDevControl(
    const TownPortalDevRequest* request,TownPortalDevStatus* status) {
    greeneye::Lease lease;
    if (!lease) return 0; // A retired generation must never handle commands.
    if (!request||!status||request->size!=sizeof(*request)||status->size!=sizeof(*status)) return -1;
    uint8_t* ui=GetUiClient();
    void* client=GetGameClient(g_currentApp);
    if (!ui||!client||!at<void*>(client,0x47d4e0)) return -2;
    float x{},y{},z{},dx{},dz{};
    if (!GetPlayerPosAndFacing(ui,x,y,z,dx,dz)) return -2;
    uint64_t owner=portal::travel::LocalSteamAccount();
    portal::PortalInstance own{};
    bool hasOwn=portal::travel::GetPortalByOwner(owner,own);
    // Mutating commands use the normal gameplay path, including authoritative
    // teleport submission and synchronization, with no OS input or focus change.
    // Resource cleanup must remain available when a menu opens during a probe.
    if (request->action && request->action!=5 && request->action!=8 && ui[0x1cd8]!=1) return -3;
    switch(request->action) {
    case 0: break;
    case 1: if(g_facing.known){dx=g_facing.x;dz=g_facing.z;} TryOpenPortalAtPlayer(client,ui,x,y,z,dx,dz); break;
    case 2:
        if (hasOwn) {
            portal::travel::ClosePortalByOwner(owner,own.portalId);
            BroadcastPortalClose(client,own.portalId,owner);
        }
        break;
    case 3: case 4: {
        if (!hasOwn) return -4;
        const auto& end=request->action==3?own.baseEnd:own.fieldEnd;
        float distance=request->action==3?1.9f:2.2f;
        if (!ExecutePortalTeleport(client,portal::ToFixed(end.wx+end.nx*distance,end.wy,end.wz+end.nz*distance))) return -5;
        break;
    }
    case 5: break;
    case 13: g_devInteractRequested=true;break;
    case 15: if(!portal::casting::Start(client))return -6;g_castUntil=GetTickCount64()+portal::casting::DurationMs();break;
    case 16: {
        const auto* p=request->from;
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])||
           p[0]<100||p[0]>12000||p[1]<1||p[1]>5000||p[2]<100||p[2]>12000)return -1;
        if(!ExecutePortalTeleport(client,portal::ToFixed(p[0],p[1],p[2])))return -5;
        break;
    }
    case 14: {
        if(!hasOwn)return -4;
        float df=(x-own.fieldEnd.wx)*(x-own.fieldEnd.wx)+(z-own.fieldEnd.wz)*(z-own.fieldEnd.wz);
        float db=(x-own.baseEnd.wx)*(x-own.baseEnd.wx)+(z-own.baseEnd.wz)*(z-own.baseEnd.wz);
        const auto& end=df<=db?own.fieldEnd:own.baseEnd;
        if(!ExecutePortalTeleport(client,portal::ToFixed(end.wx+end.nx*.15f,end.wy,end.wz+end.nz*.15f)))return -5;
        break;
    }
    case 7: case 8: case 9: case 10: case 11: case 12:

        if (!portal::sceneprobe::TargetRequest(request->action)) return -6;
        break;
    case 6: {
        if (!hasOwn) return -4;
        float df=(x-own.fieldEnd.wx)*(x-own.fieldEnd.wx)+(z-own.fieldEnd.wz)*(z-own.fieldEnd.wz);
        float db=(x-own.baseEnd.wx)*(x-own.baseEnd.wx)+(z-own.baseEnd.wz)*(z-own.baseEnd.wz);
        if (!portal::sceneprobe::Preview(own,df<=db,2500)) return -6;
        break;
    }
    default: return -1;
    }
    TownPortalDevStatus result{};
    result.generation=greeneye::Host()->generation;
    result.screen=ui[0x1cd8];
    result.player[0]=x;result.player[1]=y;result.player[2]=z;
    hasOwn=portal::travel::GetPortalByOwner(owner,own);
    result.ownPortal=hasOwn;
    if (hasOwn) {
        result.field[0]=own.fieldEnd.wx;result.field[1]=own.fieldEnd.wy;result.field[2]=own.fieldEnd.wz;
        result.base[0]=own.baseEnd.wx;result.base[1]=own.baseEnd.wy;result.base[2]=own.baseEnd.wz;
    }
    std::vector<portal::PortalInstance> portals;
    portal::travel::GetActivePortals(portals);
    result.portals=static_cast<uint32_t>(portals.size());
    if (request->action==5) {
        result.rayVisibility=static_cast<uint32_t>(portal::travel::TraceVisibility(
            portal::travel::GetVisibilityContext(),
            {request->from[0],request->from[1],request->from[2]},
            {request->to[0],request->to[1],request->to[2]}));
    }
    portal::render::GetDevStatus(result);
    *status=result;
    return 1;
}
#endif

// The PC's graphics adapters and their video memory, for bug reports (portal pictures depend on
// the GPU keeping them). DXGI is loaded only for this, so the build gains no link dependency.
void LogGraphicsAdapters() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                char name[128]{}, line[224]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                std::snprintf(line, sizeof(line), "Town Portal: graphics adapter %s (%llu MB video memory)", name,
                              (unsigned long long)(desc.DedicatedVideoMemory >> 20));
                Log(line);
            }
            adapter->Release();
        }
        factory->Release();
    }
    FreeLibrary(dxgi);
}

GREENEYE_MOD_START {
    if (!greeneye::HostHasHooks(host) || !host->find || !host->read) {
        return false;
    }
    greeneye::UseHost(host);

    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    bool serverSide = (host->side == kGreeneyeServer);
    // Another build of the game (a beta branch, a patch, a modified exe): start anyway. Every
    // hook is found by signature and turns itself off when missing; the cast animation and the
    // in-frame renderer, which read fixed layouts, keep their own exact-build checks.
    if (serverSide ? !portal::build::Matches(portal::build::kServerStamp, portal::build::kServerSize) : !portal::build::Client()) {
        char line[200];
        std::snprintf(line, sizeof(line), "Town Portal: WARNING: game build %08lx/%lx is not the one this version was made for; trying anyway (report problems with this log)",
                      static_cast<unsigned long>(nt->FileHeader.TimeDateStamp), static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
        Log(line);
    }

    if (!greeneye::ClaimProcess(L"TownPortal", host)) {
        return false;
    }

    portal::rules::WriteDefaults();
    portal::rules::WriteControlsDefaults();
    if (!portal::travel::Init(host)) {
        Log("Town Portal: host travel bindings unavailable; disabled");
        return false;
    }

    if (serverSide) {
        Log("Town Portals 1.0.3: server companion loaded");
        return true;
    }

    auto* mapTail = host->find("48 8D AC 24 A0 F8 FF FF 48 81 EC 60 08 00 00 48 8B 05", nullptr);
    auto* loop = host->find("40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 98 00 00 00 48 8D 81 58 4F 03 00", nullptr);
    auto* send = host->find("4C 8B DC 55 56 57 49 8D AB 18 FF FF FF", nullptr);
    auto* play = host->find("4C 8B DC 55 49 8D AB E8 FD FF FF 48 81 EC 10 03 00 00", nullptr);
    if (!mapTail || !loop || !send || !play) {
        Log("Town Portal: unsupported UI signatures; disabled");
        return false;
    }

    int32_t disp = 0;
    host->read(mapTail + 0x12, &disp, 4);
    g_root = reinterpret_cast<void**>(mapTail + 0x16 + disp);

    if (!portal::render::Init(host, g_root)) {
        Log("Town Portal: portal renderer bindings unavailable; disabled");
        return false;
    }
    // GPU depth compositor (and, in dev builds, the scene diagnostics).
    portal::sceneprobe::Init(host);

    g_dispatch = reinterpret_cast<DispatchFn>(send);
    // cmp byte [rdi+allowed],0 / jne / cmp byte [rdx+1],4 in the chat command (0x74).
    if (auto* check = static_cast<uint8_t*>(host->find("80 BF ?? ?? ?? ?? 00 75 ?? 80 7A 01 04", nullptr)))
        host->read(check + 2, &g_textChatAllowed, 4);
    // movzx ecx, byte [rsi+disp32] ... mov [rsi+disp32], al: the ready-for-gameplay latch.
    if (auto* ready = static_cast<uint8_t*>(host->find(
            "0F B6 8E ?? ?? ?? ?? 84 DB 74 12 45 84 F6 74 0D 45 84 FF 75 08 84 C0 75 04 B0 01 EB 02 32 C0 0A C1 88 86", nullptr)))
        host->read(ready + 3, &g_areaReady, 4);
    if (!g_areaReady) Log("Town Portal: area-ready signal not found; arrival waits on terrain instead");
    // player_fog_update: movzx ecx,[rax] ... mov [rax+fogType],cl; sete cl ... mov [rax+deadly],cl.
    if (auto* fog = static_cast<uint8_t*>(host->find(
            "0F B6 08 48 8B 44 24 ?? 80 F9 03 88 88 ?? ?? ?? ?? 0F 94 C1 48 8B 44 24 ?? 88 88 ?? ?? ?? ??", nullptr)))
        host->read(fog + 13, &g_fogType, 4);
    // player_ui: shr edx,12h / and dl,1 / mov [rsi+bigDungeon],dl (playerStateMask bit 18).
    if (auto* dungeon = static_cast<uint8_t*>(host->find("C1 EA 12 80 E2 01 88 96 ?? ?? ?? ??", nullptr)))
        host->read(dungeon + 8, &g_bigDungeon, 4);
    if (g_fogType <= 0 || g_fogType > 0x800000) g_fogType = 0;
    if (g_bigDungeon <= 0 || g_bigDungeon > 0x800000) g_bigDungeon = 0;
    {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: Shroud state %s, Hollow Halls state %s",
                      g_fogType ? "found" : "not found (the Shroud rule cannot block)",
                      g_bigDungeon ? "found" : "not found (area boxes only)");
        Log(line);
    }
    if (g_textChatAllowed <= 0 || g_textChatAllowed > 0x800000) {
        g_textChatAllowed = 0;
        Log("Town Portal: chat permission check not found; portals on servers need text chat enabled");
    }
    g_nextLoop = reinterpret_cast<LoopFn>(host->hook(loop, reinterpret_cast<void*>(LoopHook), 13, nullptr, 0));
    g_nextPlay = reinterpret_cast<LoopFn>(host->hook(play, reinterpret_cast<void*>(PlayHook), 18, nullptr, 0));
    if (!g_nextLoop || !g_nextPlay) {
        g_paused = true;
        Log("Town Portal: hook installation failed");
        return false;
    }
    portal::casting::Init(host);
    LogGraphicsAdapters();

    Log("Town Portals 1.0.3: press T (OpenPortalKey in town-portal.ini) to open a portal to your Preferred Spawn Flame Altar");
    return true;
}

GREENEYE_MOD_HOT {
    static const GreeneyeHot kHotApi{sizeof(GreeneyeHot), &HotPrepare, &HotReady, &HotResume};
    return &kHotApi;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
