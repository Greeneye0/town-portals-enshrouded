#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include "portal_render.h"
#include "portal_travel.h"
#include "portal_math.h"
#include "portal_scene_probe.h"
#include "portal_text.h"
#include "portal_sounds.h"
#include "portal_adpcm.h"
#include "portal_fizzle.h"
#include "portal_colors.h"
#include "portal_names.h"
#include "portal_rules.h"
#include "portal_map_warp.h"
#include "portal_map_labels.h"
#include "portal_default_images.h"
#include <array>
#include <limits>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>



namespace portal::render {
namespace {

template <class T>
T& at(void* p, size_t n) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + n);
}

struct UiText {
    const char* bytes;
    size_t length;
    uint64_t hash;
};

UiText MakeText(const char* s) {
    size_t len = s ? std::strlen(s) : 0;
    UiText t{s ? s : "", len, 0xcbf29ce484222325ull};
    for (size_t i = 0; i < len; ++i) {
        t.hash = (t.hash ^ static_cast<uint8_t>(s[i])) * 0x100000001b3ull;
    }
    return t;
}

// 64-byte descriptor passed to Enshrouded's native dynamic RGBA8 UI texture uploader (0xa41b30)
struct DynamicUiTextureDesc {
    uint8_t flags;       // +0x00: 0
    uint8_t pad0[7];     // +0x01
    const void* pixels;  // +0x08: pointer to RGBA8 pixel buffer (width * height * 4 bytes)
    uint64_t byteSize;   // +0x10: width * height * 4
    uint64_t keyLo;      // +0x18: unique 24-byte key part 1
    uint64_t keyHi;      // +0x20: unique 24-byte key part 2
    uint64_t keySub;     // +0x28: unique 24-byte key part 3 (version / frame index)
    uint32_t width;      // +0x30: width in pixels
    uint32_t height;     // +0x34: height in pixels
    uint8_t format;      // +0x38: 0x25 (RGBA8_UNORM)
    uint8_t pad1[7];     // +0x39
};
static_assert(sizeof(DynamicUiTextureDesc) == 0x40, "DynamicUiTextureDesc must be 0x40 bytes");

using BeginWidgetFn = void (*)(void*, uint64_t);
using EndWidgetFn = void (*)(void*);
using SetSizeFn = void (*)(void*, uint64_t);
using BeginLayerFn = void* (*)(void*, const UiText*, uint8_t, uint32_t, int32_t, uint32_t);
using EndLayerFn = void (*)(void*);
using NamedWidgetFn = void* (*)(void*, const UiText*);
using OpaqueWidgetFn = void* (*)(void*);
using BeginLabelFn = void (*)(uint8_t*, const UiText*, const uint8_t*, uint8_t);
using EndLabelFn = void (*)(uint8_t*);
using UiCtxFn = void* (*)();
using ImageFn = uint32_t* (*)(void*, void*, const void*);
using DrawImageFn = void (*)(void*, const float*, const uint32_t*, const uint64_t*);
using DrawRealFn = void (*)(void*, const float*, uint32_t, uint64_t);
using DrawImageSubUvFn = void (*)(void*, const float*, uint32_t, uint64_t, uint64_t, uint64_t);
using NineSliceFn = void (*)(void*, const float*, const void*, const float*, uint64_t);
using CreateDynamicTextureFn = uint32_t (*)(void* assetRepo, const DynamicUiTextureDesc* desc);
using LocaTextFn = UiText* (*)(UiText*, uint32_t);

BeginWidgetFn g_begin = nullptr;
EndWidgetFn g_end = nullptr;
SetSizeFn g_setSize = nullptr;
// The game's own interaction prompt (doInteractionPrompt, evidence:
// C:\temp\enshrouded-re\out\interact-prompt): DrawActionHintRow(action, style) draws the
// action's bound key - key cap, rebinds and gamepad icons included - then the label.
// Action 15 is GameplayAction::ContextualAction, the "E" interaction.
using HintRowFn = void (*)(uint8_t action, void* style);
using HintStyleInitFn = void* (*)(void* style, bool enabled);
HintRowFn g_hintRow = nullptr;
HintStyleInitFn g_hintStyleInit = nullptr;
constexpr uint8_t kInteractAction = 15;
BeginLayerFn g_beginLayer = nullptr;
EndLayerFn g_endLayer = nullptr;
NamedWidgetFn g_named = nullptr;
OpaqueWidgetFn g_opaque = nullptr;
BeginLabelFn g_beginLabel = nullptr;
EndLabelFn g_endLabel = nullptr;
UiCtxFn g_uiCtx = nullptr;
ImageFn g_image = nullptr;
DrawImageFn g_drawImage = nullptr;
DrawRealFn g_drawReal = nullptr;
DrawImageSubUvFn g_drawImageSubUv = nullptr;
NineSliceFn g_nineSlice = nullptr;
CreateDynamicTextureFn g_createDynamicTexture = nullptr;
LocaTextFn g_loca = nullptr;
// FbUiLocaLinks (assets + 0x10) -> rpgTerms (+0x40) -> rarityLegendary (+0x1dd0): a 16-byte key.
constexpr size_t kLocaLegendary = 0x10 + 0x40 + 0x1dd0;
std::atomic<int> g_language{-1}; // -1: not known yet (English)
uint64_t g_nextLanguageCheck = 0;
void** g_root = nullptr;
bool g_ready = false;

// UI Asset offsets (FbUiPrimitives at assets + 0xd250)
constexpr size_t kAssetCircle = 0xd250 + 0x00;
constexpr size_t kAssetSoftGlow = 0xd250 + 0x20;
constexpr size_t kAssetRingGlow = 0xd250 + 0x30;
constexpr size_t kAssetBoxInnerGlow = 0xd250 + 0x120;
constexpr size_t kAssetGlowBoxSmall = 0xd250 + 0x140;
constexpr size_t kAssetRoundedBox = 0xd250 + 0x180;
constexpr size_t kAssetSmallText = 0x10a0c;

// Portal sounds: two parts of the portal whoosh the user chose (tools/build-portal-sounds.py),
// played with Windows audio. The game's own UI sound player (evidence:
// C:\temp\enshrouded-re\out\sound-fasttravel) is kept for the dev sound audition; its
// instance table has no lock, so those requests are queued and run from the HUD frame.
using PlayUiSoundFn = void (*)(void* gameUi, const uint8_t* ref);
PlayUiSoundFn g_playUiSound = nullptr;
uint32_t g_fastTravelSound = 0; // FbUiBundle offset, read from the game's own call site
std::mutex g_soundMutex;
std::vector<int32_t> g_soundOps; // offsets from fastTravelTriggered
void PlayEffect(const uint8_t* wav) {
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
#ifdef TOWN_PORTAL_DEV
// Dev audition: Ctrl+Shift+1..0 play these FbUiSounds entries (offsets from
// fastTravelTriggered at bundle+0x129f0; C:\temp\enshrouded-re\out\sound-fasttravel\types-dump.txt).
struct SoundCandidate { const char* name; int32_t delta; };
constexpr SoundCandidate kSoundCandidates[] = {
    {"1 map fastTravelTriggered", 0},
    {"2 map goToHomeBase", 0x12a10 - 0x129f0},
    {"3 events magicBarrierUnlocked", 0x12100 - 0x129f0},
    {"4 events ancientBarrierUnlocked", 0x12110 - 0x129f0},
    {"5 events secretDoorUnlocked", 0x12150 - 0x129f0},
    {"6 events flameLevelUp", 0x120f0 - 0x129f0},
    {"7 events locationDiscovery", 0x12080 - 0x129f0},
    {"8 menu flameAltarOpen", 0x12240 - 0x129f0},
    {"9 menu activatePersonalProgression", 0x12480 - 0x129f0},
    {"0 menu shroudForgeOpen", 0x12550 - 0x129f0},
};
bool g_auditionDown[10]{};
#endif

// Arrival loading overlay
std::atomic<bool> g_loading{false};
std::atomic<uint64_t> g_loadingChanged{0};
std::atomic<uint32_t> g_loadingTint{0};
std::mutex g_loadingMutex;
std::string g_loadingCaption;
constexpr float kLoadingInMs=350.0f,kLoadingOutMs=600.0f;
std::atomic<bool> g_blink{false};
std::atomic<uint64_t> g_blinkChanged{0};
std::atomic<uint32_t> g_blinkColour{0xff100805u};
constexpr float kBlinkInMs=150.0f,kBlinkOutMs=250.0f;
float BlinkAlpha(uint64_t now) {
    float since=float(now-std::min(now,g_blinkChanged.load()));
    return g_blink ? std::min(1.0f,since/kBlinkInMs) : std::max(0.0f,1.0f-since/kBlinkOutMs);
}
// The trip shown on the loading screen's map (SetLoadingRoute), under g_loadingMutex.
struct LoadingRoute { float fieldX{}, fieldZ{}, altarX{}, altarZ{}; bool toAltar{}, set{}; } g_loadingRoute;
std::atomic<float> g_loadingProgress{0.0f};
// What the screen shows of it (HUD thread): eased toward the estimate, so the path and the
// percentage glide past its stage jumps, and on to 100% as the screen leaves.
float g_shownProgress = 0.0f;
uint64_t g_shownProgressAt = 0;
float LoadingAlpha(uint64_t now) {
    float since=float(now-std::min(now,g_loadingChanged.load()));
    return g_loading ? std::min(1.0f,since/kLoadingInMs) : std::max(0.0f,1.0f-since/kLoadingOutMs);
}

// Notice banner state
std::mutex g_noticeMutex;
std::string g_noticeTitle;
std::string g_noticeBody;
uint64_t g_noticeUntil = 0;

uint64_t PackXY(float x, float y) {
    float xy[2] = {x, y};
    uint64_t v = 0;
    std::memcpy(&v, xy, 8);
    return v;
}

uint64_t PackColor(uint32_t abgr, float alpha, float brightness = 1.0f) {
    float a = std::max(0.0f, std::min(1.0f, alpha));
    uint32_t aByte = static_cast<uint32_t>(((abgr >> 24) & 0xffu) * a + 0.5f);
    struct {
        uint32_t rgba;
        float bright;
    } c{(abgr & 0x00ffffffu) | (aByte << 24), brightness};
    uint64_t packed = 0;
    std::memcpy(&packed, &c, 8);
    return packed;
}

struct Widget {
    alignas(16) uint8_t raw[0x80]{};
    uint8_t* Handle() { return *reinterpret_cast<uint8_t**>(raw + 8); }
};

uint8_t* Assets() {
    void* g = g_root ? *g_root : nullptr;
    return g ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 200) : nullptr;
}

uint32_t Fnv1a32(const uint8_t* p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x1000193u;
    return h;
}

// The game's language can change while it runs; look at its word for "Legendary" once a second.
void DetectLanguage(uint8_t* assets, uint64_t now) {
    if (!g_loca || !assets || now < g_nextLanguageCheck) return;
    g_nextLanguageCheck = now + 1000;
    UiText result = MakeText("");
    g_loca(&result, Fnv1a32(assets + kLocaLegendary, 16));
    if (!result.bytes || !result.length) return;
    std::string word(result.bytes, std::min(result.length, size_t(64)));
    text::Lang lang{};
    if (!text::Detect(word, lang) || int(lang) == g_language.load()) return;
    g_language = int(lang);
    char line[160]{};
    std::snprintf(line, sizeof(line), "Town Portal: notices in %s (the game's \"Legendary\": %s)",
                  text::kCodes[size_t(lang)], word.c_str());
    if (auto* host = greeneye::Host(); host && host->log) host->log(line);
}

} // namespace

