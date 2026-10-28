#pragma once
// Whether portals draw their lighter version, for PCs running the game on low settings:
// [Controls] PortalQuality in town-portal.ini (Auto, Low or High; Auto when missing), and for
// Auto the game's own graphics settings (Saved Games\keen games\Enshrouded\local_settings.json,
// graphics.render). The lighter version: no portal light on the surroundings (its separate
// full-screen pass), the picture through the portal traced in fewer depth steps, and the steam
// round it marched in fewer steps.
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include "portal_rules.h"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace portal::quality {

// From local_settings.json's graphics.render: the preset Lowest, Low or Medium, or (a custom mix)
// at least two of shadows, dynamicGi (Indirect Lighting), reflections and fog at a low setting
// (Off, Lowest, Low, Medium, Performance: the game's names, seen 2026-10-28).
inline bool LowFromGameSettings(const std::string& text) {
    auto value = [&](const char* key) {
        const std::string quoted = std::string("\"") + key + "\"";
        const size_t at = text.find(quoted);
        if (at == std::string::npos) return std::string();
        const size_t open = text.find('"', text.find(':', at + quoted.size()));
        const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
        return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
    };
    auto low = [](const std::string& v) {
        return v == "Off" || v == "Disabled" || v == "Lowest" || v == "Low" || v == "Medium" || v == "Performance";
    };
    const std::string preset = value("preset");
    if (preset == "Lowest" || preset == "Low" || preset == "Medium") return true;
    if (preset == "High" || preset == "Ultra") return false;
    int lowered = 0;
    for (const char* key : {"shadows", "dynamicGi", "reflections", "fog"}) lowered += low(value(key));
    return lowered >= 2;
}

// Read again at most every 5 s; logs each change through log (may be null).
inline bool Low(void (*log)(const char*) = nullptr) {
    static bool low = false;
    static int lastMode = -1;
    static uint64_t readAt = 0;
    const uint64_t now = GetTickCount64();
    if (readAt && now - readAt <= 5000) return low;
    readAt = now;
    wchar_t mode[16]{};
    GetPrivateProfileStringW(L"Controls", L"PortalQuality", L"Auto", mode, 16, rules::Path().c_str());
    bool next = false;
    const char* why = "";
    if (!_wcsicmp(mode, L"Low")) { next = true; why = "PortalQuality=Low"; }
    else if (!_wcsicmp(mode, L"High")) { next = false; why = "PortalQuality=High"; }
    else {
        PWSTR games = nullptr;
        std::string text;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &games))) {
            const std::wstring path = std::wstring(games) + L"\\keen games\\Enshrouded\\local_settings.json";
            if (FILE* f = _wfopen(path.c_str(), L"rb")) {
                text.resize(65536);
                text.resize(std::fread(text.data(), 1, text.size(), f));
                std::fclose(f);
            }
        }
        if (games) CoTaskMemFree(games);
        next = !text.empty() && LowFromGameSettings(text);
        why = text.empty() ? "game settings not found" : "the game's graphics settings";
    }
    const int state = next ? 1 : 0;
    if (log && state != lastMode) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "Town Portal: %s portals (%s)", next ? "lighter" : "full", why);
        log(line);
    }
    lastMode = state;
    low = next;
    return low;
}
}
