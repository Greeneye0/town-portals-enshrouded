#pragma once
// Portal colours match each player's minimap icon in the map mod (UnlockMapMarkers
// src/player_markers.h, minimap_players::ColorFor): by join order - the server hands out
// player-list ids from 1, so the first player in the server is yellow, then blue, red, ... -
// the same on every screen, so "the yellow portal" means the same player to everyone. Past
// 16 players, hues. Colours are ABGR (0xAABBGGRR) like the UI.
#include <cmath>
#include <cstdint>

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

// minimap_players::ColorFor, made vivid. localPlayerId is unused: your own colour is the
// one everyone else sees.
inline uint32_t ForPlayer(uint32_t ownerPlayerId, uint32_t /*localPlayerId*/) {
    if (!ownerPlayerId) return Vivid(kUnknown);
    if (ownerPlayerId <= 16) return Vivid(kJoinPalette[ownerPlayerId - 1]);
    return Vivid(Hue((ownerPlayerId * 2654435761u) % 64));
}

inline void ToFloat(uint32_t abgr, float rgb[3]) {
    rgb[0] = float(abgr & 0xff) / 255.0f;
    rgb[1] = float((abgr >> 8) & 0xff) / 255.0f;
    rgb[2] = float((abgr >> 16) & 0xff) / 255.0f;
}

}
