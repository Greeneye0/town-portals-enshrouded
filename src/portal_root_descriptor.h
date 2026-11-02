#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace portal::sceneprobe::rootdescriptor {
// Build1283531: root constructor reads the scene-slot limits at +64/+68.
// +58..+5f is now a pointer; the old +5c field is its upper half.
inline constexpr size_t slotsOffset=0x64;
inline constexpr size_t nativeSlotReadOffset=0x184;
inline constexpr uint8_t nativeSlotReads[]{0x41,0x8b,0x4f,0x64,0x41,0x39,0x4f,0x68};
inline uint32_t Slots(const void* description){uint32_t value;std::memcpy(&value,static_cast<const uint8_t*>(description)+slotsOffset,4);return value;}
inline void Slots(void* description,uint32_t value){std::memcpy(static_cast<uint8_t*>(description)+slotsOffset,&value,4);}
inline bool NativeMatches(const uint8_t* bytes){return std::memcmp(bytes,nativeSlotReads,sizeof(nativeSlotReads))==0;}
}
