#pragma once
// Easter egg: one portal in 50 opens "to another game". For a few seconds both its ends show
// Orgrimmar, Stormwind, Whiterun, Rivendell or the Meadows (greeneye\town-portal\easter\<name>.jpg, the
// author's own art, shown by the portal like a default picture, with its window depth) and that
// place's name, then static washes over it like an old TV finding its channel and the real
// destination comes back. The player who opens the portal rolls and
// tells everyone with a hidden line (/tpx1, portal_protocol.h), so all players see the same one.
#include <cstdint>
#include <cmath>
#include <mutex>
#include "portal_text.h"
#include "portal_types.h"

namespace portal::easter {

constexpr int kVariants = 5;
constexpr const wchar_t* kFiles[kVariants] = {L"orgrimmar", L"stormwind", L"whiterun", L"rivendell", L"meadows"};
constexpr uint32_t kChancePerMille = 20; // 2%

// From the moment the portal opens (it forms over kPortalFormMs): the other game's picture
// forms with it, its name shows for 2.5 s after, then 0.8 s of static; at its height (kSwitchMs
// in) the real picture and name come back under it.
constexpr uint64_t kFlickerAtMs = kPortalFormMs + 2500;
constexpr uint64_t kFlickerMs = 800;
constexpr uint64_t kSwitchMs = 500;
constexpr uint64_t kEndMs = kFlickerAtMs + kFlickerMs;

// The places' names as each language's own release of those games (and Rivendell's books and
// films) has them; where there was none in a language, the English name.
inline const char* Name(text::Lang lang, int variant) {
    static constexpr const char* kNames[kVariants][size_t(text::Lang::count)] = {
        // en, fr, it, de, es, ja, ko, pl, pt, ru, zh-hans, zh-hant, tr, uk, th
        {"Orgrimmar", "Orgrimmar", "Orgrimmar", "Orgrimmar", "Orgrimmar", "オーグリマー", "오그리마", "Orgrimmar",
         "Orgrimmar", "Оргриммар", "奥格瑞玛", "奧格瑪", "Orgrimmar", "Оргріммар", "ออร์กริมมาร์"},
        {"Stormwind", "Hurlevent", "Roccavento", "Sturmwind", "Ventormenta", "ストームウィンド", "스톰윈드", "Stormwind",
         "Ventobravo", "Штормград", "暴风城", "暴風城", "Stormwind", "Штормград", "สตอร์มวินด์"},
        {"Whiterun", "Blancherive", "Whiterun", "Weißlauf", "Carrera Blanca", "ホワイトラン", "화이트런", "Biała Grań",
         "Whiterun", "Вайтран", "雪漫城", "雪漫城", "Whiterun", "Вайтран", "ไวท์รัน"},
        {"Rivendell", "Fondcombe", "Gran Burrone", "Bruchtal", "Rivendel", "裂け谷", "리븐델", "Rivendell",
         "Valfenda", "Ривенделл", "瑞文戴尔", "瑞文戴爾", "Ayrıkvadi", "Рівенділ", "ริเวนเดลล์"},
        {"Meadows", "Prairies", "Prati", "Wiesen", "Praderas", "草原", "초원", "Łąki",
         "Campinas", "Луга", "草原", "草原", "Çayırlar", "Луки", "Meadows"},
    };
    if (variant < 0 || variant >= kVariants || size_t(lang) >= size_t(text::Lang::count)) return "";
    return kNames[variant][size_t(lang)];
}

struct Gag {
    uint64_t portalId = 0;
    int variant = 0;
    uint64_t start = 0;
};

namespace detail {
inline std::mutex g_lock;
inline Gag g_gags[8]{};
} // namespace detail

// A portal plays the gag from now (a repeat for the same portal is ignored).
inline void Start(uint64_t portalId, int variant, uint64_t now) {
    if (!portalId || variant < 0 || variant >= kVariants) return;
    std::lock_guard guard(detail::g_lock);
    Gag* slot = &detail::g_gags[0];
    for (auto& g : detail::g_gags) {
        if (g.portalId == portalId) return;
        if (g.start < slot->start) slot = &g;
    }
    *slot = {portalId, variant, now};
}

// The static's strength f ms into the flicker (0..1): faint with two blips, at its height at the
// switch to the real picture, gone by the end.
inline float StaticAmount(uint64_t f) {
    const float ff = float(f), sw = float(kSwitchMs);
    float a = 0.12f + 0.08f * std::sin(ff * 0.021f);
    if ((f >= 150 && f < 210) || (f >= 330 && f < 380)) a += 0.3f;
    a += 0.75f * std::exp(-(ff - sw) * (ff - sw) / (2 * 60.0f * 60.0f)); // the swell hiding the switch
    const float fade = (float(kFlickerMs) - ff) / 200.0f;
    return std::min(std::max(a * std::min(std::max(fade, 0.0f), 1.0f), 0.0f), 0.9f);
}

// Whether the other game's picture shows (until the switch).
inline bool ShowsPicture(uint64_t elapsed) { return elapsed < kFlickerAtMs + kSwitchMs; }

// The gag this portal is playing, with how far into it (ms); false when none.
inline bool Playing(uint64_t portalId, uint64_t now, int& variant, uint64_t& elapsed) {
    std::lock_guard guard(detail::g_lock);
    for (const auto& g : detail::g_gags) {
        if (g.portalId != portalId || !g.portalId || now < g.start || now - g.start >= kEndMs) continue;
        variant = g.variant;
        elapsed = now - g.start;
        return true;
    }
    return false;
}

// The name a portal end shows: the other game's while the gag shows it, the real one from the
// switch on.
inline const char* LabelFor(uint64_t portalId, uint64_t now, text::Lang lang, const char* real) {
    int variant = 0;
    uint64_t t = 0;
    if (!Playing(portalId, now, variant, t)) return real;
    return t < kFlickerAtMs + kSwitchMs ? Name(lang, variant) : real;
}

// The opening player's roll: -1 for none, else one of the places in available (bit per place:
// those whose picture is installed). salt mixes in the portal's id.
inline int Roll(uint64_t salt, uint64_t now, bool always, uint32_t available = (1u << kVariants) - 1) {
    available &= (1u << kVariants) - 1;
    int count = 0;
    for (int v = 0; v < kVariants; ++v) count += (available >> v) & 1;
    if (!count) return -1;
    uint64_t h = salt * 0x9e3779b97f4a7c15ull ^ (now << 17);
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    if (!always && h % 1000 >= kChancePerMille) return -1;
    int pick = int((h >> 20) % uint64_t(count));
    for (int v = 0; v < kVariants; ++v)
        if ((available >> v) & 1 && pick-- == 0) return v;
    return -1;
}

} // namespace portal::easter
