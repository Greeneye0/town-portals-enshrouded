#pragma once
// greeneye\town-portal\town-portal.ini, written with the defaults and comments when missing.
// [Portals] are server rules: the host reads its own file (beside a dedicated server, or the
// hosting player's game); clients get the host's rules over the hidden protocol channel
// (/tph2 asks, /tps2 answers) and follow them. [Controls] are each player's own.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <cwchar>
#include <cwctype>

namespace portal::rules {

struct Rules {
    bool allowInShroud = true, allowInHollowHalls = true, disableAutoClose = false;
};

inline constexpr std::string_view kAsk = "/tph2";
inline constexpr std::string_view kAnswerPrefix = "/tps2 ";

inline std::wstring Path() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    return std::wstring(path) + L"greeneye\\town-portal\\town-portal.ini";
}

inline void WriteDefaults() {
    const std::wstring path = Path();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (FILE* f = _wfopen(path.c_str(), L"w")) {
        std::fputs("; Town Portal settings.\n"
                   ";\n"
                   "; [Portals] are server settings: the host's file applies to everyone in its world (on\n"
                   "; a dedicated server, the file beside the server's Town Portal; when a player hosts, that\n"
                   "; player's file). Changes reach players within about 20 seconds.\n"
                   "[Portals]\n"
                   "; Allow opening a portal while standing in the Shroud: true or false.\n"
                   "AllowInShroud=true\n"
                   "; Allow opening a portal inside the Hollow Halls dungeons: true or false.\n"
                   "AllowInHollowHalls=true\n"
                   "; Keep a player's portal open when they return through it from their Flame Altar,\n"
                   "; instead of it closing behind them. It stays until they open a new one. true or false.\n"
                   "DisableAutoClose=false\n", f);
        std::fclose(f);
    }
}

// [Controls], this player's own. Added to a file written before it existed.
inline constexpr const char* kControlsSection =
    "\n"
    "; [Controls] are this player's own, whoever hosts.\n"
    "[Controls]\n"
    "; The key that opens your portal: a letter or digit, F1-F24, Num0-Num9, Insert, Delete,\n"
    "; Home, End, PageUp, PageDown, Tab, Space, Enter, Up, Down, Left, Right, Mouse4, Mouse5,\n"
    "; MiddleMouse, a punctuation key such as ; or ], or a key code such as 0x54. Default T.\n"
    "OpenPortalKey=T\n"
    "; Your character speaks when a portal can't be opened: true or false.\n"
    "VoiceFeedback=true\n"
    "; Portal effects: Auto follows the game's graphics settings (lighter on Low and Medium),\n"
    "; Low always draws the lighter portals, High always the full ones.\n"
    "PortalQuality=Auto\n";

// Keys added to [Controls] after it first shipped, appended to a file that lacks them (the
// file ends with [Controls], so they land in it).
inline constexpr const char* kVoiceKey =
    "; Your character speaks when a portal can't be opened: true or false.\n"
    "VoiceFeedback=true\n";
inline constexpr const char* kQualityKey =
    "; Portal effects: Auto follows the game's graphics settings (lighter on Low and Medium),\n"
    "; Low always draws the lighter portals, High always the full ones.\n"
    "PortalQuality=Auto\n";

inline void WriteControlsDefaults() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wcsstr(_wcslwr(exe), L"enshrouded_server")) return; // a dedicated server has no keys
    const std::wstring path = Path();
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"Controls", L"OpenPortalKey", L"\x1", value, 32, path.c_str());
    if (value[0] != L'\x1') {
        // [Controls] is there; add the keys that came later.
        for (auto [key, text] : {std::pair{L"VoiceFeedback", kVoiceKey}, std::pair{L"PortalQuality", kQualityKey}}) {
            GetPrivateProfileStringW(L"Controls", key, L"\x1", value, 32, path.c_str());
            if (value[0] == L'\x1')
                if (FILE* f = _wfopen(path.c_str(), L"a")) {
                    std::fputs(text, f);
                    std::fclose(f);
                }
        }
        return;
    }
    if (FILE* f = _wfopen(path.c_str(), L"a")) {
        std::fputs(kControlsSection, f);
        std::fclose(f);
    }
}

// Whether this player hears their character's voice feedback ([Controls] VoiceFeedback, on
// unless set false), read again at most once a second.
inline bool VoiceFeedback() {
    static bool on = true;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 1000) return on;
    readAt = now;
    wchar_t value[16]{};
    GetPrivateProfileStringW(L"Controls", L"VoiceFeedback", L"true", value, 16, Path().c_str());
    on = !(!_wcsicmp(value, L"false") || !_wcsicmp(value, L"0") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off"));
    return on;
}