// The game's text lookup only works on the UI thread: elsewhere (the gameplay tick) the
// global it reads is null and it faults. Off the UI thread GameText answers from this
// cache, and an id it has not seen is queued for the UI thread to look up (ResolveTexts).
namespace {
struct CachedText { uint32_t id = 0; int language = -1; char text[128]{}; };
CachedText g_texts[64];
uint32_t g_pendingTexts[16]{};
std::mutex g_textLock;
std::atomic<DWORD> g_uiThread{0};

bool LookUp(uint32_t locaId, char* out, size_t cap) {
    UiText result = MakeText("");
    g_loca(&result, locaId);
    if (!result.bytes || !result.length || result.length >= cap) return false;
    std::memcpy(out, result.bytes, result.length);
    out[result.length] = 0;
    return true;
}

bool LookUpGuarded(uint32_t locaId, char* out, size_t cap) {
    __try {
        return LookUp(locaId, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void StoreText(uint32_t locaId, const char* text) {
    std::lock_guard lock(g_textLock);
    CachedText* slot = &g_texts[locaId % 64];
    slot->id = locaId;
    slot->language = g_language.load();
    std::snprintf(slot->text, sizeof(slot->text), "%s", text);
}

// On the UI thread: look up the ids the gameplay tick asked for.
void ResolveTexts() {
    g_uiThread = GetCurrentThreadId();
    uint32_t pending[16]{};
    {
        std::lock_guard lock(g_textLock);
        std::memcpy(pending, g_pendingTexts, sizeof(pending));
        std::memset(g_pendingTexts, 0, sizeof(g_pendingTexts));
    }
    for (uint32_t id : pending) {
        char text[128]{};
        if (id && g_loca && LookUpGuarded(id, text, sizeof(text))) StoreText(id, text);
    }
}
} // namespace

bool GameText(uint32_t locaId, char* out, size_t cap) {
    if (!g_loca || !out || cap < 2) return false;
    if (GetCurrentThreadId() == g_uiThread.load()) {
        if (!LookUpGuarded(locaId, out, cap)) return false;
        StoreText(locaId, out);
        return true;
    }
    std::lock_guard lock(g_textLock);
    const CachedText& slot = g_texts[locaId % 64];
    if (slot.id == locaId && slot.language == g_language.load() && std::strlen(slot.text) < cap) {
        std::snprintf(out, cap, "%s", slot.text);
        return true;
    }
    for (uint32_t& p : g_pendingTexts)
        if (p == locaId) return false;
    for (uint32_t& p : g_pendingTexts)
        if (!p) { p = locaId; break; }
    return false;
}

namespace {
void* AssetRepo() {
    if (!g_uiCtx) return nullptr;
    auto* ctx = static_cast<uint8_t*>(g_uiCtx());
    uint8_t* p10 = ctx ? *reinterpret_cast<uint8_t**>(ctx + 0x10) : nullptr;
    uint8_t* p58 = p10 ? *reinterpret_cast<uint8_t**>(p10 + 0x58) : nullptr;
    return p58 ? *reinterpret_cast<void**>(p58 + 0xe98) : nullptr;
}

uint32_t AssetImageId(size_t offset) {
    uint8_t* assets = Assets();
    if (!assets || !g_image || !g_uiCtx) return 0;
    alignas(16) uint8_t tmp[16]{};
    uint32_t* id = g_image(g_uiCtx(), tmp, assets + offset);
    return id ? *id : 0;
}

void DrawSolidRect(void* widgetRaw, float x, float y, float w, float h, uint32_t abgr, float alpha = 1.0f, float bright = 1.0f) {
    if (!widgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.002f) return;
    float rect[4] = {x, y, w, h};
    uint64_t col = PackColor(abgr, alpha, bright);
    if (g_drawReal) {
        g_drawReal(widgetRaw, rect, 0, col);
    } else if (g_drawImage) {
        uint32_t zero = 0;
        g_drawImage(widgetRaw, rect, &zero, &col);
    }
}

void DrawAssetQuad(void* widgetRaw, uint32_t imageId, float cx, float cy, float w, float h,
                   uint32_t abgr, float alpha, float bright = 1.0f) {
    if (!widgetRaw || !imageId || w <= 0.0f || h <= 0.0f || alpha <= 0.002f) return;
    float rect[4] = {cx - w * 0.5f, cy - h * 0.5f, w, h};
    uint64_t col = PackColor(abgr, alpha, bright);
    if (g_drawReal) {
        g_drawReal(widgetRaw, rect, imageId, col);
    } else if (g_drawImage) {
        g_drawImage(widgetRaw, rect, &imageId, &col);
    }
}

void* WidgetImageRegistry(void* widgetRaw) {
    // 1273043: 118ca30/118f140 resolve the current widget's UI renderer;
    // 118f190 looks images up in that renderer's first pointer.
    auto* header=at<uint8_t*>(widgetRaw,8);
    auto* owner=header?at<uint8_t*>(header,0x50):nullptr;
    auto* layer=owner?at<uint8_t*>(owner,0x18):nullptr;
    if(!layer||at<uint32_t>(layer,8)!=4)return nullptr;
    auto* renderer=at<uint8_t*>(layer,0x6ce0);
    return renderer?at<void*>(renderer,0):nullptr;
}

void DrawAssetSubUv(void* widgetRaw, uint32_t imageId, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    uint32_t abgr, float alpha, float bright = 1.0f) {
    if (!widgetRaw || !imageId || w <= 0.0f || h <= 0.0f || alpha <= 0.002f) return;
    float rect[4] = {x, y, w, h};
    uint64_t col = PackColor(abgr, alpha, bright);
    if (g_drawImageSubUv) {
        g_drawImageSubUv(widgetRaw, rect, imageId, PackXY(u0, v0), PackXY(u1, v1), col);
    } else if (g_drawReal) {
        g_drawReal(widgetRaw, rect, imageId, col);
    }
}

void DrawNineSliceBox(void* widgetRaw, size_t assetOffset, float x, float y, float w, float h,
                      uint32_t abgr, float alpha, float border, float bright = 1.0f) {
    uint8_t* assets = Assets();
    if (!assets || !widgetRaw || w <= 0.0f || h <= 0.0f || alpha <= 0.002f) return;
    if (!g_nineSlice) {
        DrawSolidRect(widgetRaw, x, y, w, h, abgr, alpha, bright);
        return;
    }
    float b = std::min({border, w * 0.5f, h * 0.5f});
    float borders[4] = {b, b, b, b};
    float rect[4] = {x, y, w, h};
    g_nineSlice(widgetRaw, rect, assets + assetOffset, borders, PackColor(abgr, alpha, bright));
}

// FbUiBundle.namedStyles (+0x10a0c, UiCharacterStyle entries of 0x58 bytes: fontSize at
// +0x20, dropShadow at +0x44): the game's interaction prompts use the outlined default
// style, larger than the default text.
constexpr size_t kStyleOutline = kAssetSmallText + 0x58;
alignas(16) uint8_t g_promptStyle[0x58]{};
const uint8_t* PromptStyle(uint8_t* assets) {
    std::memcpy(g_promptStyle, assets + kStyleOutline, sizeof(g_promptStyle));
    *reinterpret_cast<float*>(g_promptStyle + 0x20) *= 1.3f;
    g_promptStyle[0x44] = 0;
    return g_promptStyle;
}

// alignH: 2 centres the text in the box; 0 starts it at the left edge. style: a
// UiCharacterStyle, or null for the default text.
void DrawLabel(float absX, float absY, float w, float h, const char* textStr, uint32_t abgr, float alpha, uint8_t alignH,
               const uint8_t* style = nullptr) {
    uint8_t* assets = Assets();
    if (!assets || !g_beginLabel || !g_endLabel || !textStr || !*textStr) return;
    UiText text = MakeText(textStr);
    alignas(16) uint8_t label[0x400]{};
    g_beginLabel(label, &text, style ? style : assets + kAssetSmallText, 0);
    if (uint8_t* lh = *reinterpret_cast<uint8_t**>(label + 8)) {
        *reinterpret_cast<float*>(lh + 0x114) = absX;
        *reinterpret_cast<float*>(lh + 0x118) = absY - 1.5f;
        *reinterpret_cast<float*>(lh + 0x11c) = w;
        *reinterpret_cast<float*>(lh + 0x120) = h;
    }
    label[0x38] = alignH;
    label[0x39] = 2; // Center V
    label[0x3c] = 0; // Single-line
    label[0x3d] = 1;
    *reinterpret_cast<uint32_t*>(label + 0xb0) = abgr;
    *reinterpret_cast<float*>(label + 0xb4) = alpha;
    g_endLabel(label);
}

void DrawLabelCentered(float absX, float absY, float w, float h, const char* textStr, uint32_t abgr, float alpha = 1.0f) {
    DrawLabel(absX, absY, w, h, textStr, abgr, alpha, 2);
}

// 3D Camera & Perspective Projection Frame
struct CameraFrame {
    float view[16]{};
    float proj[16]{};
    float viewport[4]{};
    float camX = 0.0f;
    float camY = 0.0f;
    float camZ = 0.0f;
    float fwdX = 0.0f;
    float fwdY = 0.0f;
    float fwdZ = 1.0f;

    bool Capture(uint8_t* uiClient) {
        void* g = g_root ? *g_root : nullptr;
        if (!g || !uiClient) return false;
        auto* window = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g) + 0x10);
        if (!window) return false;
        std::memcpy(view, uiClient + 0x1d20, sizeof(view));
        std::memcpy(proj, uiClient + 0x1d60, sizeof(proj));
        std::memcpy(viewport, window + 0x18, sizeof(viewport));
        if (!(viewport[2] >= 64.0f && viewport[3] >= 64.0f)) return false;

        camX = -(view[0] * view[12] + view[1] * view[13] + view[2] * view[14]);
        camY = -(view[4] * view[12] + view[5] * view[13] + view[6] * view[14]);
        camZ = -(view[8] * view[12] + view[9] * view[13] + view[10] * view[14]);
        fwdX = view[2];
        fwdY = view[6];
        fwdZ = view[10];
        return std::isfinite(camX) && std::isfinite(camY) && std::isfinite(camZ);
    }

    bool Project(float wx, float wy, float wz, float& sx, float& sy, float& depth) const {
        depth = wx * view[2] + wy * view[6] + wz * view[10] + view[14];
        if (!std::isfinite(depth) || depth <= 0.08f) return false;
        float inv = 1.0f / depth;
        float vx = inv * (wx * view[0] + wy * view[4] + wz * view[8] + view[12]);
        float vy = inv * (wx * view[1] + wy * view[5] + wz * view[9] + view[13]);
        sx = ((vy * proj[4] + vx * proj[0] + inv * depth * proj[8] + proj[12]) * 0.5f + 0.5f) * viewport[2] + viewport[0];
        sy = (0.5f - (vy * proj[5] + vx * proj[1] + inv * depth * proj[9] + proj[13]) * 0.5f) * viewport[3] + viewport[1];
        return std::isfinite(sx) && std::isfinite(sy);
    }
};

using math::Vec3;
using math::Visibility;

constexpr uint32_t kFrameDim = 768;
// Transparent guard band keeps curved glow away from the rectangular tile clip.
constexpr float kTextureExtent = 1.16f;
constexpr size_t kFramePixels = size_t(kFrameDim) * kFrameDim;
constexpr int kFrameCount = 24;
constexpr int kOccCols = 6;
constexpr int kOccRows = 10;
constexpr int kMaxVisibleEndpoints = 4;
constexpr float kPi = 3.14159265359f;
std::array<std::vector<uint32_t>, kFrameCount> g_frames;
std::array<uint32_t, kFrameCount> g_textureIds{};
void* g_textureRepo = nullptr;
uint64_t g_textureGeneration = 0;
uint8_t* g_observedAssets=nullptr;
uint64_t g_hudFrame=0,g_compositeDrawnFrame=~0ull;
// The local player's player-list id: their own portals take their minimap colour (gold).
uint32_t g_localPlayerId=0;
uint64_t g_observedAssetsTick=0;

struct Metrics {
    uint64_t since{}, frames{}, rays{}, quads{}, uploads{}, deferred{};
    double ms{}, maxMs{};
} g_metrics;
Metrics g_lifetimeMetrics{};
LARGE_INTEGER g_frequency{};

double CounterMs() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return double(value.QuadPart) * 1000.0 / double(g_frequency.QuadPart);
}

struct Cell {
    Vec3 from{}, to{};
    uint64_t tick{};
    Visibility visibility = Visibility::unknown;
};
struct EndpointCache {
    uint64_t owner{}, id{}, lastUsed{};
    bool field{};
    std::array<Cell, kOccCols*kOccRows> cells{};
};
std::array<EndpointCache, kMaxPortals*2> g_occlusion{};
std::vector<RemotePlayerPos> g_remotePlayers;
travel::VisibilityContext g_visibilityContext{};
int g_queriesLeft = 0;
double g_queryDeadline = 0;

uint32_t Rgba(float r, float g, float b, float a) {
    auto byte=[](float c) { return uint32_t(std::clamp(c,0.0f,1.0f)*255.0f+0.5f); };
    return byte(r) | (byte(g)<<8) | (byte(b)<<16) | (byte(a)<<24);
}
float Smooth(float a, float b, float x) {
    float t=std::clamp((x-a)/(b-a),0.0f,1.0f);
    return t*t*(3-2*t);
}

// All expensive texture synthesis happens once at load, never in the HUD loop.
// Integer angular harmonics make the last/first animation frames continuous.
void BuildFrames() {
    QueryPerformanceFrequency(&g_frequency);
    g_textureGeneration = GetTickCount64();
    for (int f=0;f<kFrameCount;++f) {
        auto& pixels=g_frames[f];
        pixels.resize(kFramePixels);
        float phase=2*kPi*float(f)/float(kFrameCount);
        for (uint32_t y=0;y<kFrameDim;++y) {
            float v=((float(y)+0.5f)*2/float(kFrameDim)-1)*kTextureExtent;
            for (uint32_t x=0;x<kFrameDim;++x) {
                float u=((float(x)+0.5f)*2/float(kFrameDim)-1)*kTextureExtent;
                float r=std::sqrt(u*u+v*v);
                float angle=std::atan2(v,u);
                float wave=std::sin(angle*7+phase*2+r*19)*0.55f+
                           std::sin(angle*13-phase*3-r*27)*0.30f+
                           std::sin(angle*23+phase+r*41)*0.15f;
                float band=std::exp(-std::pow((r-0.884f-wave*0.005f)/0.023f,2.0f));
                float halo=std::exp(-std::pow((r-0.886f)/0.065f,2.0f));
                float filament=std::exp(-std::pow((r-0.884f-wave*0.006f)/0.007f,2.0f));
                float thread=std::pow(0.5f+0.5f*std::sin(angle*5+phase-r*29),8.0f);
                float swirl=0.5f+0.5f*std::sin(r*22-angle*3-phase*2+
                              0.65f*std::sin(angle*6+phase));
                float membrane=1-Smooth(0.855f,0.885f,r);
                float edge=1-Smooth(0.93f,0.995f,r);
                float alpha=std::max(membrane*0.97f,(band*0.85f+halo*0.25f))*edge;
                // Intensity only: each draw tints it with the owner's colour.
                float intensity=0.16f+0.14f*swirl+halo*0.30f+band*0.55f+filament*0.3f;
                // Fine moving strands soften the opaque membrane without pretending to be a camera feed.
                float strands=thread*std::pow(std::max(0.0f,r),3.0f)*membrane;
                intensity=std::min(1.0f,intensity+strands*0.11f);
                // Native UI uses premultiplied alpha. Transparent texels must
                // carry zero RGB or each rectangular strip leaks a coloured box.
                pixels[size_t(y)*kFrameDim+x]=Rgba(intensity*alpha,intensity*alpha,intensity*alpha,alpha);
            }
        }
    }
}

// The loading screen's map (greeneye\town-portal\map\map.jpg, tools/map/make_map.py; labels drawn on it) and
// its markers: loaded once, on the first trip; -1 when the file is missing (the swirl is shown).
std::atomic<int> g_mapState{0};
std::vector<uint32_t> g_mapPixels, g_mapEnPixels; // labels drawn on it; English: its own lettering
std::vector<uint32_t> g_mapLabeledPixels; // g_mapPixels with the labels written in (BakeMapLabels)
// The backdrop round the map (tools/map/make_backdrop.py): the game's menus' navy with ring arcs.
std::vector<uint32_t> g_backdropPixels;
uint32_t g_backdropW = 0, g_backdropH = 0;
uint32_t g_mapLabeledGeneration = 0;
uint32_t g_mapSize = 0;
constexpr uint32_t kDotDim = 64;
std::vector<uint32_t> g_dotPixels, g_ringPixels, g_discPixels; // disc: soft, filled, for the route's strokes
// The field portal on the map, drawn like the portal in the world: an upright oval with a
// glowing rim, mist drifting inside it (kMistFrames looping frames) and motes rising round it.
// White intensity with alpha, so each draw tints it with the portal's colour.
constexpr uint32_t kOvalW = 96, kOvalH = 144;
constexpr int kMistFrames = 16;
std::vector<uint32_t> g_ovalRimPixels;
std::array<std::vector<uint32_t>, kMistFrames> g_mistPixels;
// The Flame Altar icon (tools/map/make_altar_icon.py): whole, and as its stone base and its
// flame, which flickers over the base.
std::vector<uint32_t> g_altarPixels, g_altarBasePixels, g_altarFlamePixels;
uint32_t g_altarDim = 0;
void LoadMap() {
    if (g_mapState.load() != 0) return;
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    wcscat_s(path, L"greeneye\\town-portal\\map\\map.jpg");
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!defaults::LoadRgba(path, w, h, rgba) || w != h || w > 2048) {
        g_mapState = -1;
        if (auto* host = greeneye::Host()) host->log("Town Portal: loading-screen map not found (map\\map.jpg); the swirl is shown");
        return;
    }
    g_mapPixels.resize(size_t(w) * h);
    std::memcpy(g_mapPixels.data(), rgba.data(), rgba.size()); // opaque: premultiplied as it is
    g_mapSize = w;
    {
        wchar_t* file = wcsrchr(path, L'\\');
        wcscpy_s(file + 1, MAX_PATH - (file + 1 - path), L"map-en.jpg");
        std::vector<uint8_t> en;
        uint32_t ew = 0, eh = 0;
        if (defaults::LoadRgba(path, ew, eh, en) && ew == w && eh == h) {
            g_mapEnPixels.resize(size_t(w) * h);
            std::memcpy(g_mapEnPixels.data(), en.data(), en.size());
        }
        wcscpy_s(file + 1, MAX_PATH - (file + 1 - path), L"backdrop.jpg");
        if (defaults::LoadRgba(path, ew, eh, en) && ew <= 2048 && eh <= 2048) {
            g_backdropPixels.resize(size_t(ew) * eh);
            std::memcpy(g_backdropPixels.data(), en.data(), en.size());
            g_backdropW = ew;
            g_backdropH = eh;
        }
    }
    // The Flame Altar icon's images, premultiplied for the UI; all the same size.
    wchar_t* name = wcsrchr(path, L'\\');
    auto icon = [&](const wchar_t* file, std::vector<uint32_t>& out) {
        wcscpy_s(name + 1, MAX_PATH - (name + 1 - path), file);
        if (!defaults::LoadRgba(path, w, h, rgba) || w != h || w > 256 || (g_altarDim && w != g_altarDim)) return;
        out.resize(size_t(w) * h);
        for (size_t i = 0; i < out.size(); ++i) {
            const float a = rgba[i * 4 + 3] / 255.0f;
            out[i] = Rgba(rgba[i * 4] / 255.0f * a, rgba[i * 4 + 1] / 255.0f * a, rgba[i * 4 + 2] / 255.0f * a, a);
        }
        g_altarDim = w;
    };
    icon(L"altar.png", g_altarPixels);
    icon(L"altar-base.png", g_altarBasePixels);
    icon(L"altar-flame.png", g_altarFlamePixels);
    // Markers, white so each draw tints them: a filled dot with a dark rim, and a ring.
    g_discPixels.assign(size_t(kDotDim) * kDotDim, 0);
    g_dotPixels.assign(size_t(kDotDim) * kDotDim, 0);
    g_ringPixels.assign(size_t(kDotDim) * kDotDim, 0);
    for (uint32_t y = 0; y < kDotDim; ++y)
        for (uint32_t x = 0; x < kDotDim; ++x) {
            const float dx = (x + 0.5f) / kDotDim * 2 - 1, dy = (y + 0.5f) / kDotDim * 2 - 1, r = std::sqrt(dx * dx + dy * dy);
            const float body = 1 - Smooth(0.62f, 0.70f, r), rim = (1 - Smooth(0.86f, 0.96f, r)) * (1 - body);
            const float a = std::max(body, rim * 0.85f), c = body; // rim dark, body white
            g_dotPixels[size_t(y) * kDotDim + x] = Rgba(c * a, c * a, c * a, a);
            const float disc = 1 - Smooth(0.80f, 1.0f, r);
            g_discPixels[size_t(y) * kDotDim + x] = Rgba(disc, disc, disc, disc);
            const float ring = Smooth(0.70f, 0.76f, r) * (1 - Smooth(0.88f, 0.95f, r));
            g_ringPixels[size_t(y) * kDotDim + x] = Rgba(ring, ring, ring, ring);
        }
    // The oval: u, v in -1..1 across the texture; the portal's edge where the ellipse radius is 0.86.
    auto noise = [](float x, float y) { // smooth value noise, 0..1
        auto hash = [](int i, int j) { uint32_t h = uint32_t(i) * 374761393u + uint32_t(j) * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return float(h & 0xffff) / 65535.0f; };
        const int i = int(std::floor(x)), j = int(std::floor(y));
        const float fx = x - i, fy = y - j, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        const float a = hash(i, j), b = hash(i + 1, j), c = hash(i, j + 1), d = hash(i + 1, j + 1);
        return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
    };
    g_ovalRimPixels.assign(size_t(kOvalW) * kOvalH, 0);
    for (auto& frame : g_mistPixels) frame.assign(size_t(kOvalW) * kOvalH, 0);
    for (uint32_t y = 0; y < kOvalH; ++y)
        for (uint32_t x = 0; x < kOvalW; ++x) {
            const float u = (x + 0.5f) / kOvalW * 2 - 1, v = (y + 0.5f) / kOvalH * 2 - 1, r = std::sqrt(u * u + v * v);
            const float rim = std::exp(-std::pow((r - 0.86f) / 0.035f, 2.0f)), halo = std::exp(-std::pow((r - 0.86f) / 0.12f, 2.0f));
            const float a = std::min(1.0f, rim + halo * 0.45f);
            const float c = std::min(1.0f, rim * 1.0f + halo * 0.6f);
            g_ovalRimPixels[size_t(y) * kOvalW + x] = Rgba(c * a, c * a, c * a, a);
            const float inside = 1 - Smooth(0.70f, 0.86f, r);
            if (inside <= 0) continue;
            for (int f = 0; f < kMistFrames; ++f) {
                // Two noise layers drifting upward at different speeds, looping over the frames.
                const float t = float(f) / kMistFrames;
                const float n1 = noise(u * 2.2f + 7, v * 2.2f + t * 4.0f), n1b = noise(u * 2.2f + 7, v * 2.2f + t * 4.0f - 4.0f);
                const float n2 = noise(u * 4.5f + 3, v * 4.5f + t * 9.0f), n2b = noise(u * 4.5f + 3, v * 4.5f + t * 9.0f - 9.0f);
                const float m = (n1 * (1 - t) + n1b * t) * 0.65f + (n2 * (1 - t) + n2b * t) * 0.35f; // crossfade: seamless loop
                const float wisp = Smooth(0.35f, 0.85f, m);                                       // visible wisps, not a flat haze
                const float glow = 0.12f + 0.80f * wisp + 0.30f * (1 - r);                          // brighter toward the middle
                const float alpha = inside * (0.78f + 0.20f * wisp);
                g_mistPixels[f][size_t(y) * kOvalW + x] = Rgba(glow * alpha, glow * alpha, glow * alpha, alpha);
            }
        }
    g_mapState = 1;
}

