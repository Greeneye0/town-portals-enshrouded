#pragma once
#include "greeneye.h"
#include "portal_types.h"
#include "portal_math.h"
#include <cstdint>
#include <vector>

namespace portal::travel {

enum class Submission { remote, queued, rejected };

bool Init(const GreeneyeHost* host);
void SetClient(void* gameClient);

// True while this process runs the authoritative ECS world (singleplayer, host, or dedicated server).
bool HostsWorld();
bool HostAnswered();

// Queue an immediate authoritative teleport for `entity` to `target` (with known 3D x, y, z).
Submission SubmitTeleport(uint32_t entity, const FixedPos& target);

// Refine a 3D world point's ground Y using a short local vertical voxel raycast ([wy+2.8m .. wy-3.5m])
// if the terrain chunk is loaded, preserving indoor/cave/multi-floor heights.
bool RefineLocalGroundY(float wx, float& wyInOut, float wz);
// False while this machine's terrain cells around the point are still streaming in.
bool LocalTerrainReady(float wx, float wy, float wz);

// Check whether a 3D line-of-sight segment from (fromX, fromY, fromZ) to (toX, toY, toZ)
// is occluded by solid voxel terrain, hills, or building walls.
bool IsRayOccluded(float fromX, float fromY, float fromZ, float toX, float toY, float toZ);

struct VisibilityContext {
    void* physics{};
    void* terrain{};
    uint8_t realm{};
    uint32_t ignoreEntity{};
};
VisibilityContext GetVisibilityContext();
math::Visibility TraceVisibility(const VisibilityContext& context, math::Vec3 from, math::Vec3 to);

// Sample a 5x5 local terrain height patch around a portal endpoint for the 3D see-through window.
void SampleEndpointTerrain(PortalEndpoint& ep);

// Gather live 3D positions of remote players from the authoritative ECS world.
void GatherEcsRemotePlayers(std::vector<RemotePlayerPos>& outPlayers);

// Access & manage the synchronized table of active portals in the current world.
void UpsertPortal(const PortalInstance& instance);
bool ClosePortalByOwner(uint64_t ownerAccount, uint64_t portalId = 0);
void ClearAllPortals();
void GetActivePortals(std::vector<PortalInstance>& outPortals);
bool GetPortalByOwner(uint64_t ownerAccount, PortalInstance& outPortal);
bool MarkOwnerVisitedBase(uint64_t ownerAccount);

uint64_t LocalSteamAccount();

bool Prepare();
void Resume();
void AppendDevStatus(char* json,uint32_t capacity);

} // namespace portal::travel
