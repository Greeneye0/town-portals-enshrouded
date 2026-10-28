#pragma once
#include "portal_snapshot_wire.h"
#include <string_view>
namespace portal::sharing {
using Broadcast=void(*)(void*,std::string_view);
// Gameplay thread. Session membership arrives exclusively through game-relayed chat.
void Tick(void* world,void* client,uint64_t account,const std::vector<PortalInstance>& portals,Broadcast broadcast);
// gameAccountHash is the native chat identity, distinct from the SteamID64.
// Control validates the announcement's Steam ID against this native hash.
void Control(uint64_t gameAccountHash,std::string_view line);
void Stop();
uint64_t Epoch();
bool OwnPortal(PortalInstance& out);
bool Current(const Key& key);
bool Submit(std::shared_ptr<Image> image);
std::shared_ptr<Image> TakeIncoming();
void AppendStatus(char* out,uint32_t capacity);
// Experimental native GPU transfer is gated until its first approved in-game test.
bool Enabled();
}
