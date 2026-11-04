#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "portal_build.h"
#include "portal_natives.h"
#include "portal_natives_table.h"

namespace portal::natives {
namespace {
struct Entry {
    const char* name;
    const char* signature;
    uint32_t offset;
    bool follow;
    uint32_t key; // the address in the reference build; 0 for a layout guard
    uint8_t* found;
};
#define PORTAL_NATIVE_ENTRY(name, signature, offset, follow, key) {#name, signature, offset, follow != 0, key, nullptr},
Entry g_entries[] = {PORTAL_NATIVE_TABLE(PORTAL_NATIVE_ENTRY)};
#undef PORTAL_NATIVE_ENTRY
bool g_renderReady = false, g_tripped = false, g_armed = false;

std::vector<int> Parse(const char* text) {
    std::vector<int> bytes;
    for (const char* p = text; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?') { bytes.push_back(-1); p += 2; continue; }
        char hex[3]{p[0], p[1], 0};
        bytes.push_back(static_cast<int>(std::strtoul(hex, nullptr, 16)));
        p += 2;
    }
    return bytes;
}

bool TextSection(uint8_t*& start, size_t& size) {
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(section[i].Name, ".text", 6) == 0) {
            start = exe + section[i].VirtualAddress;
            size = section[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// The crash guard's file, beside the mod's ini: "<stamp> running" until the session has played a while,
// "<stamp> ok" after.
std::wstring GuardPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    return std::wstring(path) + L"greeneye\\town-portal\\untested-build.txt";
}
uint32_t Stamp() {
    auto* exe = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    return reinterpret_cast<IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew)->FileHeader.TimeDateStamp;
}
void WriteGuard(const char* state) {
    if (FILE* f = _wfopen(GuardPath().c_str(), L"wb")) {
        std::fprintf(f, "%08x %s\n", Stamp(), state);
        std::fclose(f);
    }
}
bool KnownBuild() {
#ifdef TOWN_PORTAL_DEV
    // Dev builds can act as if the game had been patched: greeneye\town-portal\pretend-untested.txt
    std::wstring pretend = GuardPath();
    pretend.replace(pretend.rfind(L"untested-build.txt"), 18, L"pretend-untested.txt");
    if (GetFileAttributesW(pretend.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
#endif
    return build::Client();
}
void CheckCrashGuard(const GreeneyeHost* host) {
    if (KnownBuild()) return; // the build this version was made for
    char line[64]{};
    if (FILE* f = _wfopen(GuardPath().c_str(), L"rb")) {
        std::fread(line, 1, sizeof(line) - 1, f);
        std::fclose(f);
    }
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "%08x ", Stamp());
    const bool same = std::strncmp(line, stamp, 9) == 0;
    if (same && std::strstr(line, "ok")) return;
    // A generation after the first is a reload, not a new session.
    if (same && std::strstr(line, "running") && host->generation <= 1) {
        g_tripped = true;
        host->log("Town Portal: the last session on this untested game build ended early, so the native features stay off on it; delete greeneye\\town-portal\\untested-build.txt to try again");
        return;
    }
    WriteGuard("running");
    g_armed = true;
}
}

void Resolve(const GreeneyeHost* host) {
    g_renderReady = false;
    g_tripped = false;
    for (auto& e : g_entries) e.found = nullptr;
    CheckCrashGuard(host);
    if (g_tripped) return;
    uint8_t* text{};
    size_t size{};
    if (!TextSection(text, size)) {
        host->log("Town Portal: no code section found; native features off");
        return;
    }
    // The code as it was before any hook, searched once for all signatures by first byte.
    std::vector<uint8_t> code(size);
    host->read(text, code.data(), size);
    const size_t count = sizeof(g_entries) / sizeof(g_entries[0]);
    std::vector<std::vector<int>> patterns(count);
    std::vector<uint32_t> bucket[256];
    size_t longest = 0;
    for (size_t i = 0; i < count; ++i) {
        patterns[i] = Parse(g_entries[i].signature);
        bucket[patterns[i][0]].push_back(static_cast<uint32_t>(i));
        if (patterns[i].size() > longest) longest = patterns[i].size();
    }
    std::vector<const uint8_t*> hit(count, nullptr);
    std::vector<uint8_t> hits(count, 0);
    for (size_t at = 0; at + longest <= size; ++at) {
        const auto& candidates = bucket[code[at]];
        for (uint32_t i : candidates) {
            const auto& pattern = patterns[i];
            size_t k = 1;
            while (k < pattern.size() && (pattern[k] < 0 || code[at + k] == pattern[k])) ++k;
            if (k == pattern.size()) {
                hit[i] = code.data() + at;
                if (hits[i] < 2) ++hits[i];
            }
        }
    }
    std::string missing;
    size_t found = 0;
    for (size_t i = 0; i < count; ++i) {
        auto& e = g_entries[i];
        uint8_t* live = nullptr;
        if (hits[i] == 1) {
            live = text + (hit[i] - code.data());
        } else if (!hits[i]) {
            live = host->find(e.signature, nullptr); // behind another mod's hook
        }
        if (live) {
            live += e.offset;
            if (e.follow) {
                uint8_t call[5]{};
                host->read(live, call, 5);
                int32_t delta = 0;
                std::memcpy(&delta, call + 1, 4);
                live = call[0] == 0xe8 ? live + 5 + delta : nullptr;
            }
        }
        e.found = live;
        if (live) {
            ++found;
        } else {
            if (!missing.empty()) missing += ' ';
            missing += e.name;
        }
    }
    g_renderReady = missing.empty();
    char line[1024];
    if (missing.empty())
        std::snprintf(line, sizeof(line), "Town Portal: all %zu native functions and layout checks found%s", count,
                      KnownBuild() ? "" : " on this untested game build");
    else
        std::snprintf(line, sizeof(line), "Town Portal: %zu of %zu native checks not found, native rendering off: %s", count - found, count, missing.c_str());
    host->log(line);
}

uint8_t* At(uint32_t referenceRva) {
    for (const auto& e : g_entries)
        if (e.key == referenceRva) return e.found;
    return nullptr;
}

bool Has(std::initializer_list<uint32_t> referenceRvas) {
    for (uint32_t rva : referenceRvas)
        if (!At(rva)) return false;
    return true;
}

bool RenderReady() { return g_renderReady; }
bool Tripped() { return g_tripped; }

void NoteStablePlay() {
    if (!g_armed) return;
    g_armed = false;
    WriteGuard("ok");
}
}
