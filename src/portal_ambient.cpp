// The sound a portal gives off where it stands (portal_ambient.h).
//
// Played through XAudio2 (loaded at run time from xaudio2_9.dll, part of Windows 10 and later):
// one looping mono voice whose volume and left/right balance follow the nearest portal end.
// The loop is read from greeneye\town-portal\portal-ambient.wav (16-bit PCM, any rate, mono or
// stereo); without it the portals are silent. Volume: full within 2 m of an end, nothing past
// 12 m, times the game's Master and Ambience volumes (Steam's cloud copy of the game's
// user_settings.json, read again when it changes).
#define NOMINMAX
#include "portal_ambient.h"
#include "portal_travel.h"
#include "portal_types.h"
#include "greeneye.h"

#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace portal::ambient {
namespace {

constexpr float kNear = 2.f, kFar = 12.f; // was 22 m; the user wanted it to fall off twice as fast (2026-11-10)
constexpr float kLevel = 0.07f; // right by a portal: 7% of the loop file's level (user, 2026-11-10: 35%, then a fifth of that)

IXAudio2* g_engine{};
IXAudio2MasteringVoice* g_master{};
IXAudio2SourceVoice* g_voice{};
HMODULE g_library{};
std::vector<int16_t> g_samples; // mono
uint32_t g_rate{};
bool g_failed{}, g_started{};
uint32_t g_outputs{2};
float g_gain{}, g_pan{};

void Log(const char* line) { greeneye::Host()->log(line); }

std::wstring GameFolder() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    return path;
}

