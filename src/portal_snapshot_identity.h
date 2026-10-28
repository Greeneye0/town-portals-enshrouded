#pragma once
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace portal::sharing {
// The game relays AccountId.hash, NOT SteamID64, in chat metadata.
// Build 1273043: FUN_140a71340, LAB_140a7176d writes SessionPlayer+0x68
// from SteamID64 at +0x60 using this seeded, little-endian FNV-1a.
// Verified against the native sender hash in a live session. Keep this pure:
// discovery must not require a new native hook or execute game functions.
constexpr uint64_t SteamGameAccount(uint64_t steam) {
    uint64_t hash=0x3f63b56db2890a16ull;
    for(unsigned shift=0;shift<64;shift+=8)hash=(hash^((steam>>shift)&255))*0x100000001b3ull;
    return hash;
}
constexpr bool SteamPlayer(uint64_t steam) {
    const auto universe=steam>>56;
    return uint32_t(steam)!=0 && universe>=1 && universe<=4 &&
        ((steam>>52)&15)==1 && ((steam>>32)&0xfffff)==1;
}
struct Announcement {uint64_t steam{},nonce{};};
inline bool ParseAnnouncement(uint64_t gameSender,std::string_view line,Announcement& out) {
    if(!gameSender||!line.starts_with("/tpi2 ")||line.size()>64)return false;
    line.remove_prefix(6);
    const auto split=line.find(' ');if(split==std::string_view::npos)return false;
    Announcement candidate{};
    const char* first=line.data();const char* last=first+line.size();
    auto id=std::from_chars(first,first+split,candidate.steam);
    auto nonce=std::from_chars(first+split+1,last,candidate.nonce);
    if(id.ec!=std::errc{}||id.ptr!=first+split||nonce.ec!=std::errc{}||nonce.ptr!=last||
       !SteamPlayer(candidate.steam)||!candidate.nonce||SteamGameAccount(candidate.steam)!=gameSender)return false;
    out=candidate;return true;
}
inline int FormatAnnouncement(char* out,size_t capacity,uint64_t steam,uint64_t nonce) {
    return std::snprintf(out,capacity,"/tpi2 %llu %llu",
        static_cast<unsigned long long>(steam),static_cast<unsigned long long>(nonce));
}
}
