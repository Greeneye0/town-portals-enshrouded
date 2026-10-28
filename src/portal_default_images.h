#pragma once
// Default pictures for altar portals: until anything has been captured of where a portal's
// field end stands, the portal by the altar shows a landmark of that region instead of the
// animated swirl, by day or by night (greeneye\town-portal\biomes\<region>-<day|night>.jpg,
// 2048x1024, made by tools\biome-shots\export_defaults.py). They become snapshots: colour
// turned back into scene light for the game's tone curve, at a backdrop's depth, with a camera
// from portal_default_view.h. The first real capture of that place replaces it.
#include "portal_snapshot_wire.h"

namespace portal::defaults {
constexpr int kRegions=8; // portal_regions.h order
// The light a picture's sRGB value stands for, so the game's tone curve shows it about as the
// photo looked (an inverse Reinhard curve, capped: the photos were already tone mapped).
float SceneLight(uint8_t srgb);
// Worker-thread loading: Request starts one picture (replacing any pending request); Take
// hands it over once loaded: 1 (image set, meta empty), 0 still loading, -1 none.
void Request(int region,bool night);
// Any image file Windows can decode, as 8-bit RGBA rows (straight alpha). Initialises COM on
// the calling thread when needed. False when missing or unreadable.
bool LoadRgba(const wchar_t* path,uint32_t& width,uint32_t& height,std::vector<uint8_t>& rgba);
int Take(int region,bool night,std::shared_ptr<sharing::Image>& image);
void Stop();
}
