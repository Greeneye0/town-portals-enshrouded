#pragma once
// Portal water: while one end of a portal is at least half under water, the other end pours
// water like the game's Metal Water Dispenser I, until that end is itself half under water.
// The world's host does it (the hosting player's game or a dedicated server), through the
// server's own water simulation: each pouring end is a dispenser registered with it.
// [Portals] PortalWater=false (or NoFun=true) in town-portal.ini turns it off.
#include <cstdint>

struct GreeneyeHost;

namespace portal::water {

// Finds the water simulation functions; false (and a log line) when this game build lacks them.
bool Init(const GreeneyeHost* host);
// Called on the server thread with the server object, every tick.
void ServerTick(void* server);
// Hot reload: switches this generation's spouts off on the next server tick; true once done
// (or when no server is running here).
bool Prepare();
void Resume();

// How high the water stands at each end of a portal, in metres above that end's foot (-1: dry
// there). Measured on the world's host every half second, for every open portal, so the view
// through a portal can show water the far end stands in (the far camera stands in air at the
// viewer's eye height and does not draw it). false when this process is not the host or the
// portal is unknown.
bool Levels(uint64_t portalId, float& field, float& base);
// Levels heard from the host ("/tpw1"), for clients; kept 8 s.
void SetHeardLevels(uint64_t portalId, float field, float base);
// Measured here when hosting, else heard.
bool KnownLevels(uint64_t portalId, float& field, float& base);

} // namespace portal::water
