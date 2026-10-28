#pragma once
// The home snapshot kept on disk: the last snapshot of the player's own altar, so the next
// login can show it at once instead of covering the screen while a new one is taken.
// One file per Steam account and marked respawn altar (exact fixed-point position), in
// greeneye\town-portal\home-<account>-<altar hash>.tps.
// Pixels: colour as R11G11B10 floats and camera depth as the snapshot's own half float
// (6 bytes instead of RGBA16F's 8; the colour loses mantissa bits nobody can see), stored as
// byte planes and LZMS compressed (smaller than XPRESS_HUFF; saving runs on a worker thread).
#include "portal_snapshot_wire.h"
#include <cstdio>

namespace portal::homecache {
struct Key {
    uint64_t account{};int64_t altar[3]{};
    bool operator==(const Key&) const = default;
};
constexpr bool Valid(const Key& k) {return k.account!=0&&(k.altar[0]||k.altar[1]||k.altar[2]);}
constexpr uint64_t AltarHash(const Key& k) {
    uint64_t hash=0xcbf29ce484222325ull;
    for(int64_t v:k.altar)for(unsigned shift=0;shift<64;shift+=8)hash=(hash^((uint64_t(v)>>shift)&255))*0x100000001b3ull;
    return hash;
}
inline void FileName(const Key& k,wchar_t* out,size_t capacity) {
    std::swprintf(out,capacity,L"home-%llu-%016llx.tps",
        static_cast<unsigned long long>(k.account),static_cast<unsigned long long>(AltarHash(k)));
}

constexpr uint32_t kFileMagic=0x31485054; // TPH1
constexpr uint32_t kFileVersion=3;        // 2: the same pixels XPRESS_HUFF compressed (still read);
                                          // 1: RGBA16F (sharing codec), no longer read
constexpr uint32_t kPackedBytes=sharing::kWidth*sharing::kHeight*6;
#pragma pack(push,1)
struct FileHeader {
    uint32_t magic=kFileMagic;uint32_t version=kFileVersion;
    uint64_t account{};int64_t altar[3]{};
    sharing::Metadata meta{};
    uint32_t crc{},size{}; // crc of the packed pixels; size of the compressed bytes after this
};
#pragma pack(pop)
static_assert(sizeof(FileHeader)==8+8+24+140+8);

// Half float (no NaN or infinity: the snapshot codec rejects those) to the unsigned 11- and
// 10-bit floats of R11G11B10. Same exponent bias, so it is the mantissa rounded off;
// negative colour becomes 0 and rounding never reaches infinity.
constexpr uint32_t HalfToSmall(uint16_t half,unsigned dropBits) {
    if(half&0x8000)return 0;
    const uint32_t rounded=(uint32_t(half)+(1u<<(dropBits-1)))>>dropBits;
    const uint32_t largest=(0x7bffu>>dropBits);
    return rounded>largest?largest:rounded;
}
constexpr uint16_t SmallToHalf(uint32_t value,unsigned dropBits) {return uint16_t(value<<dropBits);}
constexpr uint32_t PackColour(uint16_t r,uint16_t g,uint16_t b) {
    return HalfToSmall(r,4)|HalfToSmall(g,4)<<11|HalfToSmall(b,5)<<22;
}

// RGBA16F pixels to the file's bytes and back (portal_snapshot_codec.cpp). EncodeFile returns
// nothing for unusable pixels; DecodeFile is false when the file is not a home snapshot of
// this key, or damaged.
std::vector<uint8_t> EncodeFile(const Key& key,const sharing::Image& image);
bool DecodeFile(const Key& key,std::span<const uint8_t> file,sharing::Image& out);

// Worker-thread file access. Save keeps a copy of the image; Load decodes in the background
// and TakeLoaded hands the image over once (nullptr while pending or when there is none).
bool Exists(const Key& key);
void Save(const Key& key,std::shared_ptr<const sharing::Image> image);
void Load(const Key& key);
// 0: loading, 1: loaded (image set), -1: no usable file.
int TakeLoaded(const Key& key,std::shared_ptr<sharing::Image>& image);
void Stop();
}
