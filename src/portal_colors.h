#pragma once
// Portal colours match each player's minimap icon in the map mod (UnlockMapMarkers
// src/player_markers.h, minimap_players::ColorFor): by join order - the server hands out
// player-list ids from 1, so the first player in the server is yellow, then blue, red, ... -
// the same on every screen, so "the yellow portal" means the same player to everyone. Past
// 16 players, hues. Colours are ABGR (0xAABBGGRR) like the UI.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace portal::colors {

constexpr uint32_t kUnknown = 0xFFFFA61Fu;     // the original portal blue (owner not known)

// Same formula as minimap_players::Color: 64 hues around the whole circle.
inline uint32_t Hue(unsigned slot) {
    const float hue = static_cast<float>((slot * 23) % 64) * (360.0f / 64.0f) / 60.0f;
    const float c = 0.62f, m = 0.33f;
    const float x = c * (1.0f - std::abs(std::fmod(hue, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    if (hue < 1) { r = c; g = x; }
    else if (hue < 2) { r = x; g = c; }
    else if (hue < 3) { g = c; b = x; }
    else if (hue < 4) { g = x; b = c; }
    else if (hue < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return 0xff000000u | (static_cast<uint32_t>((b + m) * 255) << 16) |
        (static_cast<uint32_t>((g + m) * 255) << 8) | static_cast<uint32_t>((r + m) * 255);
}

// The minimap colours are pastel; a portal built from one looks like a tinted blue one.
// Push the colour away from grey and scale its brightest channel to kValue (HSV value; full
// brightness was too bright in game).
constexpr float kValue = 0.5f;
inline uint32_t Vivid(uint32_t abgr) {
    float c[3] = {float(abgr & 0xff) / 255.0f, float((abgr >> 8) & 0xff) / 255.0f, float((abgr >> 16) & 0xff) / 255.0f};
    const float lum = 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
    float top = 0.0f;
    for (float& v : c) { v = std::fmin(1.0f, std::fmax(0.0f, lum + (v - lum) * 1.8f)); top = std::fmax(top, v); }
    uint32_t out = 0xff000000u;
    for (int i = 0; i < 3; ++i) out |= uint32_t((top > 0 ? c[i] / top : 1.0f) * kValue * 255.0f + 0.5f) << (8 * i);
    return out;
}

inline constexpr uint32_t kJoinPalette[16] = {
    0xFF28D2FFu /* yellow */,
    0xFFFF7828u /* blue */,
    0xFF3232EBu /* red */,
    0xFF50C83Cu /* green */,
    0xFFE650A0u /* purple */,
    0xFF1E82FFu /* orange */,
    0xFFDCD228u /* cyan */,
    0xFFAA5AF0u /* pink */,
    0xFFEBEBEBu /* white */,
    0xFF28E6AAu /* lime */,
    0xFF8C9614u /* teal */,
    0xFFD228D2u /* magenta */,
    0xFF325F96u /* brown */,
    0xFFA03228u /* navy */,
    0xFF288282u /* olive */,
    0xFF321E8Cu /* maroon */};

// The palette's names, for PortalColor in town-portal.ini ([Controls]).
inline constexpr const char* kNames[16] = {"yellow", "blue", "red", "green", "purple", "orange", "cyan", "pink",
                                           "white", "lime", "teal", "magenta", "brown", "navy", "olive", "maroon"};
// A name to its palette slot; -1 for Auto or anything else.
inline int ParseName(const char* name) {
    for (int i = 0; i < 16; ++i)
        if (!_stricmp(name, kNames[i])) return i;
    return -1;
}

// Preferred colours (PortalColor): each player's choice reaches everyone with their portal's
// announcement (/tpk1, portal_protocol.h). Among the players with open portals, those with no
// choice keep their join colour; then, in join order, a choice is granted when no one has that
// colour yet. Every game decides from the same portals, so all agree.
namespace detail {
inline constexpr uint32_t kMaxIds = 64;
inline std::mutex g_lock;
inline uint8_t g_preferred[kMaxIds + 1]{}; // slot + 1; 0: none
inline uint8_t g_granted[kMaxIds + 1]{};   // slot + 1; 0: the join colour
} // namespace detail

inline void SetPreference(uint32_t playerId, int slot) {
    if (!playerId || playerId > detail::kMaxIds) return;
    std::lock_guard guard(detail::g_lock);
    detail::g_preferred[playerId] = slot >= 0 && slot < 16 ? uint8_t(slot + 1) : 0;
}

// The players present: owners of open portals and the local player.
inline void Resolve(const uint32_t* playerIds, size_t count) {
    uint32_t ids[32]{};
    size_t n = 0;
    for (size_t i = 0; i < count && n < 32; ++i)
        if (playerIds[i] && playerIds[i] <= detail::kMaxIds && std::find(ids, ids + n, playerIds[i]) == ids + n) ids[n++] = playerIds[i];
    std::sort(ids, ids + n);
    std::lock_guard guard(detail::g_lock);
    uint32_t claimed = 0;
    for (size_t i = 0; i < n; ++i)
        if (!detail::g_preferred[ids[i]] && ids[i] <= 16) claimed |= 1u << (ids[i] - 1);
    uint8_t granted[detail::kMaxIds + 1]{};
    for (size_t i = 0; i < n; ++i) {
        const uint8_t want = detail::g_preferred[ids[i]];
        if (want && !(claimed & (1u << (want - 1)))) {
            granted[ids[i]] = want;
            claimed |= 1u << (want - 1);
        }
    }
    std::memcpy(detail::g_granted, granted, sizeof(granted));
    // The Worst Map Mod colours the players on its map from this too (UnlockMapMarkers
    // player_markers.h): "Local\\GreeneyePortalColours-<process id>", {u32 'GPC1', u8 slot + 1
    // per player id 0..64}. Kept open for the life of the game.
    static uint8_t* shared = nullptr;
    if (!shared) {
        wchar_t name[64];
        swprintf_s(name, L"Local\\GreeneyePortalColours-%lu", GetCurrentProcessId());
        if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 128, name))
            shared = static_cast<uint8_t*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 128));
        if (shared) std::memcpy(shared, "GPC1", 4);
    }
    if (shared) std::memcpy(shared + 4, granted, sizeof(granted));
}

// minimap_players::ColorFor, made vivid, unless a preferred colour was granted. localPlayerId is
// unused: your own colour is the one everyone else sees.
inline uint32_t ForPlayer(uint32_t ownerPlayerId, uint32_t /*localPlayerId*/) {
    if (!ownerPlayerId) return Vivid(kUnknown);
    if (ownerPlayerId <= detail::kMaxIds) {
        std::lock_guard guard(detail::g_lock);
        if (const uint8_t slot = detail::g_granted[ownerPlayerId]) return Vivid(kJoinPalette[slot - 1]);
    }
    if (ownerPlayerId <= 16) return Vivid(kJoinPalette[ownerPlayerId - 1]);
    return Vivid(Hue((ownerPlayerId * 2654435761u) % 64));
}

inline void ToFloat(uint32_t abgr, float rgb[3]) {
    rgb[0] = float(abgr & 0xff) / 255.0f;
    rgb[1] = float((abgr >> 8) & 0xff) / 255.0f;
    rgb[2] = float((abgr >> 16) & 0xff) / 255.0f;
}

}
