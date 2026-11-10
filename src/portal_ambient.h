#pragma once
#include <cstdint>

// The sound a portal gives off where it stands: a quiet loop heard near its ends, fading with
// distance, panned to the side it is on, and as loud as the game's own Master and Ambience
// volumes allow (the game's user_settings.json).
namespace portal::ambient {

// Once a frame on the game thread: the listener (the player's camera) and the camera's right.
void Update(uint64_t now, float x, float y, float z, float rightX, float rightZ, bool playing);
// Before this generation of the mod is let go (hot reload, exit): the sound stops and its
// engine is released.
void Stop();

} // namespace portal::ambient