// A key name to its Windows virtual-key code; 0 when not recognised.
inline int ParseKey(const wchar_t* name) {
    std::wstring key(name);
    while (!key.empty() && iswspace(key.back())) key.pop_back();
    while (!key.empty() && iswspace(key.front())) key.erase(key.begin());
    if (key.empty()) return 0;
    if (key.size() > 2 && key[0] == L'0' && (key[1] == L'x' || key[1] == L'X')) {
        const long code = wcstol(key.c_str() + 2, nullptr, 16);
        return code > 0 && code < 0xff ? int(code) : 0;
    }
    if (key.size() == 1) {
        const wchar_t c = towupper(key[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return int(c);
        const SHORT scan = VkKeyScanW(key[0]); // punctuation on this keyboard layout
        return scan != -1 && (scan & 0xff) ? scan & 0xff : 0;
    }
    if ((key[0] == L'F' || key[0] == L'f') && iswdigit(key[1])) {
        const int n = _wtoi(key.c_str() + 1);
        return n >= 1 && n <= 24 ? VK_F1 + n - 1 : 0;
    }
    auto is = [&](const wchar_t* other) { return _wcsicmp(key.c_str(), other) == 0; };
    for (int n = 0; n <= 9; ++n) {
        wchar_t a[16], b[16];
        swprintf_s(a, L"Num%d", n);
        swprintf_s(b, L"Numpad%d", n);
        if (is(a) || is(b)) return VK_NUMPAD0 + n;
    }
    struct Named { const wchar_t* name; int vk; };
    static constexpr Named kNames[] = {
        {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Tab", VK_TAB}, {L"Space", VK_SPACE},
        {L"Enter", VK_RETURN}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT},
        {L"Mouse4", VK_XBUTTON1}, {L"Mouse5", VK_XBUTTON2}, {L"MiddleMouse", VK_MBUTTON}};
    for (const auto& n : kNames)
        if (is(n.name)) return n.vk;
    return 0;
}

// The key that opens this player's portal (T unless set), read again at most once a second.
inline int OpenPortalKey() {
    static int key = 'T';
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 1000) return key;
    readAt = now;
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"Controls", L"OpenPortalKey", L"T", value, 32, Path().c_str());
    const int parsed = ParseKey(value);
    key = parsed ? parsed : 'T';
    return key;
}

inline Rules ReadFile() {
    const std::wstring path = Path();
    auto flag = [&](const wchar_t* key, bool fallback) {
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"Portals", key, L"", value, 16, path.c_str());
        if (!_wcsicmp(value, L"true") || !_wcsicmp(value, L"1") || !_wcsicmp(value, L"yes") || !_wcsicmp(value, L"on")) return true;
        if (!_wcsicmp(value, L"false") || !_wcsicmp(value, L"0") || !_wcsicmp(value, L"no") || !_wcsicmp(value, L"off")) return false;
        return fallback;
    };
    return {flag(L"AllowInShroud", true), flag(L"AllowInHollowHalls", true), flag(L"DisableAutoClose", false)};
}

// This machine's file, read again at most once a second (the travel prompt asks every frame).
inline Rules Local() {
    static Rules rules{};
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (!readAt || now - readAt > 1000) { rules = ReadFile(); readAt = now; }
    return rules;
}

inline uint32_t Bits(const Rules& r) {
    return (r.allowInShroud ? 1u : 0u) | (r.allowInHollowHalls ? 2u : 0u) | (r.disableAutoClose ? 4u : 0u);
}
inline Rules FromBits(uint32_t bits) { return {(bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0}; }

// "/tps2 <bits>" with the host's rules.
inline int FormatAnswer(char* buf, size_t cap, const Rules& r) {
    return std::snprintf(buf, cap, "%.*s%u", int(kAnswerPrefix.size()), kAnswerPrefix.data(), Bits(r));
}
inline bool ParseAnswer(std::string_view line, Rules& out) {
    if (!line.starts_with(kAnswerPrefix)) return false;
    line.remove_prefix(kAnswerPrefix.size());
    uint32_t bits = 0;
    if (line.empty() || line.size() > 3) return false;
    for (char c : line) {
        if (c < '0' || c > '9') return false;
        bits = bits * 10 + uint32_t(c - '0');
    }
    if (bits > 7) return false;
    out = FromBits(bits);
    return true;
}

// The host's rules as last answered (0: none yet, so the defaults apply).
inline std::atomic<uint32_t> g_hostBits{0};
inline std::atomic<bool> g_hostKnown{false};
inline void SetHost(const Rules& r) { g_hostBits = Bits(r); g_hostKnown = true; }
inline void ForgetHost() { g_hostKnown = false; }

// The rules in force: this machine's own file when it hosts the world (or plays alone), the
// host's otherwise; the defaults until a host that predates them has not answered.
inline Rules Current(bool hostsWorld) {
    if (hostsWorld) return Local();
    return g_hostKnown.load() ? FromBits(g_hostBits.load()) : Rules{};
}

} // namespace portal::rules
