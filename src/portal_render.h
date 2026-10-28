#pragma once
#include "greeneye.h"
#include "portal_types.h"
#include "portal_dev.h"
#include "portal_text.h"
#include <cstdint>
#include <string>
#include <vector>

namespace portal::render {

bool Init(const GreeneyeHost* host, void** uiRoot);

// Called inside PlayHook to project fixed world-plane portals into the HUD,
// with conservative visibility clipping, an interaction prompt and notices.
void DrawHudFrame(uint8_t* uiClient,
                  const std::vector<PortalInstance>& portals,
                  uint64_t localAccount,
                  float playerX, float playerY, float playerZ,
                  const char* interactPrompt,
                  // World point the prompt sits beside (the portal it is for), like the game's
                  // own interaction prompts; without one it is drawn at the bottom of the screen.
                  float promptX, float promptY, float promptZ, bool promptAnchored);

// Portal sounds: the summon sound over the 3 s channel (stopped if it is cancelled) and the
// travel whoosh when the player steps through, at gain (the game's master times game volume).
void PlayPortalSound(float gain);
// The character's voice refusing a cast: one of three takes of line ("no_room", "shroud",
// "hollow_halls") from greeneye\town-portal\voice\<voice>\<language>\<line>_<n>.wav, in
// voice (f1..f5, m1..m5; f1 when unknown) and the game's language, else English. At gain
// (the game's master volume times its player voice volume; 0 plays nothing).
void PlayVoiceLine(const char* line, const char* voice, float gain);
// The fizzle of an interrupted cast, at gain (the game's master times game volume).
void PlayFizzle(float gain);
void StartChannelSound(float gain);
void FinishChannelSound();
void CancelChannelSound();
// Full-screen portal loading overlay while arrival terrain streams in (fades in and out).
// tint: the colour of the portal stepped through; caption: where it leads (already localized).
void SetLoading(bool on, uint32_t tint = 0, const std::string& caption = {});
// The trip the loading screen shows on the world map: the portal's field end and its altar end
// (world metres), travelling toward the altar or away from it. Set before SetLoading(true).
void SetLoadingRoute(float fieldX, float fieldZ, float altarX, float altarZ, bool toAltar);
// The loading screen's progress (0..1, shown as a percentage at the bottom). It never goes
// back during one loading screen; leaving the screen shows 100%.
void SetLoadingProgress(float progress);
// A short dark cover over the game view (a snapshot taken outside travel): fades in fast, out
// soon after it is turned off.
// instant: fully dark at once (continuing a loading screen), no fade in.
void SetBlink(bool on, uint32_t colour = 0xff100805u, bool instant = false);
// Fully dark (faded in) since this tick; 0 when not.
uint64_t BlinkDarkSince();
// Show a brief HUD notification banner (e.g., when opening/closing a portal).
void ShowNotice(const std::string& title, const std::string& body, uint64_t durationMs = 3000);
void ClearNotice();
// The game's own language, for localized notices (English until detected).
text::Lang Language();
// Game text for a loca id, in the game's language; false when the lookup is unavailable.
bool GameText(uint32_t locaId, char* out, size_t cap);

// Clean up any native 3D scene VFX handles before hot reload or world exit.
void ClearNativeEffects();
void GetDevStatus(TownPortalDevStatus& out);
#ifdef TOWN_PORTAL_DEV
void AppendPresentationStatus(char* json,uint32_t capacity);
#endif

} // namespace portal::render