// 16-bit PCM WAV, mixed down to mono.
bool LoadLoop() {
    const std::wstring file = GameFolder() + L"greeneye\\town-portal\\portal-ambient.wav";
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) data.insert(data.end(), chunk, chunk + n);
    fclose(f);
    if (data.size() < 44 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4)) return false;
    uint16_t channels = 0, bits = 0, format = 0;
    uint32_t rate = 0;
    size_t at = 12;
    while (at + 8 <= data.size()) {
        uint32_t size = 0;
        memcpy(&size, data.data() + at + 4, 4);
        const uint8_t* body = data.data() + at + 8;
        if (at + 8 + size > data.size()) break;
        if (!memcmp(data.data() + at, "fmt ", 4) && size >= 16) {
            memcpy(&format, body, 2); memcpy(&channels, body + 2, 2); memcpy(&rate, body + 4, 4); memcpy(&bits, body + 14, 2);
        } else if (!memcmp(data.data() + at, "data", 4) && format == 1 && bits == 16 && channels >= 1 && channels <= 2) {
            const size_t frames = size / (2 * channels);
            g_samples.resize(frames);
            const auto* in = reinterpret_cast<const int16_t*>(body);
            for (size_t i = 0; i < frames; ++i)
                g_samples[i] = channels == 1 ? in[i] : int16_t((int(in[2 * i]) + int(in[2 * i + 1])) / 2);
            g_rate = rate;
            return frames > 0;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

bool Start() {
    if (g_started || g_failed) return g_started;
    g_failed = true; // until it has all worked
    if (!LoadLoop()) return false;
    g_library = LoadLibraryW(L"xaudio2_9.dll");
    if (!g_library) { Log("Town Portal: no XAudio2 on this system; portals give off no sound"); return false; }
    using CreateFn = HRESULT(WINAPI*)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(g_library, "XAudio2Create"));
    if (!create || FAILED(create(&g_engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !g_engine) return false;
    if (FAILED(g_engine->CreateMasteringVoice(&g_master)) || !g_master) return false;
    XAUDIO2_VOICE_DETAILS details{};
    g_master->GetVoiceDetails(&details);
    g_outputs = std::clamp<uint32_t>(details.InputChannels, 1, 8);
    WAVEFORMATEX wave{};
    wave.wFormatTag = WAVE_FORMAT_PCM; wave.nChannels = 1; wave.nSamplesPerSec = g_rate;
    wave.wBitsPerSample = 16; wave.nBlockAlign = 2; wave.nAvgBytesPerSec = g_rate * 2;
    if (FAILED(g_engine->CreateSourceVoice(&g_voice, &wave)) || !g_voice) return false;
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = UINT32(g_samples.size() * 2);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(g_samples.data());
    buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
    if (FAILED(g_voice->SubmitSourceBuffer(&buffer))) return false;
    g_voice->SetVolume(0.f);
    if (FAILED(g_voice->Start())) return false;
    g_failed = false;
    g_started = true;
    Log("Town Portal: portals give off their sound (portal-ambient.wav)");
    return true;
}

// The game's volumes: "1", "0", or a float's bits in hex ("3f000000" = 0.5).
float ParseVolume(const std::string& json, const char* key) {
    const auto at = json.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return 1.f;
    const auto open = json.find('"', json.find(':', at) + 1);
    const auto close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
    if (close == std::string::npos) return 1.f;
    const std::string value = json.substr(open + 1, close - open - 1);
    if (value.size() == 8) {
        uint32_t bits = std::strtoul(value.c_str(), nullptr, 16);
        float f; memcpy(&f, &bits, 4);
        return std::isfinite(f) ? std::clamp(f, 0.f, 1.f) : 1.f;
    }
    return std::clamp(float(std::atof(value.c_str())), 0.f, 1.f);
}

// The game's Master x Ambience volume, from the newest user_settings.json of this game in Steam's
// userdata (Steam is two folders above steamapps\common\Enshrouded).
float GameVolume(uint64_t now) {
    static uint64_t checked = 0;
    static float volume = 1.f;
    static FILETIME seen{};
    if (now - checked < 2000) return volume;
    checked = now;
    std::wstring steam = GameFolder();
    for (int up = 0; up < 3; ++up) { // Enshrouded -> common -> steamapps -> Steam
        steam.pop_back();
        const auto slash = steam.find_last_of(L'\\');
        if (slash == std::wstring::npos) return volume;
        steam.resize(slash + 1);
    }
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((steam + L"userdata\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return volume;
    std::wstring newest;
    FILETIME newestTime{};
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        const std::wstring file = steam + L"userdata\\" + found.cFileName + L"\\1203620\\remote\\user_settings.json";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) && CompareFileTime(&info.ftLastWriteTime, &newestTime) > 0) {
            newest = file;
            newestTime = info.ftLastWriteTime;
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (newest.empty()) {
        static bool told = false;
        if (!told) { told = true; Log("Town Portal: no user_settings.json found; portal sound plays at full level"); }
        return volume;
    }
    if (CompareFileTime(&newestTime, &seen) == 0) return volume;
    seen = newestTime;
    FILE* f = _wfopen(newest.c_str(), L"rb");
    if (!f) return volume;
    std::string json;
    char chunk[4096];
    for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0;) json.append(chunk, n);
    fclose(f);
    volume = ParseVolume(json, "masterVolume") * ParseVolume(json, "ambienceVolume");
    char line[120];
    std::snprintf(line, sizeof(line), "Town Portal: portal sound follows the game's volumes: master x ambience = %.2f", volume);
    Log(line);
    return volume;
}

} // namespace

void Update(uint64_t now, float x, float y, float z, float rightX, float rightZ, bool playing) {
    static uint64_t retryAt = 0;
    if (!g_started) {
        if (g_failed || now < retryAt) return;
        retryAt = now + 5000;
        if (!Start()) { g_failed = false; return; } // the file may be put there later
    }
    // The nearest end of an open portal.
    float best = 1e9f, side = 0.f;
    if (playing) {
        std::vector<PortalInstance> portals;
        travel::GetActivePortals(portals);
        for (const auto& p : portals) {
            if (!p.active || p.closing) continue;
            for (const PortalEndpoint* e : {&p.fieldEnd, &p.baseEnd}) {
                if (!e->wx && !e->wz) continue;
                const float dx = e->wx - x, dy = e->wy + 1.62f - y, dz = e->wz - z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d < best) {
                    best = d;
                    const float flat = std::sqrt(dx * dx + dz * dz);
                    side = flat > 0.5f ? (dx * rightX + dz * rightZ) / flat : 0.f;
                }
            }
        }
    }
    float target = 0.f;
    if (best < kFar) {
        const float f = std::clamp(1.f - (best - kNear) / (kFar - kNear), 0.f, 1.f);
        target = f * f * kLevel * GameVolume(now);
    }
    // Silent while the game is in the background, like the game's own sounds.
    DWORD owner = 0;
    if (HWND front = GetForegroundWindow()) GetWindowThreadProcessId(front, &owner);
    if (owner != GetCurrentProcessId()) target = 0.f, g_gain = 0.f;
    // Eased so it never steps, and the balance only moves when it can be heard.
    g_gain += (target - g_gain) * 0.08f;
    if (target > 0.f) g_pan += (std::clamp(side, -1.f, 1.f) * 0.7f - g_pan) * 0.15f;
    if (!g_voice) return;
    g_voice->SetVolume(g_gain);
    float matrix[8]{};
    if (g_outputs >= 2) {
        matrix[0] = std::sqrt(0.5f * (1.f - g_pan));
        matrix[1] = std::sqrt(0.5f * (1.f + g_pan));
    } else {
        matrix[0] = 1.f;
    }
    g_voice->SetOutputMatrix(g_master, 1, g_outputs, matrix);
}

void Stop() {
    if (g_voice) { g_voice->Stop(); g_voice->DestroyVoice(); g_voice = nullptr; }
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_engine) { g_engine->Release(); g_engine = nullptr; }
    if (g_library) { FreeLibrary(g_library); g_library = nullptr; }
    g_samples.clear();
    g_started = false;
    g_failed = false;
    g_gain = 0.f;
}

} // namespace portal::ambient
