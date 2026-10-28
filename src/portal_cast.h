#pragma once
#include "greeneye.h"
#include <cstdint>
namespace portal::casting {
bool Init(const GreeneyeHost* host);
void Bind(void* gameClient);
// Read-only local ClientActor state for the initial snapshot's spawn gate.
bool SnapshotActorStates(void* gameClient, uint64_t& states);
bool Start(void* gameClient);
// The shrug (hands up, palms up) when there is no room for a portal; not over a channel.
bool Shrug(void* gameClient);
void Stop();
// Releases the pose over 0.25 s (the channel was aborted).
void Cancel();
// Movement lock: the local player's stick, dodge, sprint, dive and glide input
// is zeroed until the deadline. Pressing Jump ends the lock early (the jump
// goes through) and is reported once by TakeJumpAbort - unless the lock is
// hard (the arrival loading screen), which nothing but the deadline ends.
bool MovementLockReady();
void LockMovement(uint64_t until, bool hard = false);
void UnlockMovement();
bool TakeJumpAbort();
uint64_t DurationMs();
void AppendStatus(char* json,uint32_t capacity);
}