uint32_t SafeTexture(void* repo, const DynamicUiTextureDesc& desc) {
    __try { return g_createDynamicTexture(repo,&desc); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
uint32_t FrameTexture(int index) {
    void* repo=AssetRepo();
    if (!repo || !g_createDynamicTexture) return 0;
    if (repo!=g_textureRepo) {
        g_textureIds.fill(0);
        g_textureRepo=repo;
    }
    // Ask the uploader every frame instead of caching its ID. For a resident
    // key it only refreshes the texture's last-use stamp and returns the ID
    // (0x140a40000); an evicted one is uploaded again. A cached ID went stale
    // once the game evicted the frames (e.g. after travel) and drew solid white.
    DynamicUiTextureDesc desc{};
    desc.pixels=g_frames[index].data();
    desc.byteSize=kFramePixels*sizeof(uint32_t);
    desc.keyLo=0x544f574e504f5254ull;
    desc.keyHi=0x454e455247593032ull ^ g_textureGeneration;
    desc.keySub=uint64_t(index+1);
    desc.width=kFrameDim;
    desc.height=kFrameDim;
    desc.format=0x25;
    uint32_t id=SafeTexture(repo,desc);
    // A new ID means the key was (re)created; count it as an upload.
    if (id && id!=g_textureIds[index]) ++g_metrics.uploads;
    g_textureIds[index]=id;
    return id;
}

// A loading-map texture by its sub-key (1 map, 10 it labelled, 11 the backdrop, 8 the English map, 9 a soft disc, 2 dot, 3 ring, 4 altar, 5 its
// base, 6 its flame, 7 the portal oval's rim, 16.. its mist frames), refreshed like FrameTexture.
uint32_t MapTexture(uint64_t sub) {
    void* repo = AssetRepo();
    if (g_mapState.load() != 1 || !repo || !g_createDynamicTexture) return 0;
    const bool oval = sub == 7 || (sub >= 16 && sub < 16 + kMistFrames);
    const auto& pixels = sub == 11 ? g_backdropPixels : sub == 10 ? g_mapLabeledPixels : sub == 1 ? g_mapPixels : sub == 8 ? g_mapEnPixels : sub == 9 ? g_discPixels : sub == 2 ? g_dotPixels : sub == 3 ? g_ringPixels :
                         sub == 4 ? g_altarPixels : sub == 5 ? g_altarBasePixels : sub == 6 ? g_altarFlamePixels :
                         sub == 7 ? g_ovalRimPixels : g_mistPixels[(sub - 16) % kMistFrames];
    const uint32_t dim = sub == 1 || sub == 8 || sub == 10 ? g_mapSize : sub == 9 ? kDotDim : sub >= 4 ? g_altarDim : kDotDim;
    if (pixels.empty()) return 0;
    DynamicUiTextureDesc desc{};
    desc.pixels = pixels.data();
    desc.byteSize = uint32_t(pixels.size() * sizeof(uint32_t));
    desc.keyLo = 0x544f574e504f5254ull;
    desc.keyHi = 0x4d41504d41503031ull ^ g_textureGeneration; // "MAPMAP01"
    desc.keySub = sub == 10 ? sub | uint64_t(g_mapLabeledGeneration) << 8 : sub; // relabelled: a new texture
    desc.width = oval ? kOvalW : sub == 11 ? g_backdropW : dim;
    desc.height = oval ? kOvalH : sub == 11 ? g_backdropH : dim;
    desc.format = 0x25;
    return SafeTexture(repo, desc);
}

// The map's labels in the game's language (portal_map_labels.h), looked up again every few
// seconds while the game's text loads, and when the language changes. True when they changed.
struct MapLabelText { std::array<std::string, std::size(maplabels::kLabels)> text; uint64_t at{}; int language = -1; bool complete{}; } g_mapLabelText;
bool RefreshMapLabels(uint64_t now) {
    auto& cache = g_mapLabelText;
    const int language = int(Language());
    if (cache.language == language && (cache.complete || now - cache.at < 2000)) return false;
    const auto before = cache.text;
    const bool languageChanged = cache.language != language;
    cache.language = language;
    cache.at = now;
    cache.complete = true;
    for (size_t i = 0; i < std::size(maplabels::kLabels); ++i) {
        const auto& label = maplabels::kLabels[i];
        char name[128]{}, line[192]{};
        const bool ready = label.kind == maplabels::region || label.kind == maplabels::spire
            ? names::RegionTitle(int(label.value), name, sizeof(name))
            : names::PlaceTitle(label.value, label.english, name, sizeof(name));
        cache.complete = cache.complete && ready;
        // Spire scrolls are small and the spire is painted beside them: the place's name alone.
        std::snprintf(line, sizeof(line), "%s", name);
        cache.text[i] = line;
    }
    return languageChanged || cache.text != before;
}

// The labels are written into a copy of the blank map with Windows' text renderer, in dark ink,
// each as tall as the painted text was and narrower when a translation is long; the map then
// shows them at every zoom. (The game's own label styles fault in its text code when made
// smaller than their size, 2026-10-28.) A font that has the script: Palatino Linotype for Latin
// and Cyrillic, and the Windows fonts for Thai, Japanese, Korean and Chinese.
const wchar_t* MapFont(text::Lang language) {
    switch (language) {
    case text::Lang::th: return L"Leelawadee UI";
    case text::Lang::ja: return L"Yu Mincho";
    case text::Lang::ko: return L"Malgun Gothic";
    case text::Lang::zhHans: return L"Microsoft YaHei";
    case text::Lang::zhHant: return L"Microsoft JhengHei";
    default: return L"Palatino Linotype";
    }
}
void BakeMapLabels() {
    if (g_mapPixels.empty() || !g_mapSize) return;
    g_mapLabeledPixels = g_mapPixels;
    const int dim = int(g_mapSize);
    const wchar_t* face = MapFont(Language());
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return;
    for (size_t i = 0; i < std::size(maplabels::kLabels); ++i) {
        const auto& label = maplabels::kLabels[i];
        const std::string& line = g_mapLabelText.text[i];
        if (line.empty()) continue;
        wchar_t wide[192]{};
        if (!MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide, 192)) continue;
        // The text may run a little past the scroll's straight middle onto its curls.
        const int room = std::max(8, int(label.room * dim * 1.2f));
        // The painted capitals with descenders were `height`: about 1.15 em. Never below a
        // readable size: a long name then runs onto the scroll's ends rather than shrinking away.
        constexpr int kSmallest = 12;
        int px = std::max(kSmallest, int(label.height * dim * 1.25f));
        HFONT font = nullptr;
        SIZE extent{};
        for (; px >= kSmallest; px -= 1) { // narrower when long
            if (font) DeleteObject(font);
            // Semibold suits the large scrolls; small text stays regular so thin scripts stay clear.
            font = CreateFontW(-px, 0, 0, 0, px >= 18 ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
            SelectObject(dc, font);
            GetTextExtentPoint32W(dc, wide, int(wcslen(wide)), &extent);
            if (extent.cx <= room) break;
        }
        const int w = extent.cx + 8, h = extent.cy + 4;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            HGDIOBJ old = SelectObject(dc, bitmap);
            std::memset(bits, 0, size_t(w) * h * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 4, 2, wide, int(wcslen(wide)));
            GdiFlush();
            // White on black: the coverage, laid on the parchment in ink.
            const int x0 = int(label.u * dim) - w / 2, y0 = int(label.v * dim) - h / 2;
            const auto* src = static_cast<const uint8_t*>(bits);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int mx = x0 + x, my = y0 + y;
                    if (mx < 0 || my < 0 || mx >= dim || my >= dim) continue;
                    const float a = src[(size_t(y) * w + x) * 4 + 1] / 255.0f * 0.92f;
                    if (a <= 0) continue;
                    uint32_t& p = g_mapLabeledPixels[size_t(my) * dim + mx];
                    const float ink[3] = {58, 34, 20}; // dark brown, as the painted lettering
                    uint32_t out = p & 0xff000000u;
                    for (int c = 0; c < 3; ++c) {
                        const float v = float((p >> (c * 8)) & 255);
                        out |= uint32_t(v + (ink[c] - v) * a + 0.5f) << (c * 8);
                    }
                    p = out;
                }
            SelectObject(dc, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (font) DeleteObject(font);
    }
    DeleteDC(dc);
    ++g_mapLabeledGeneration;
}

// The field portal on the loading map, like the portal in the world: soft light, mist drifting
// inside an upright oval, the glowing rim, and motes rising round it, all in the portal's colour.
void DrawMapPortal(void* widget, float cx, float cy, float scale, uint32_t tint, float alpha, uint64_t now) {
    const float h = 62 * scale, w = h * float(kOvalW) / float(kOvalH);
    const uint32_t mist = MapTexture(16 + (now / 70) % kMistFrames), rim = MapTexture(7), dot = MapTexture(2);
    if (!mist || !rim) return;
    DrawAssetQuad(widget, dot, cx, cy, h * 1.5f, h * 1.5f, tint, alpha * 0.18f); // light round it
    DrawAssetSubUv(widget, mist, cx - w * 0.5f, cy - h * 0.5f, w, h, 0, 0, 1, 1, tint, alpha * 0.9f, 1.15f);
    DrawAssetSubUv(widget, rim, cx - w * 0.5f, cy - h * 0.5f, w, h, 0, 0, 1, 1, tint, alpha, 1.8f);
    // Motes: each rises from a point on the rim, fading in and out, then starts again elsewhere.
    for (int i = 0; i < 12; ++i) {
        const float period = 1.6f + 0.13f * float(i % 5), phase = float(i) * 0.37f;
        const float t = float(now % 1000000) * 0.001f / period + phase, life = t - std::floor(t);
        const uint32_t cycle = uint32_t(std::floor(t)) * 2654435761u + uint32_t(i) * 40503u;
        const float angle = float(cycle % 6283) * 0.001f;
        const float sx = cx + std::sin(angle) * w * 0.47f, sy = cy + std::cos(angle) * h * 0.45f - life * h * 0.45f;
        const float fade = std::sin(life * 3.14159f), size = (2.0f + float(cycle % 3)) * scale;
        DrawAssetQuad(widget, dot, sx + std::sin(life * 6 + i) * 2 * scale, sy, size, size, tint, alpha * fade, 2.2f);
    }
}

// The loading screen's map: the world map zoomed to the trip (whole map for a long one, at
// most about 3.3x for a short one), both portal ends, and red dashes marching from where the
// trip starts toward where it leads, reaching it as the loading does (progress 0..1). False
// when the map is not available (the swirl is drawn instead).
bool DrawLoadingMap(void* widget, float ox, float oy, float sw, float sh, float alpha, uint64_t now,
                    float progress, uint32_t tint, float& bottom) {
    LoadingRoute route;
    {
        std::lock_guard guard(g_loadingMutex);
        route = g_loadingRoute;
    }
    // English: the painting as supplied, with its own lettering; otherwise the blank one, labelled.
    const uint32_t english = route.set && Language() == text::Lang::en ? MapTexture(8) : 0;
    if (route.set && !english && g_mapState.load() == 1 && (RefreshMapLabels(now) || g_mapLabeledPixels.empty())) BakeMapLabels();
    const uint32_t labelled = route.set && !english && !g_mapLabeledPixels.empty() ? MapTexture(10) : 0;
    const uint32_t map = english ? english : labelled ? labelled : route.set ? MapTexture(1) : 0;
    if (!map) return false;
    double fu, fv, au, av;
    mapwarp::ToMap(route.fieldX, route.fieldZ, fu, fv);
    mapwarp::ToMap(route.altarX, route.altarZ, au, av);
    double span = std::max(std::fabs(fu - au), std::fabs(fv - av)) * 1.7 + 0.06;
    span = std::clamp(span, 0.3, 1.0); // at most ~3.3x: the painting is 1254 px across
    // Zoomed in, the panel widens to the screen's shape so the map around the trip shows too
    // (the whole map, being square, stays square).
    const float side = std::min(sh * 0.8f, sw * 0.9f);
    const double spanU = std::min(1.0, span * std::max(1.0, double(sw * 0.94f) / side));
    const float width = side * float(spanU / span);
    const double u0 = std::clamp((fu + au) * 0.5 - spanU * 0.5, 0.0, 1.0 - spanU);
    const double v0 = std::clamp((fv + av) * 0.5 - span * 0.5, 0.0, 1.0 - span);
    const float x = ox + (sw - width) * 0.5f, y = oy + (sh - side) * 0.5f - 24;
    // A gold frame, as the game's menus edge their panels: a dark line, the gold, a soft inner glint.
    auto frame = [&](float inset, float thick, uint32_t colour, float a) {
        DrawSolidRect(widget, x - inset, y - inset, width + inset * 2, thick, colour, alpha * a);
        DrawSolidRect(widget, x - inset, y + side + inset - thick, width + inset * 2, thick, colour, alpha * a);
        DrawSolidRect(widget, x - inset, y - inset, thick, side + inset * 2, colour, alpha * a);
        DrawSolidRect(widget, x + width + inset - thick, y - inset, thick, side + inset * 2, colour, alpha * a);
    };
    const float scaleFrame = side / 1000.0f;
    frame(6 * scaleFrame, 6 * scaleFrame, 0xff0a0806u, 0.8f);   // dark outer edge
    frame(4.5f * scaleFrame, 3 * scaleFrame, 0xff3ab4d6u, 1.0f); // gold
    frame(3 * scaleFrame, 1 * scaleFrame, 0xff9ce6f5u, 0.7f);    // glint
    DrawAssetSubUv(widget, map, x, y, width, side, float(u0), float(v0), float(u0 + spanU), float(v0 + span), 0xffffffffu, alpha);
    auto screen = [&](double u, double v, float& sx, float& sy) {
        sx = x + float((u - u0) / span) * side;
        sy = y + float((v - v0) / span) * side;
    };
    float fx, fy, ax, ay;
    screen(fu, fv, fx, fy);
    screen(au, av, ax, ay);
    // From where the trip starts to where it leads.
    const float sx = route.toAltar ? fx : ax, sy = route.toAltar ? fy : ay;
    const float ex = route.toAltar ? ax : fx, ey = route.toAltar ? ay : fy;
    const float dx = ex - sx, dy = ey - sy, length = std::sqrt(dx * dx + dy * dy);
    const float scale = side / 1000.0f;
    if (length > 1.0f) {
        // Each dash a rounded stroke: soft discs packed along it, the dark edge under all of the
        // red (drawn first), so it reads as one smooth line at any angle, not a row of beads.
        const float dash = 18 * scale, period = 30 * scale, thick = 6 * scale, step = std::max(0.75f, thick * 0.22f);
        const float march = std::fmod(float(now % 100000) * 0.045f * scale, period);
        const float drawn = length * std::clamp(progress, 0.0f, 1.0f);
        const uint32_t disc = MapTexture(9);
        for (int pass = 0; pass < 2 && disc; ++pass) {
            const float size = pass ? thick : thick * 1.7f;
            const uint32_t colour = pass ? 0xff2b2be0u : 0xff100805u;
            for (float t = march - period; t < drawn; t += period) // dashes move toward the destination
                for (float s = std::max(0.0f, t); s <= std::min(drawn, t + dash); s += step)
                    DrawAssetQuad(widget, disc, sx + dx * s / length, sy + dy * s / length, size, size, colour,
                                  pass ? alpha : alpha * 0.6f);
        }
    }
    // The field portal as the portal itself (DrawMapPortal), in its colour. At the altar end a
    // little Flame Altar standing on its spot, its flame flickering (else a ringed dot). Where the
    // trip leads pulses.
    const uint32_t dotTexture = MapTexture(2), ringTexture = MapTexture(3), altarTexture = MapTexture(4);
    const uint32_t baseTexture = MapTexture(5), flameTexture = MapTexture(6);
    const float marker = 30 * scale, pulse = 0.5f + 0.5f * std::sin(float(now % 100000) * 0.006f);
    if (MapTexture(7))
        DrawMapPortal(widget, fx, fy - 22 * scale, scale, tint, alpha, now); // standing on its spot
    else
        DrawAssetQuad(widget, dotTexture, fx, fy, marker, marker, tint, alpha);
    if (baseTexture && flameTexture) {
        const float icon = 64 * scale, top = ay - icon * 0.88f;           // the icon's top-left: centred, its foot on the spot
        const float t = float(now % 100000) * 0.001f;
        // Flicker: a few unrelated waves, so it never visibly repeats.
        const float n = 0.5f * std::sin(t * 11.3f) + 0.3f * std::sin(t * 17.9f + 1.3f) + 0.2f * std::sin(t * 29.1f + 2.1f);
        const float tall = 1.0f + 0.10f * n, wide = 1.0f - 0.04f * n, sway = 1.2f * scale * std::sin(t * 5.3f);
        const float foot = top + icon * 0.566f;                           // where the flame meets the bowl
        DrawAssetQuad(widget, dotTexture, ax, foot - icon * 0.30f, icon * 0.9f, icon * 0.9f, 0xff2a8cffu, alpha * (0.22f + 0.08f * n)); // warm glow
        DrawAssetSubUv(widget, flameTexture, ax - icon * wide * 0.5f + sway, foot - icon * 0.566f * tall, icon * wide, icon * tall,
                       0, 0, 1, 1, 0xffffffffu, alpha, 1.0f + 0.25f * (0.5f + 0.5f * n));
        DrawAssetQuad(widget, baseTexture, ax, top + icon * 0.5f, icon, icon, 0xffffffffu, alpha);
    } else if (altarTexture) {
        const float icon = 64 * scale;
        DrawAssetQuad(widget, altarTexture, ax, ay - icon * 0.38f, icon, icon, 0xffffffffu, alpha);
    } else {
        DrawAssetQuad(widget, dotTexture, ax, ay, marker, marker, tint, alpha);
        DrawAssetQuad(widget, ringTexture, ax, ay, marker * 1.7f, marker * 1.7f, 0xffffffffu, alpha);
    }
    if (progress >= 1.0f) // the destination pulses once the path has reached it
        DrawAssetQuad(widget, ringTexture, ex, ey, marker * (2.0f + pulse), marker * (2.0f + pulse), tint, alpha * (1 - pulse * 0.6f));
    bottom = y + side;
    return true;
}

EndpointCache& CacheFor(const PortalInstance& portal, bool field, uint64_t now) {
    EndpointCache* oldest=&g_occlusion[0];
    for (auto& cache:g_occlusion) {
        if (cache.id==portal.portalId && cache.owner==portal.ownerAccount && cache.field==field) {
            cache.lastUsed=now;
            return cache;
        }
        if (cache.lastUsed<oldest->lastUsed) oldest=&cache;
    }
    *oldest={};
    oldest->id=portal.portalId;
    oldest->owner=portal.ownerAccount;
    oldest->field=field;
    oldest->lastUsed=now;
    return *oldest;
}

Visibility QueryCell(Cell& cell, Vec3 from, Vec3 to, uint64_t now) {
    float drift=std::max(math::LengthSq(cell.from-from),math::LengthSq(cell.to-to));
    if (drift<0.0025f && cell.tick && now-cell.tick<100) return cell.visibility;
    if (g_queriesLeft<=0 || CounterMs()>=g_queryDeadline) {
        ++g_metrics.deferred;
        // Only ~5 rays fit in a frame, so hiding every unrefreshed cell punched
        // rectangular holes in the portal. A recent result from a nearby ray
        // stays valid briefly; anything older or farther is still unknown.
        if (cell.tick && now-cell.tick<400 && drift<0.35f*0.35f) return cell.visibility;
        return Visibility::unknown;
    }
    --g_queriesLeft;
    ++g_metrics.rays;
    cell.from=from;
    cell.to=to;
    cell.tick=now;
    cell.visibility=travel::TraceVisibility(g_visibilityContext,from,to);
    return cell.visibility;
}

struct Plane {
    Vec3 center{},normal{},right{};
    float rx{},ry{};
    bool At(const CameraFrame& cam,float sx,float sy,Vec3& world,float& u,float& v) const {
        Vec3 direction{};
        if (!math::ScreenRay(cam.view,cam.proj,cam.viewport,sx,sy,direction) ||
            !math::IntersectPlane({cam.camX,cam.camY,cam.camZ},direction,center,normal,world)) return false;
        Vec3 delta=world-center;
        u=0.5f+math::Dot(delta,right)/(2*rx);
        v=0.5f-delta.y/(2*ry);
        return std::isfinite(u)&&std::isfinite(v);
    }
};

// The tile footprint is measured on the portal plane; a character nearer the
// camera only needs that padding scaled down to its own depth.
bool PlayerBlocks(Vec3 camera,Vec3 point,Vec3 local,float expansion) {
    float rayLen=std::sqrt(math::LengthSq(point-camera));
    auto hits=[&](Vec3 feet) {
        Vec3 body=feet+Vec3{0,1,0};
        float scale=rayLen>0.1f?std::clamp(std::sqrt(math::LengthSq(body-camera))/rayLen,0.0f,1.0f):1.0f;
        return math::SegmentHitsPlayer(camera,point,feet,expansion*scale);
    };
    if (hits(local)) return true;
    for (const auto& p:g_remotePlayers) {
        if (hits({p.wx,p.wy,p.wz})) return true;
    }
    return false;
}

struct Candidate {
    const PortalInstance* portal{};
    bool field{};
    float distSq{};
};

void DrawEndpoint(void* widget, const CameraFrame& cam, const Candidate& candidate,
                  Vec3 local,uint32_t texture,uint64_t now) {
    const auto& portal=*candidate.portal;
    const auto& ep=candidate.field?portal.fieldEnd:portal.baseEnd;
    const uint32_t tint=colors::ForPlayer(portal.ownerPlayerId,g_localPlayerId);
    float scale=1;
    if (portal.closing) scale=1-Smooth(0,500,float(now-std::min(now,portal.closedTick)));
    // The surface grows over the channel after a short spark (GPU path only).
    else scale=Smooth(0.15f*kPortalFormMs,float(kPortalFormMs),float(now-std::min(now,portal.openedTick)));
    // Cull at full size while forming so the GPU spark and streams still draw.
    const bool surface=scale>=0.03f;
    const float size=surface?scale:1.0f;
    if (!surface&&portal.closing) return;
    Plane plane{{ep.wx,ep.wy+1.62f,ep.wz},{ep.nx,0,ep.nz},{ep.rx,0,ep.rz},
                1.02f*size*kTextureExtent,1.42f*size*kTextureExtent};
    Vec3 camera{cam.camX,cam.camY,cam.camZ};
    Vec3 delta=camera-plane.center;
    float dist=std::sqrt(math::LengthSq(delta));
    // A real plane becomes edge-on; it never turns to follow the camera. The
    // GPU lens has thickness, so only the tile fallback skips edge-on views.
    if (dist<0.12f) return;
    const bool edgeOn=std::fabs(math::Dot(delta,plane.normal))/dist<0.055f;
    float left=std::numeric_limits<float>::max(),top=left;
    float right=-left,bottom=-left;
    for (int i=0;i<64;++i) {
        float angle=float(i)*(2*kPi/64);
        Vec3 corner=plane.center+plane.right*(std::cos(angle)*plane.rx)+Vec3{0,std::sin(angle)*plane.ry,0};
        float sx{},sy{},depth{};
        if (!cam.Project(corner.x,corner.y,corner.z,sx,sy,depth)) return;
        left=std::min(left,sx);right=std::max(right,sx);
        top=std::min(top,sy);bottom=std::max(bottom,sy);
    }
    // Pad by the lens thickness so a nearly edge-on portal is not culled.
    left-=24;right+=24;
    if (right<cam.viewport[0] || left>cam.viewport[0]+cam.viewport[2] ||
        bottom<cam.viewport[1] || top>cam.viewport[1]+cam.viewport[3] ||
        bottom-top<6) return;
    // Keep texture lookup and all visibility work AFTER frustum/distance culling.
    if (!texture) return;
    uint32_t liveImage=0;
#ifdef TOWN_PORTAL_DEV
    if(auto* registry=WidgetImageRegistry(widget))liveImage=sceneprobe::LiveImage(portal,candidate.field,registry,now);
#endif
    // GPU depth compositor: per-pixel occlusion against the game's depth
    // buffer. One full-screen image holds every composited endpoint, so draw
    // it once per HUD frame; endpoints it did not render use the tiles below.
    if(auto* registry=WidgetImageRegistry(widget))if(auto composite=sceneprobe::CompositeImage(portal,candidate.field,tint,registry,now)) {
        if(composite==sceneprobe::kDrawnInFrame)return;
        if(g_compositeDrawnFrame!=g_hudFrame) {
            DrawAssetSubUv(widget,composite,cam.viewport[0],cam.viewport[1],cam.viewport[2],cam.viewport[3],0,0,1,1,0xffffffffu,1);
            ++g_metrics.quads;g_compositeDrawnFrame=g_hudFrame;
        }
        return;
    }
    left+=24;right-=24;
    if (!surface||edgeOn||right-left<4) return;
    auto& cache=CacheFor(portal,candidate.field,now);
    bool clear[kOccRows][kOccCols]{};
    const float width=right-left,height=bottom-top;
    // Refresh the stalest cells first. A fixed row-major order always starved
    // the bottom-right cells once the ray budget ran out.
    std::array<int,kOccCols*kOccRows> order{};
    for (int i=0;i<int(order.size());++i) order[i]=i;
    std::stable_sort(order.begin(),order.end(),[&](int a,int b) { return cache.cells[a].tick<cache.cells[b].tick; });
    for (int i:order) {
        int row=i/kOccCols,col=i%kOccCols;
        Vec3 point{};float u{},v{};
        if (!plane.At(cam,left+(float(col)+0.5f)*width/kOccCols,
                         top+(float(row)+0.5f)*height/kOccRows,point,u,v)) continue;
        clear[row][col]=math::CanDraw(QueryCell(cache.cells[i],camera,point,now));
    }
    // Fine screen tiles clip the player's silhouette every frame, independently
    // of the slower world visibility cache. Tile footprint expands the body mask.
    int cols=std::clamp(int(std::ceil(width/12)),8,40);
    int rows=std::clamp(int(std::ceil(height/10)),12,64);
    float alpha=1-Smooth(55,75,dist);
    for (int row=0;row<rows;++row) {
      struct Span { bool active{};float x0{},y0{},x1{},y1{},u0{},v0{},u1{},v1{}; } span;
      auto flush=[&]() {
        if (!span.active) return;
        DrawAssetSubUv(widget,texture,span.x0,span.y0,span.x1-span.x0,span.y1-span.y0,
                       span.u0,span.v0,span.u1,span.v1,tint,alpha);
        ++g_metrics.quads;
        span.active=false;
      };
      for(int col=0;col<cols;++col) {
        float x0=left+float(col)*width/cols,x1=left+float(col+1)*width/cols;
        float y0=top+float(row)*height/rows,y1=top+float(row+1)*height/rows;
        // Entire tile must lie in clear coarse cells; never merge across a wall.
        int c0=std::clamp(col*kOccCols/cols,0,kOccCols-1);
        int c1=std::clamp(((col+1)*kOccCols-1)/cols,0,kOccCols-1);
        int r0=std::clamp(row*kOccRows/rows,0,kOccRows-1);
        int r1=std::clamp(((row+1)*kOccRows-1)/rows,0,kOccRows-1);
        bool visible=true;
        for(int r=r0;r<=r1;++r) for(int c=c0;c<=c1;++c) visible &= clear[r][c];
        if (!visible) continue;
        Vec3 p0{},p1{},pm{},p2{},p3{};
        float u0{},v0{},u1{},v1{},um{},vm{},u2{},v2{},u3{},v3{};
        if (!plane.At(cam,x0,y0,p0,u0,v0) || !plane.At(cam,x1,y1,p1,u1,v1) ||
            !plane.At(cam,(x0+x1)*0.5f,(y0+y1)*0.5f,pm,um,vm) ||
            !plane.At(cam,x1,y0,p2,u2,v2) || !plane.At(cam,x0,y1,p3,u3,v3)) continue;
        if (std::min({u0,u1,u2,u3})<0 || std::max({u0,u1,u2,u3})>1 ||
            std::min({v0,v1,v2,v3})<0 || std::max({v0,v1,v2,v3})>1) continue;
        float footprint=0.5f*std::max(std::sqrt(math::LengthSq(p1-p0)),std::sqrt(math::LengthSq(p3-p2)));
        if (PlayerBlocks(camera,pm,local,footprint)) continue;
        // Reject cells next to a blocked world sample as a conservative guard
        // against geometry edges falling between coarse ray sample centers.
        bool boundary=(c0>0&&!clear[r0][c0-1]) || (c1+1<kOccCols&&!clear[r0][c1+1]) ||
                      (r0>0&&!clear[r0-1][c0]) || (r1+1<kOccRows&&!clear[r1+1][c0]);
        if (boundary) continue;
        if(liveImage) {
            auto radius=[](float u,float v){return (u-.5f)*(u-.5f)*4+(v-.5f)*(v-.5f)*4;};
            if(std::max({radius(u0,v0),radius(u1,v1),radius(u2,v2),radius(u3,v3)})<.66f/(kTextureExtent*kTextureExtent)) {
                DrawAssetSubUv(widget,liveImage,x0,y0,x1-x0,y1-y0,
                    (x0-cam.viewport[0])/cam.viewport[2],(y0-cam.viewport[1])/cam.viewport[3],
                    (x1-cam.viewport[0])/cam.viewport[2],(y1-cam.viewport[1])/cam.viewport[3],0xffffffffu,alpha);
                flush();continue; // Do not cover the live image with the fallback membrane.
            }
        }
        // Merge adjacent visible pieces only while the perspective mapping error
        // stays below half a source texel. Hidden tiles always break the span.
        float expected=span.active?span.u0+(u1-span.u0)*(span.x1-span.x0)/(x1-span.x0):0;
        if (span.active&&std::fabs(x0-span.x1)<0.001f&&
            std::fabs(v0-span.v0)<0.5f/kFrameDim&&std::fabs(v1-span.v1)<0.5f/kFrameDim&&
            std::fabs(expected-span.u1)<0.5f/kFrameDim) {
            span.x1=x1;span.u1=u1;span.v1=v1;
        } else {
            flush();
            span={true,x0,y0,x1,y1,u0,v0,u1,v1};
        }
      }
      flush();
    }
}

// world: the portals themselves, drawn on a layer beneath the game's and other
// mods' HUD; otherwise the prompt and notices, drawn above it.
// InputHintStyle (0x78 bytes): +0x2F auto label (off), +0x3C fade, +0x4C skips the hold-ring
// path, +0x54 key size in px (the game's prompt uses 32), +0x58 the label. The row is drawn in
// its own widget anchored at the point.
bool DrawNativePrompt(float x, float y, const char* label) {
    if (!g_hintRow || !g_hintStyleInit || !g_named || !g_end) return false;
    __try {
        Widget box;
        UiText name = MakeText("townPortalInteraction");
        g_named(box.raw, &name);
        if (uint8_t* node = box.Handle()) {
            *reinterpret_cast<uint16_t*>(node + 0x6e) = 0x0101;
            const float position[2] = {x, y};
            std::memcpy(node + 0x70, position, sizeof(position));
        }
        alignas(16) uint8_t style[0x78]{};
        g_hintStyleInit(style, true);
        style[0x2f] = 0;
        style[0x4c] = 1;
        *reinterpret_cast<float*>(style + 0x3c) = 1.0f;
        *reinterpret_cast<float*>(style + 0x54) = 32.0f;
        *reinterpret_cast<UiText*>(style + 0x58) = MakeText(label);
        g_hintRow(kInteractAction, style);
        g_end(box.raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hintRow = nullptr; // fall back to the mod's own prompt from now on
        return false;
    }
}

struct PromptAnchor { float x = 0, y = 0, z = 0; bool valid = false; };
PromptAnchor g_promptAnchor;
constexpr float kKeyCap = 34;

// Where the prompt's key cap sits: beside the portal it is for, else low in the centre.
void PromptPosition(const CameraFrame& cam, float& sx, float& sy) {
    const float sw=cam.viewport[2],sh=cam.viewport[3],ox=cam.viewport[0],oy=cam.viewport[1];
    sx=ox+sw*0.5f-160;sy=oy+sh-170;
    float depth=0;
    if (g_promptAnchor.valid&&cam.Project(g_promptAnchor.x,g_promptAnchor.y,g_promptAnchor.z,sx,sy,depth)) {
        sx=std::clamp(sx+70,ox+20,ox+sw-560);
        sy=std::clamp(sy,oy+60,oy+sh-120);
    }
}

uint8_t* g_uiClient=nullptr;

// Over each nearby, visible portal end: the owner's name in their colour and, beneath it,
// where that end leads.
// Over each nearby, visible portal end: the owner's name in their colour and, beneath it,
// where that end leads. Nearest first; a label is left out when it would sit on a nearer
// portal or overlap a nearer portal's label.
void DrawPortalLabels(const CameraFrame& cam, const std::vector<PortalInstance>& portals, uint64_t now) {
    Vec3 camera{cam.camX,cam.camY,cam.camZ};
    struct End { const PortalInstance* p; bool field; float distSq, sx, sy, left, top, right, bottom; bool boxed; };
    std::vector<End> ends;
    for (const auto& p : portals) {
        if (p.closing) continue;
        for (bool field : {true,false}) {
            const auto& e=field?p.fieldEnd:p.baseEnd;
            End end{&p,field,math::LengthSq(Vec3{e.wx,e.wy+1.62f,e.wz}-camera),0,0,0,0,0,0,false};
            if (end.distSq>40.0f*40.0f) continue;
            // The portal's screen box, from its top, bottom and sides.
            float xs[4]{},ys[4]{},depth{};
            const Vec3 corners[4]={{e.wx,e.wy+3.0f,e.wz},{e.wx,e.wy+0.2f,e.wz},
                                   {e.wx+e.rx*1.2f,e.wy+1.62f,e.wz+e.rz*1.2f},{e.wx-e.rx*1.2f,e.wy+1.62f,e.wz-e.rz*1.2f}};
            bool all=true;
            for (int i=0;i<4;++i) all&=cam.Project(corners[i].x,corners[i].y,corners[i].z,xs[i],ys[i],depth);
            if (all) {
                end.boxed=true;
                end.left=std::min({xs[0],xs[1],xs[2],xs[3]});end.right=std::max({xs[0],xs[1],xs[2],xs[3]});
                end.top=std::min({ys[0],ys[1],ys[2],ys[3]});end.bottom=std::max({ys[0],ys[1],ys[2],ys[3]});
            }
            if (!cam.Project(e.wx,e.wy+3.35f,e.wz,end.sx,end.sy,depth)) end.sx=-1e9f;
            ends.push_back(end);
        }
    }
    std::sort(ends.begin(),ends.end(),[](const End& a,const End& b){return a.distSq<b.distSq;});
    struct Rect { float l,t,r,b; };
    std::vector<Rect> placed;
    const uint8_t* style=Assets()?Assets()+kStyleOutline:nullptr;
    for (size_t i=0;i<ends.size();++i) {
        const End& end=ends[i];
        const auto& p=*end.p;
        if (now-std::min(now,p.openedTick)<kPortalFormMs||end.distSq>28.0f*28.0f) continue;
        if (end.sx<cam.viewport[0]||end.sx>cam.viewport[0]+cam.viewport[2]||end.sy<cam.viewport[1]||end.sy>cam.viewport[1]+cam.viewport[3]) continue;
        const auto& e=end.field?p.fieldEnd:p.baseEnd;
        if (travel::IsRayOccluded(camera.x,camera.y,camera.z,e.wx,e.wy+3.35f,e.wz)) continue;
        Rect rect{end.sx-200,end.sy-30,end.sx+200,end.sy+30};
        bool hidden=false;
        for (size_t j=0;j<i&&!hidden;++j)
            hidden=ends[j].boxed&&end.sx>ends[j].left&&end.sx<ends[j].right&&end.sy>ends[j].top&&end.sy<ends[j].bottom;
        for (const auto& r : placed)
            hidden|=rect.l<r.r&&rect.r>r.l&&rect.t<r.b&&rect.b>r.t;
        if (hidden) continue;
        placed.push_back(rect);
        char owner[48]{};
        bool named=names::PlayerName(g_uiClient,p.ownerPlayerId,owner,sizeof(owner));
        float alpha=std::clamp((28.0f-std::sqrt(end.distSq))/6.0f,0.0f,1.0f);
        // Lift the (deliberately dark) portal colour back to a readable label colour.
        float rgb[3];colors::ToFloat(colors::ForPlayer(p.ownerPlayerId,g_localPlayerId),rgb);
        float top3=std::max({rgb[0],rgb[1],rgb[2],0.01f});
        uint32_t bright=0xff000000u;
        for (int k=0;k<3;++k) bright|=uint32_t(std::min(1.0f,rgb[k]/top3*0.85f+0.15f)*255)<<(8*k);
        const char* destination=names::Destination(g_uiClient,p,end.field);
        if (named) DrawLabel(end.sx-200,end.sy-28,400,30,owner,bright,alpha,2,style);
        DrawLabel(end.sx-260,end.sy,520,28,destination,0xffe4ded4u,0.9f*alpha,2,style);
    }
}

void DrawFrameBody(Widget& widget, const CameraFrame& cam, const std::vector<PortalInstance>& portals,
                   Vec3 local, uint64_t now, const char* prompt,
                   const char* noticeTitle,const char* noticeBody,bool world) {
  if (world) {
#ifdef TOWN_PORTAL_DEV
    if(auto* registry=WidgetImageRegistry(widget.raw)) for(uint32_t i=0;i<3;++i)if(auto image=sceneprobe::ProbeImage(registry,now,i)) {
        const float x=60+float(i)*1340;
        DrawSolidRect(widget.raw,x,300,1280,360,0xff000000u);
        DrawAssetQuad(widget.raw,image,x+640,480,1280,360,0xffffffffu,1);
        DrawLabelCentered(x,260,1280,35,i==0?"Owned output":i==1?"Owned intermediate":"Primary intermediate",0xffffffffu);
    }
#endif
    std::array<Candidate,kMaxPortals*2> candidates{};
    size_t count=0;
    for (const auto& p:portals) for (bool field:{true,false}) {
        const auto& ep=field?p.fieldEnd:p.baseEnd;
        float ds=math::LengthSq(Vec3{ep.wx,ep.wy+1.62f,ep.wz}-Vec3{cam.camX,cam.camY,cam.camZ});
        float sx{},sy{},depth{};
        if (ds>75*75 || !cam.Project(ep.wx,ep.wy+1.62f,ep.wz,sx,sy,depth)) continue;
        if (count<candidates.size()) candidates[count++]={&p,field,ds};
    }
    std::sort(candidates.begin(),candidates.begin()+count,[](const auto& a,const auto& b) { return a.distSq<b.distSq; });
    // At most four nearby endpoints get work; off-screen and distant portals cost no raycasts.
    uint32_t texture=count?FrameTexture(int((now*24/1000)%kFrameCount)):0;
    // Each endpoint gets its own share of the ray budget; drawing the nearer
    // portal's tiles must not use up the farther portal's visibility queries.
    const size_t visible=std::min(count,size_t(kMaxVisibleEndpoints));
    // The GPU compositor takes endpoints in the order they are asked for and draws the
    // first-asked last, on top; ask nearest first so a nearer portal covers a farther one.
    if (texture) if (auto* registry=WidgetImageRegistry(widget.raw))
        for (size_t i=0;i<visible;++i) {
            const auto& p=*candidates[i].portal;
            sceneprobe::CompositeImage(p,candidates[i].field,colors::ForPlayer(p.ownerPlayerId,g_localPlayerId),registry,now);
        }
    // Farthest first: the tile renderer has no depth between portals, so a nearer one must be
    // drawn over a farther one.
    for (size_t i=visible;i-->0;) {
        g_queriesLeft=96/int(visible);
        g_queryDeadline=CounterMs()+0.8/double(visible);
        DrawEndpoint(widget.raw,cam,candidates[i],local,texture,now);
    }
    // The prompt's key cap. Text is drawn before images within a layer, so the cap goes on
    // this lower layer and the HUD layer's letter sits on top of it.
    if (prompt&&*prompt&&LoadingAlpha(now)<=0.0f&&!g_hintRow) {
        float sx{},sy{};
        PromptPosition(cam,sx,sy);
        DrawSolidRect(widget.raw,sx,sy-kKeyCap*0.5f,kKeyCap,kKeyCap,0xfff4f1edu,1.0f);
    }
    return;
  }

    const float sw=cam.viewport[2],sh=cam.viewport[3],ox=cam.viewport[0],oy=cam.viewport[1];
    if (LoadingAlpha(now)<=0.0f) DrawPortalLabels(cam,portals,now);
    if (float blink=BlinkAlpha(now); blink>0.0f) DrawSolidRect(widget.raw,ox,oy,sw,sh,g_blinkColour.load(),blink);
    // Arrival: a dark screen with the portal swirl until the destination has loaded. Stepping
    // in, the swirl rushes in from filling the screen; arriving, it bursts outward with a
    // flash of the portal's colour as the screen clears.
    if (float loading=LoadingAlpha(now); loading>0.0f) {
        const bool entering=g_loading;
        const float since=float(now-std::min(now,g_loadingChanged.load()));
        const float in=std::min(1.0f,since/kLoadingInMs),out=std::min(1.0f,since/kLoadingOutMs);
        uint32_t tint=g_loadingTint.load();
        if (!tint) tint=colors::Vivid(colors::kUnknown);
        // The game's menus' navy with ring arcs, cropped to the screen's shape; else plain dark.
        if (uint32_t backdrop=g_backdropW?MapTexture(11):0) {
            const float image=float(g_backdropW)/float(g_backdropH),screen=sw/sh;
            const float cu=screen<image?screen/image:1.0f,cv=screen<image?1.0f:image/screen;
            DrawSolidRect(widget.raw,ox,oy,sw,sh,0xff100805u,loading);
            DrawAssetSubUv(widget.raw,backdrop,ox,oy,sw,sh,(1-cu)*0.5f,(1-cv)*0.5f,(1+cu)*0.5f,(1+cv)*0.5f,0xffffffffu,loading);
        } else DrawSolidRect(widget.raw,ox,oy,sw,sh,0xff100805u,loading);
        if (!entering) DrawSolidRect(widget.raw,ox,oy,sw,sh,tint,0.35f*(1-out)*(1-out),2.0f);
        float grow=entering?1.0f+5.0f*(1-in)*(1-in):1.0f+7.0f*out*out;
        float d=std::min(sw,sh)*0.32f*grow;
        float swirlAlpha=entering?in:1.0f-out;
        // The trip on the world map, else the portal swirl.
        float mapBottom=0;
        const float target=entering?std::clamp(g_loadingProgress.load(),0.0f,1.0f):1.0f;
        if (entering&&target<g_shownProgress) g_shownProgress=target; // a new trip starts again
        const float dt=std::min(0.1f,float(now-std::min(now,g_shownProgressAt))*0.001f);
        g_shownProgressAt=now;
        g_shownProgress+=(target-g_shownProgress)*std::min(1.0f,dt*(entering?5.0f:9.0f));
        if (target-g_shownProgress<0.002f) g_shownProgress=target;
        if (DrawLoadingMap(widget.raw,ox,oy,sw,sh,entering?in:1.0f-out,now,g_shownProgress,tint,mapBottom)) d=mapBottom*2-sh-oy*2+36; // the caption just under the map
        else if (uint32_t swirl=FrameTexture(int((now*24/1000)%kFrameCount)))
            DrawAssetSubUv(widget.raw,swirl,ox+(sw-d)*0.5f,oy+(sh-d)*0.5f-30,d,d,0,0,1,1,tint,swirlAlpha,entering?1.0f:1.8f);
        if (!entering) loading*=loading; // the caption leaves ahead of the burst
        std::string caption;
        {
            std::lock_guard guard(g_loadingMutex);
            caption=g_loadingCaption;
        }
        DrawLabelCentered(ox,oy+(sh+d)*0.5f-10,sw,40,caption.empty()?text::Get(Language(),text::Travelling):caption.c_str(),0xffe8e2d8u,loading);
        // Progress at the bottom of the screen: an estimate from the trip's stages (main.cpp).
        char percent[96]{};
        const int shown=int(g_shownProgress*100.0f+0.5f);
        std::snprintf(percent,sizeof(percent),text::Get(Language(),text::LoadingPercent),shown);
        DrawLabelCentered(ox,oy+sh-64,sw,36,percent,0xffc8c0b4u,loading);
        prompt=nullptr;
    }
    if (prompt&&*prompt) {
        // The game's own interaction prompt: a white key cap (drawn on the world layer) with a
        // dark letter, then outlined text in the game's prompt style, beside the object.
        float sx{},sy{};
        PromptPosition(cam,sx,sy);
        // A prompt with a second part ("Return to X  /  Closes your portal") shows that part on
        // its own line under the first.
        std::string first(prompt), second;
        if (size_t cut=first.find("  /  "); cut!=std::string::npos) {
            second=first.substr(cut+5);
            first.resize(cut);
        }
        uint8_t* assets=Assets();
        if (DrawNativePrompt(sx,sy,first.c_str())) {
            // Drawn by the game's own prompt code.
        } else if (assets) {
            const uint8_t* style=PromptStyle(assets);
            DrawLabel(sx,sy-kKeyCap*0.5f,kKeyCap,kKeyCap,"E",0xff1a1612u,1.0f,2);
            DrawLabel(sx+kKeyCap+12,sy-22,900,44,first.c_str(),0xffffffffu,1.0f,0,style);
        }
        if (!second.empty()&&assets)
            DrawLabel(sx+kKeyCap+12,sy+28,900,40,second.c_str(),0xffd8d2c8u,0.9f,0,PromptStyle(assets));

    }
    if (noticeBody&&*noticeBody&&LoadingAlpha(now)<=0.0f) { // never over the loading screen
        // Long (localized) messages wrap; short ones keep the original 540 px box.
        // Below the compass and the game's own lines under it (the Shroud timer bar and its
        // "ENSHROUDED" label).
        float w=std::min(540.0f,sw-40),x=ox+(sw-w)*0.5f,y=oy+215;
        auto lines=text::Wrap(noticeBody,std::max(size_t(10),size_t((w-20)/9.5f)));
        if (lines.empty()) lines.emplace_back(noticeBody);
        float h=42+26*float(lines.size());
        DrawNineSliceBox(widget.raw,kAssetRoundedBox,x,y,w,h,0xe824180eu,0.9f,8);
        DrawLabelCentered(x+10,y+5,w-20,24,noticeTitle,0xffffd898u);
        for (size_t i=0;i<lines.size();++i)
            DrawLabelCentered(x+10,y+31+26*float(i),w-20,26,lines[i].c_str(),0xffffffffu);
    }
}

void SafeFrameBody(Widget& widget,const CameraFrame& cam,const std::vector<PortalInstance>& portals,
                   Vec3 local,uint64_t now,const char* prompt,const char* title,const char* body,bool world) {
    __try { DrawFrameBody(widget,cam,portals,local,now,prompt,title,body,world); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        if (auto* host=greeneye::Host();host&&host->log) host->log("Town Portal: renderer fault; skipping this frame");
    }
}

template<class T>
bool Bind(const GreeneyeHost* host,T& out,const char* pattern) {
    out=reinterpret_cast<T>(host->find(pattern,nullptr));
    return out!=nullptr;
}

} // namespace

bool Init(const GreeneyeHost* host, void** uiRoot) {
    g_root = uiRoot;
    bool ok = true;
    ok &= Bind(host, g_begin, "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 45 33 C0");
    ok &= Bind(host, g_end, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 60 80 79 20 00");
    ok &= Bind(host, g_setSize, "48 89 54 24 08 0F 57 C0 F3 0F 10 54 24 08");
    ok &= Bind(host, g_beginLayer, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 45 8B D9 44 8B 8C 24 80 00 00 00 41 0F B6 D8");
    ok &= Bind(host, g_endLayer, "40 53 48 83 EC 20 48 89 74 24 38 48 8B D9 48 8B 35 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 8B 09 E8");
    ok &= Bind(host, g_named, "48 89 5C 24 08 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B F9 48 8B DA 45 33 C0 48 8B 52 10 48 8B 48 18 48 89 0F 48 C7 47");
    ok &= Bind(host, g_opaque, "40 53 48 83 EC 20 F3 0F 10 0D ?? ?? ?? ?? 48 8B D9 48 8B 49 08 0F 28 C1 0F 14 C1 66 48 0F 7E C2 E8");
    ok &= Bind(host, g_beginLabel, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 50 48 8B DA 41 0F B6 F9");
    ok &= Bind(host, g_endLabel, "40 55 56 57 48 8D 6C 24 B9 48 81 EC F0 00 00 00 0F 29 BC 24 B0 00 00 00");
    ok &= Bind(host, g_uiCtx, "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 4C 8B 41 28");
    ok &= Bind(host, g_image,
               "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 41 0F 10 10 33 C0 C7 44 24 30 97 69 E8 F9 "
               "48 89 44 24 34 48 8B D9 49 8B 48 08 48 8B FA 89 44 24 3C 66 48 0F 7E D0 48 0B C8 "
               "0F 10 44 24 30 0F 11 54 24 40 0F 11 44 24 50 75 ?? 48 83 C3 10 EB ?? 48 8B 43 10 "
               "48 8D 54 24 40 48 83 C3 10 45 33 C0");
    ok &= Bind(host, g_drawImage, "4D 8B 09 45 8B 00 E9 ?? ?? ?? ?? CC");
    g_drawImageSubUv = reinterpret_cast<DrawImageSubUvFn>(host->find(
        "48 83 EC 48 48 8B 41 08 48 8B 48 50 48 8B 49 18 48 85 C9 74 36 83 79 08 04", nullptr));
    g_nineSlice = reinterpret_cast<NineSliceFn>(host->find(
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 30 48 8B F2", nullptr));
    // Optional: without it notices stay in English.
    g_loca = reinterpret_cast<LocaTextFn>(host->find(
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF", nullptr));
    if (!g_loca && host->log) host->log("Town Portal: game text lookup not found; notices use English");
#ifdef TOWN_PORTAL_DEV
    // Dev sound audition only.
    g_playUiSound = reinterpret_cast<PlayUiSoundFn>(host->find(
        "48 8B C4 48 89 78 10 4C 89 70 18 4C 89 78 20 55 48 8B EC 48 81 EC 80 00 00 00 48 8B 79 10 4C 8B F2 4C 8B F9 83 7F 68 00", nullptr));
    // The map's fast-travel handler loads the sound reference with movups [rax+disp32].
    if (auto* site = static_cast<uint8_t*>(host->find(
            "48 8B 0D ?? ?? ?? ?? 48 8D 55 A0 48 8B 81 C8 00 00 00 0F 10 80 ?? ?? ?? ?? 0F 29 45 A0 E8 ?? ?? ?? ?? 41 80 BF", nullptr))) {
        int32_t offset = 0;
        host->read(site + 0x15, &offset, sizeof(offset));
        if (offset > 0 && offset < 0x20000) g_fastTravelSound = uint32_t(offset);
    }
    if (host->log && !(g_playUiSound && g_fastTravelSound))
        host->log("Town Portal: native UI sounds not found; dev sound audition unavailable");
#endif
    // Optional: without these the mod draws its own key cap and label.
    g_hintRow = reinterpret_cast<HintRowFn>(host->find("48 89 5C 24 18 56 48 81 EC A0 02 00 00 80 7A 2F 00", nullptr));
    g_hintStyleInit = reinterpret_cast<HintStyleInitFn>(host->find(
        "48 83 EC 28 45 33 C0 48 C7 41 2D 00 00 01 00 44 88 41 06", nullptr));
    if (host->log && !(g_hintRow && g_hintStyleInit)) host->log("Town Portal: game prompt drawing not found; using the mod's own");
    g_createDynamicTexture = reinterpret_cast<CreateDynamicTextureFn>(host->find(
        "40 53 56 57 41 56 41 57 48 81 EC F0 00 00 00 48 83 7A 18 00 48 8B F2 48 8B F9", nullptr));

    if (g_drawImage) {
        auto* thunk = reinterpret_cast<uint8_t*>(g_drawImage);
        uint8_t bytes[11]{};
        host->read(thunk, bytes, sizeof(bytes));
        if (bytes[6] == 0xE9) {
            int32_t rel = 0;
            std::memcpy(&rel, bytes + 7, 4);
            g_drawReal = reinterpret_cast<DrawRealFn>(thunk + 11 + rel);
        }
    }

    BuildFrames();
    g_ready = ok && g_drawImageSubUv && g_createDynamicTexture;
    if (host->log) host->log(g_ready
        ? "Town Portal: planar energy renderer ready; cached textures, bounded visibility queries, gameplay HUD only"
        : "Town Portal: renderer unavailable (required UI signatures missing); world overlay disabled");
    return g_ready;
}

void PlayScaled(const uint8_t* wav, size_t size, float gain);
void PlayPortalSound(float gain) { PlayScaled(portal::sounds::kTravelWav, sizeof(portal::sounds::kTravelWav), gain); }

// Plays a 16-bit WAV from a copy scaled to the game's volume (Windows plays it outside the
// game's mixer). Stops whatever was playing first: the copy is rewritten.
void PlayScaled(const uint8_t* wav, size_t size, float gain) {
    if (!(gain > 0.001f)) return;
    static std::vector<uint8_t> scaled;
    PlaySoundW(nullptr, nullptr, 0);
    scaled.assign(wav, wav + size);
    const float g = std::min(gain, 1.0f);
    for (size_t i = 44; i + 1 < scaled.size(); i += 2) {
        int16_t sample;
        std::memcpy(&sample, &scaled[i], 2);
        sample = int16_t(float(sample) * g);
        std::memcpy(&scaled[i], &sample, 2);
    }
    PlayEffect(scaled.data());
}

// A voice line file, read whole; empty when missing.
std::vector<uint8_t> ReadVoiceFile(const char* voice, const char* language, const char* line, int take) {
    wchar_t path[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto* slash = wcsrchr(path, L'\\')) slash[1] = 0;
    wchar_t file[160]{};
    swprintf_s(file, L"greeneye\\town-portal\\voice\\%hs\\%hs\\%hs_%d.wav", voice, language, line, take);
    wcscat_s(path, file);
    std::vector<uint8_t> data;
    if (FILE* f = _wfopen(path, L"rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 44 && size < (8 << 20)) {
            data.resize(size_t(size));
            data.resize(std::fread(data.data(), 1, data.size(), f));
        }
        std::fclose(f);
    }
    // The build writes IMA ADPCM; PlayScaled scales 16-bit PCM samples, so decode it here.
    return adpcm::ToPcm(data.data(), data.size());
}

void PlayVoiceLine(const char* line, const char* voice, float gain) {
    if (!(gain > 0.001f) || !line) return;
    if (!voice || !*voice) voice = "f1";
    // One of three at random, never the same one twice running.
    static std::string lastLine;
    static int last = -1;
    int pick = int(GetTickCount64() % 3);
    if (lastLine == line && pick == last) pick = (pick + 1 + int((GetTickCount64() / 7) % 2)) % 3;
    lastLine = line;
    last = pick;
    const char* language = text::kCodes[size_t(Language())];
    // Traditional Chinese speaks the same Mandarin lines as Simplified.
    if (!std::strcmp(language, "zh-hant")) language = "zh-hans";
    // Lines with a single take (the nag_* jokes) fall back to take 1.
    std::vector<uint8_t> data;
    for (int take : {pick + 1, 1}) {
        if (data.empty()) data = ReadVoiceFile(voice, language, line, take);
        if (data.empty()) data = ReadVoiceFile(voice, "en", line, take);
        if (data.empty()) data = ReadVoiceFile("f1", "en", line, take);
    }
    if (data.empty()) return;
    PlayScaled(data.data(), data.size(), gain);
}

void PlayFizzle(float gain) { PlayScaled(portal::sounds::kFizzleWav, sizeof(portal::sounds::kFizzleWav), gain); }
void StartChannelSound(float gain) { PlayScaled(portal::sounds::kSummonWav, sizeof(portal::sounds::kSummonWav), gain); }
void FinishChannelSound() {} // the summon sound is cut to the 3 s channel
void CancelChannelSound() { PlaySoundW(nullptr, nullptr, 0); }
void SetBlink(bool on, uint32_t colour, bool instant) {
    if (on) g_blinkColour=colour;
    if (g_blink.exchange(on)!=on) g_blinkChanged=GetTickCount64()-(on&&instant?uint64_t(kBlinkInMs):0);
}

uint64_t BlinkDarkSince() {
    return g_blink ? g_blinkChanged.load()+uint64_t(kBlinkInMs) : 0;
}

void SetLoadingRoute(float fieldX, float fieldZ, float altarX, float altarZ, bool toAltar) {
    std::lock_guard guard(g_loadingMutex);
    g_loadingRoute = {fieldX, fieldZ, altarX, altarZ, toAltar, true};
}

void SetLoadingProgress(float progress) {
    float current = g_loadingProgress.load();
    while (progress > current && !g_loadingProgress.compare_exchange_weak(current, progress)) {}
}

void SetLoading(bool on, uint32_t tint, const std::string& caption) {
    if (on) LoadMap(); // once, on the first trip (gameplay thread; a moment's decode under the cover)
    if (on && !g_loading.load()) g_loadingProgress = 0.0f;
    if (on) {
        g_loadingTint=tint;
        std::lock_guard guard(g_loadingMutex);
        g_loadingCaption=caption;
    }
    if (g_loading.exchange(on)!=on) g_loadingChanged=GetTickCount64();
}

void ShowNotice(const std::string& title, const std::string& body, uint64_t durationMs) {
    std::lock_guard guard(g_noticeMutex);
    g_noticeTitle = title;
    g_noticeBody = body;
    g_noticeUntil = GetTickCount64() + std::min<uint64_t>(durationMs, 15000ULL);
}

void ClearNotice() {
    std::lock_guard guard(g_noticeMutex);
    g_noticeUntil = 0;
    g_noticeTitle.clear();
    g_noticeBody.clear();
}

void ClearNativeEffects() {
    // Called on world exit/change and hot reload; do not retain world pointers.
    g_occlusion={};
    g_visibilityContext={};
    g_remotePlayers.clear();
    g_textureIds.fill(0);
    g_textureRepo=nullptr;
    g_observedAssets=nullptr;g_observedAssetsTick=0;
    g_metrics={};
}

// The game's language, unless town-portal.ini's [Controls] Language names one of text::kCodes
// (a test switch: Town Portal's own text in that language; the game's names stay in the game's).
text::Lang Language() {
    static int forced=-1;
    static uint64_t readAt=0;
    const uint64_t now=GetTickCount64();
    if(!readAt||now-readAt>2000){
        readAt=now;
        wchar_t value[16]{};
        GetPrivateProfileStringW(L"Controls",L"Language",L"Auto",value,16,rules::Path().c_str());
        forced=-1;
        for(int i=0;i<int(text::Lang::count);++i){
            wchar_t code[16]{};
            for(int k=0;text::kCodes[i][k]&&k<15;++k)code[k]=wchar_t(text::kCodes[i][k]);
            if(!_wcsicmp(value,code))forced=i;
        }
    }
    if(forced>=0)return text::Lang(forced);
    int lang=g_language.load();
    return text::Lang(lang>=0?lang:0);
}

void GetDevStatus(TownPortalDevStatus& out) {
    out.renderedFrames=g_lifetimeMetrics.frames;
    out.drawnQuads=g_lifetimeMetrics.quads+g_metrics.quads;
    out.rayQueries=g_lifetimeMetrics.rays+g_metrics.rays;
    out.textureUploads=g_lifetimeMetrics.uploads+g_metrics.uploads;
    out.averageMs=g_lifetimeMetrics.frames?g_lifetimeMetrics.ms/double(g_lifetimeMetrics.frames):0;
    out.maxMs=g_lifetimeMetrics.maxMs;
}

#ifdef TOWN_PORTAL_DEV
void AppendPresentationStatus(char* json,uint32_t capacity) {
    // Additive diagnostics keep the existing camera and MCP ABI unchanged.
    // FbUiBundle.emotes / FbUiEmoteEntry verified in build 1273043 reflection.
    auto n=std::strlen(json);
    if(n<2||capacity<n+512)return;
    // The global UI context is set only while its frame is being built. MCP
    // dispatch runs outside that scope, so use the most recent HUD observation.
    uint8_t* assets=GetTickCount64()-g_observedAssetsTick<2500?g_observedAssets:nullptr;
    uint8_t* entries=nullptr;uint32_t count=0;
    __try {
        if(assets) {
            auto* array=assets+0xff88;
            count=at<uint32_t>(array,4);
            if(count<=64&&at<int32_t>(array,0))entries=array+at<int32_t>(array,0);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {count=0;}
    n+=std::snprintf(json+n-1,capacity-n+1,
        ",\"presentation\":{\"textureSize\":%u,\"cachedFrames\":%u,\"premultiplied\":true,\"eOnly\":true,\"teleportEffects\":false,\"assets\":\"0x%llx\",\"emotes\":[",
        kFrameDim,kFrameCount,reinterpret_cast<unsigned long long>(assets))-1;
    for(uint32_t i=0;entries&&i<count&&n+170<capacity;++i) {
        uint32_t id{},sequence{};uint8_t order{},cancel{};
        __try {
            auto* entry=entries+i*0x48;
            id=at<uint32_t>(entry,0);sequence=at<uint32_t>(entry,0x34);
            order=entry[0x45];cancel=entry[0x44];
        } __except(EXCEPTION_EXECUTE_HANDLER) {break;}
        n+=std::snprintf(json+n,capacity-n,"%s{\"id\":%u,\"sequence\":%u,\"sort\":%u,\"cancel\":%s}",
            i?",":"",id,sequence,order,cancel?"true":"false");
    }
    std::snprintf(json+n,capacity-n,"]}}");
}
#endif

void PlayNativeSound(uint8_t* assets, int32_t delta) {
    __try {
        g_playUiSound(*g_root, assets + g_fastTravelSound + delta);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_playUiSound = nullptr;
    }
}

void RunSoundOps(uint8_t* assets) {
    std::vector<int32_t> ops;
    {
        std::lock_guard guard(g_soundMutex);
        ops.swap(g_soundOps);
    }
    for (int32_t delta : ops)
        if (g_root && *g_root && assets && g_fastTravelSound && g_playUiSound) PlayNativeSound(assets, delta);
}

void DrawHudFrame(uint8_t* uiClient,const std::vector<PortalInstance>& portals,
                  uint64_t /*localAccount*/,float playerX,float playerY,float playerZ,
                  const char* interactPrompt,float promptX,float promptY,float promptZ,bool promptAnchored) {
    g_promptAnchor={promptX,promptY,promptZ,promptAnchored};
    if (!g_ready || !g_root || !*g_root || !uiClient) return;
    // PlayHook can also run behind the map, inventory and other full-screen UI.
    // Reject those frames before texture lookup, raycasts, or widget creation.
    uint64_t now=GetTickCount64();
    g_observedAssets=Assets();g_observedAssetsTick=now;
    g_localPlayerId=*reinterpret_cast<uint32_t*>(uiClient+0x18);
    if (static uint32_t logged=0; g_localPlayerId!=logged) {
        logged=g_localPlayerId;
        char line[96]{};
        std::snprintf(line,sizeof(line),"Town Portal: local player id %u (0x%x)",logged,logged);
        if (auto* host=greeneye::Host();host&&host->log) host->log(line);
    }
    g_uiClient=uiClient;
#ifdef TOWN_PORTAL_DEV
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (foreground == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        for (int i = 0; i < 10; ++i) {
            bool down = (GetAsyncKeyState(i == 9 ? '0' : '1' + i) & 0x8000) != 0;
            if (down && !g_auditionDown[i]) {
                {
                    std::lock_guard guard(g_soundMutex);
                    g_soundOps.push_back(kSoundCandidates[i].delta);
                }
                ShowNotice("Sound audition", kSoundCandidates[i].name, 2500);
            }
            g_auditionDown[i] = down;
        }
    }
#endif
    RunSoundOps(g_observedAssets);
    ++g_hudFrame;
    DetectLanguage(g_observedAssets,now);
    ResolveTexts();
#ifdef TOWN_PORTAL_DEV
    sceneprobe::GameplayHud(uiClient[0x1cd8]==1,now);
#endif
    if (uiClient[0x1cd8]!=1) return;
    std::string title,body;
    {
        std::lock_guard guard(g_noticeMutex);
        if (now<g_noticeUntil) { title=g_noticeTitle;body=g_noticeBody; }
    }
    bool probeActive=false;
#ifdef TOWN_PORTAL_DEV
    probeActive=sceneprobe::ProbeActive(now);
    // tools\prompt-test.enabled: show a sample prompt mid-screen.
    if (GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\prompt-test.enabled")!=INVALID_FILE_ATTRIBUTES) {
        interactPrompt="Travel to Fort Kelvin";
        g_promptAnchor.valid=false;
    }
#endif
    if (portals.empty()&&body.empty()&&(!interactPrompt||!*interactPrompt)&&!probeActive&&LoadingAlpha(now)<=0.0f&&BlinkAlpha(now)<=0.0f) return;
    CameraFrame cam{};
    if (!cam.Capture(uiClient)) return;
    double start=CounterMs();
    g_visibilityContext=travel::GetVisibilityContext();
    g_remotePlayers.clear();
    travel::GatherEcsRemotePlayers(g_remotePlayers);
    // Portals sit in the world, so they go beneath the HUD (the minimap uses
    // layer order 2); the prompt and notices stay on top at 0x16.
    for (bool world:{true,false}) {
        UiText name=MakeText(world?"townPortalWorldLayer":"townPortalHudLayer");
        alignas(16) uint8_t layer[0x400]{};
        g_beginLayer(layer,&name,1,world?0:0x16,-1,0);
        Widget widget;
        g_named(widget.raw,&name);
        g_opaque(widget.raw);
        if (widget.Handle()) SafeFrameBody(widget,cam,portals,{playerX,playerY,playerZ},now,
                                         interactPrompt,title.c_str(),body.c_str(),world);
        g_end(widget.raw);
        g_endLayer(layer);
    }
    double ms=CounterMs()-start;
    ++g_metrics.frames;
    g_metrics.ms+=ms;
    g_metrics.maxMs=std::max(g_metrics.maxMs,ms);
    ++g_lifetimeMetrics.frames;
    g_lifetimeMetrics.ms+=ms;
    g_lifetimeMetrics.maxMs=std::max(g_lifetimeMetrics.maxMs,ms);
    if (!g_metrics.since) g_metrics.since=now;
    if (now-g_metrics.since>=5000) {
        char line[256]{};
        std::snprintf(line,sizeof(line),
            "Town Portal renderer: %.3f ms avg / %.3f ms max; %.1f rays/frame, %.1f quads/frame; %llu uploads, %llu deferred visibility cells (5s)",
            g_metrics.ms/double(g_metrics.frames),g_metrics.maxMs,
            double(g_metrics.rays)/double(g_metrics.frames),double(g_metrics.quads)/double(g_metrics.frames),
            static_cast<unsigned long long>(g_metrics.uploads),static_cast<unsigned long long>(g_metrics.deferred));
        if (auto* host=greeneye::Host();host&&host->log) host->log(line);
        g_lifetimeMetrics.quads+=g_metrics.quads;
        g_lifetimeMetrics.rays+=g_metrics.rays;
        g_lifetimeMetrics.uploads+=g_metrics.uploads;
        g_metrics={};
        g_metrics.since=now;
    }
}

} // namespace portal::render
