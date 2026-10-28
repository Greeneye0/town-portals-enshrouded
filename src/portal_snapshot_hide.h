#pragma once
#include <cstddef>
#include <cstdint>

namespace portal::snapshot::hide {
// Build 1273043, static evidence: out/hideplayer/dis-local_visual_equipment_hide-382f60.asm.
// This call reads the camera's equipment-hide bit. The caller applies it only
// to visual equipment whose DespawnLink matches the local player (382fc5),
// marking Hide dirty and setting Hide[1] (382fff). No inventory state changes.
inline constexpr char equipmentCall[] =
    "48 8B 4C 24 70 8B D8 0F B6 91 60 14 00 00 E8 ?? ?? ?? ?? "
    "41 B8 50 00 00 00 48 8D 54 24 30 48 8B CE 0F B6 F8";
inline constexpr size_t callOffset = 14;
inline constexpr uint32_t callRva = 0x382f9d, targetRva = 0xe8c260;
inline constexpr char equipmentTarget[] =
    "4C 8B 49 30 4C 8D 81 C8 20 00 00 4D 85 C9 75 ?? "
    "48 8D 05 ?? ?? ?? ?? 0F B6 00 C0 E8 06";
}
