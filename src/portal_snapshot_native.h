#pragma once
#include <cstdint>
#include <cstddef>
namespace portal::sharing::native {
// Build 1273043. Kept beside the call sites so the offline binary test validates
// exactly the signatures used by the runtime, not a second handwritten copy.
struct Entry {const char* name;uint32_t rva;const char* signature;};
inline constexpr Entry entries[]={
    {"SnapshotDownloadCreate",0xfc1550,"48 89 6C 24 18 56 41 56 41 57 48 81 EC 80 00 00 00 48 8B F2 48 C7 44 24 20 10 00 00 00 48 8B 52 08 4C 8B F1 48 8D 4C 24 30 44 0F B6 06 4C 8B 7A 08 41 8B AF 40 04 00 00 44 8B CD 4C 0F AF 4E 10 E8 ?? ?? ?? ?? 80 7C 24 30 00 74 ?? 33 C0 E9 ?? ?? ?? ?? 49 8B 0E 41 B1 08 0F B6 06 BA 88 02 00 00"},
    {"SnapshotDownloadPop",0xfe6d80,"48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC 80 00 00 00 48 83 BA 78 02 00 00 00"},
    {"SnapshotDownloadImage",0x1015860,"48 89 5C 24 08 48 89 6C 24 10 56 48 81 EC D0 00 00 00"},
    {"SnapshotUploadImage",0x1018150,"48 8B C4 48 89 58 18 48 89 70 20 55 57 41 54 41 56 41 57 48 8D 68 A9"}
};
struct Region {
    uint32_t aspect=1,mip{},layer{},layers=1;
    int32_t x{},y{},z{};
    uint32_t width{},height{},depth=1;
};
static_assert(sizeof(Region)==40 && offsetof(Region,y)==20 && offsetof(Region,width)==28);
}
