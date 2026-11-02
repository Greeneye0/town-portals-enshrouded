#include "../src/portal_root_descriptor.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <algorithm>
int main(){
 using namespace portal::sceneprobe::rootdescriptor;
 std::array<uint8_t,0xa0> desc{};for(size_t i=0;i<desc.size();++i)desc[i]=uint8_t(i*13+7);
 const uint64_t allocator=0x1234567887654321ull;std::memcpy(desc.data()+0x58,&allocator,8);
 for(uint32_t n:{0u,1u,4u,8u}){
  Slots(desc.data(),n);auto before=desc;
  Slots(desc.data(),std::max(Slots(desc.data()),4u));assert(Slots(desc.data())==std::max(n,4u));
  uint64_t after;std::memcpy(&after,desc.data()+0x58,8);assert(after==allocator);
  for(size_t i=0;i<desc.size();++i)if(i<slotsOffset||i>=slotsOffset+4)assert(desc[i]==before[i]);
  Slots(desc.data(),n);assert(desc==before);
 }
 assert(NativeMatches(nativeSlotReads));auto old=nativeSlotReads;uint8_t stale[sizeof(nativeSlotReads)];std::memcpy(stale,old,sizeof(stale));stale[3]=0x5c;assert(!NativeMatches(stale));
 std::puts("PASS: renderer slot reservation preserves the adjacent pointer and every other descriptor byte; original count restored; old descriptor layout rejected");
}
