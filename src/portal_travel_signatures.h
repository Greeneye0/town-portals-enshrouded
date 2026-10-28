#pragma once
#include <cstddef>

namespace portal::travel::signatures {

// Native travel gate (also used by fast travel and Embervale Teleport). Answers a refusal
// bitmask: 1 = travel disabled in this area, 2 = falling, 4 = under water.
inline constexpr char travelGate[] =
    "C6 01 00 49 0F BA E0 0E 72 ?? 32 C0 84 D2 74 ?? 80 FA 01 74 ?? C6 01 01 B0 01 49 0F BA E0 2C";
// The player system's call to the gate, relative to the player system (kPlayerPattern).
inline constexpr size_t travelGateCall = 0xfb2;

// Value the player system leaves in the command descriptor (+8) when it accepted a travel action.
inline constexpr unsigned travelAccepted = 0x1cc90f15u;

} // namespace portal::travel::signatures
