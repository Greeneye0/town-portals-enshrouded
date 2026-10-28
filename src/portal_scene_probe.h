#pragma once
#include "greeneye.h"
#include "portal_types.h"
#include "portal_snapshot_cache.h"
namespace portal::sceneprobe {
bool Init(const GreeneyeHost* host);
bool Json(char* out,uint32_t capacity);
bool Preview(const PortalInstance& portal,bool fieldSide,uint32_t durationMs);
bool TargetRequest(uint32_t action);
uint32_t LiveImage(const PortalInstance& portal,bool fieldSide,void* registry,uint64_t now);
// tint: the owner's colour (ABGR), see portal_colors.h.
// The endpoint was drawn into the game's own frame this frame; the HUD draws nothing for it.
constexpr uint32_t kDrawnInFrame=0xffffffffu;
uint32_t CompositeImage(const PortalInstance& portal,bool fieldSide,uint32_t tint,void* registry,uint64_t now);
uint32_t ProbeImage(void* registry,uint64_t now,uint32_t source=0);
bool ProbeActive(uint64_t now);
// Snapshot of what stands in front of a portal endpoint, from a camera `distance` metres out
// along its normal, no sooner than notBefore (GetTickCount64). The portal leading there shows
// it. Only request it while a loading screen hides the frame: for a few frames the world is
// drawn from that camera.
// shroud: the player is in the Shroud there (the snapshot gets its colour grade when shown).
bool RequestSnapshot(const PortalEndpoint& end,float distance,uint64_t notBefore,bool shroud=false);
// The pending request is of the player's own altar: once taken it is read back from the GPU
// and saved for the next login (portal_snapshot_cache.h).
void MarkHomeSnapshot(const homecache::Key& key);
// Show a snapshot made outside a capture (the saved home snapshot, a default picture; its
// meta says where): uploaded into a free snapshot slot on the render side. One at a time;
// what (a string literal) names it in the log. Pass nullptr to poll: 0 while pending, 1 once
// shown, -1 when it cannot be (no free slot, images made before this build by a hot reload,
// or the native upload unavailable).
int UploadSnapshot(std::shared_ptr<sharing::Image> image,const char* what=nullptr);
// The altar-side portal most recently drawn with nothing to show of its field end (within
// the last half second, and after `after`, GetTickCount64): its id and that destination
// (fieldEnd turned). False when none.
bool MissingSnapshot(uint64_t& portalId,PortalEndpoint& destination,uint64_t after=0);
// The game frame's exposure (as a capture records it); 0 before the first frame. A snapshot
// made outside a capture gets this one so it shows as if taken now.
float FrameExposure();
// Gameplay-thread readiness of the player's current area. Capture needs fresh,
// continuously ready samples; a timeout keeps the previous image intact.
void SnapshotAreaReady(bool ready,uint64_t now);
// Stop a pending capture; SnapshotBusy stays true until the camera has cut back.
void CancelSnapshot();
// A snapshot is queued or being taken (its frames are not yet back to the player's camera).
bool SnapshotBusy();
// Whether snapshots can be taken; when not yet, asks the render thread to make the images
// (normally made for the first portal in view). Call again later.
bool SnapshotsReady();
// Both local-player body and carried-equipment hiding hooks are available.
bool HidesPlayer();
// The spell effect while a portal forms: from the caster's hand (world metres) to that
// portal's field end; effect 1-3 (portal_composite.frag CastEffect), 0 or portalId 0 for none.
void SetCast(uint64_t portalId,float x,float y,float z,int effect);
void GameplayHud(bool playing,uint64_t now);
}
