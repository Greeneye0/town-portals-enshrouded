#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace portal::build {
// Whether this process runs the game build a fixed layout was taken from: the same link
// time stamp, and the same image, or the same image with Steam's DRM wrapper (the trailing
// .bind section) stripped off, as Steamless does. Stripping leaves the code byte for byte
// the same and only shortens the image (the client's 0x2a1e000 to 0x29e4000, where .bind
// began), so a stripped exe passes when it has no .bind and is not larger.
inline bool Matches(uint32_t stamp, uint32_t size) {
    auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != stamp) return false;
    if (nt->OptionalHeader.SizeOfImage == size) return true;
    if (nt->OptionalHeader.SizeOfImage > size) return false;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (std::memcmp(section[i].Name, ".bind", 6) == 0) return false;
    return true;
}
constexpr uint32_t kClientStamp = 0x6ad77f0c, kClientSize = 0x2a1e000;
constexpr uint32_t kServerStamp = 0x6ad77f14, kServerSize = 0x1dd1000;
inline bool Client() { return Matches(kClientStamp, kClientSize); }
}
