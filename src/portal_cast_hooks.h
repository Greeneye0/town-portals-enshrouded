#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace portal::casting::hooks {
inline constexpr char inputPattern[] =
    "4C 89 6C 24 28 48 89 44 24 20 E8 ?? ?? ?? ?? 48 8B 9C 24 98 01 00 00";
inline constexpr uint32_t inputCallRva = 0x3abd23, inputTargetRva = 0x2437c0;
inline constexpr size_t inputCallOffset = 10;
using Read = void (*)(const void*, void*, size_t);

inline bool CallsTarget(const uint8_t* bytes, uintptr_t site, uintptr_t target) {
    int32_t displacement{};
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    return bytes[0] == 0xe8 && int64_t(site) + 5 + displacement == int64_t(target);
}

// Always inspect the loader's original bytes. Live calls may already point to
// another mod's thunk, including on our first generation after a game restart.
inline bool CheckCall(Read read, const uint8_t* site, const uint8_t* target) {
    if (!read) return false;
    uint8_t bytes[5]{};
    read(site, bytes, sizeof(bytes));
    return CallsTarget(bytes, uintptr_t(site), uintptr_t(target));
}

inline bool CheckInputCall(Read read, const uint8_t* site, const uint8_t* target) {
    if (!read) return false;
    constexpr uint8_t before[]{0x4c,0x89,0x6c,0x24,0x28,0x48,0x89,0x44,0x24,0x20};
    constexpr uint8_t after[]{0x48,0x8b,0x9c,0x24,0x98,0x01,0x00,0x00};
    uint8_t bytes[sizeof(before) + 5 + sizeof(after)]{};
    read(site - sizeof(before), bytes, sizeof(bytes));
    return std::memcmp(bytes, before, sizeof(before)) == 0 &&
           std::memcmp(bytes + sizeof(before) + 5, after, sizeof(after)) == 0 &&
           CallsTarget(bytes + sizeof(before), uintptr_t(site), uintptr_t(target));
}
}
