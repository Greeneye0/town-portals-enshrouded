#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include "greeneye.h"
#include "portal_snapshot_cache.h"

namespace {
// The Compression API, looked up at run time: Wine's (Proton's, SteamOS's) cabinet.dll does
// not have it, and an import missing there is bound to a stub that crashes the game when
// called. Without it pictures are shared uncompressed, and compressed ones (other players',
// the saved home picture) cannot be read; the portal shows its default picture instead.
struct CompressionApi {
    decltype(&::CreateCompressor) createCompressor{};
    decltype(&::CreateDecompressor) createDecompressor{};
    decltype(&::CloseCompressor) closeCompressor{};
    decltype(&::CloseDecompressor) closeDecompressor{};
    decltype(&::Compress) compress{};
    decltype(&::Decompress) decompress{};
    bool Available() const {return createCompressor&&createDecompressor&&closeCompressor&&closeDecompressor&&compress&&decompress;}
};
const CompressionApi& Cabinet() {
    static const CompressionApi api=[]{
        CompressionApi a{};
        // Wine reports itself through ntdll; its cabinet.dll may gain stubs that are not real.
        if(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version"))return a;
        if(HMODULE m=LoadLibraryW(L"cabinet.dll")){
            a.createCompressor=reinterpret_cast<decltype(a.createCompressor)>(GetProcAddress(m,"CreateCompressor"));
            a.createDecompressor=reinterpret_cast<decltype(a.createDecompressor)>(GetProcAddress(m,"CreateDecompressor"));
            a.closeCompressor=reinterpret_cast<decltype(a.closeCompressor)>(GetProcAddress(m,"CloseCompressor"));
            a.closeDecompressor=reinterpret_cast<decltype(a.closeDecompressor)>(GetProcAddress(m,"CloseDecompressor"));
            a.compress=reinterpret_cast<decltype(a.compress)>(GetProcAddress(m,"Compress"));
            a.decompress=reinterpret_cast<decltype(a.decompress)>(GetProcAddress(m,"Decompress"));
        }
        if(!a.Available()){
            a={};
            if(auto* host=greeneye::Host())host->log("Town Portal: no Windows Compression API here (Wine / Proton); portal pictures are not shared with other players or kept between sessions");
        }
        return a;
    }();
    return api;
}
}

namespace portal::sharing {
namespace {
// Byte planes improve lossless compression of half floats, especially depth.
std::vector<uint8_t> Planes(std::span<const uint8_t> input,bool inverse,size_t stride=8) {
    std::vector<uint8_t> out(input.size());const size_t n=input.size()/stride;
    for(size_t channel=0;channel<stride;++channel)for(size_t p=0;p<n;++p){
        if(inverse)out[p*stride+channel]=input[channel*n+p];else out[channel*n+p]=input[p*stride+channel];
    }
    return out;
}
bool FinitePixels(std::span<const uint8_t> p){
    for(size_t i=0;i<p.size();i+=2)if((p[i+1]&0x7c)==0x7c)return false;
    return true;
}
}
bool Encode(const Image& image,Encoded& out) {
    if(image.pixels.size()!=kRawBytes||!ValidMetadata(image.meta)||!FinitePixels(image.pixels))return false;
    out={image.key,image.revision,image.meta,Checksum(image.pixels),image.pixels};out.meta.codec=0;
    COMPRESSOR_HANDLE c{};
    // Without the Compression API (Wine) a picture is not shared at all: 16 MB uncompressed
    // would hold up the other pictures for long.
    if(!Cabinet().Available())return false;
    if(!Cabinet().createCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&c))return true;
    auto planes=Planes(image.pixels,false);SIZE_T size{};
    Cabinet().compress(c,planes.data(),planes.size(),nullptr,0,&size);
    // The sizing call reports capacity (which can exceed raw size), not the
    // eventual compressed length. Compare the actual size after compression.
    if(size&&size<=kMaxEncoded){
        std::vector<uint8_t> packed(size);
        if(Cabinet().compress(c,planes.data(),planes.size(),packed.data(),packed.size(),&size)&&size<kRawBytes){
            packed.resize(size);out.bytes=std::move(packed);out.meta.codec=1;
        }
    }
    Cabinet().closeCompressor(c);return true;
}
bool Decode(const Encoded& encoded,Image& out) {
    if(!ValidMetadata(encoded.meta)||encoded.bytes.empty()||encoded.bytes.size()>kMaxEncoded)return false;
    std::vector<uint8_t> pixels;
    if(!encoded.meta.codec){if(encoded.bytes.size()!=kRawBytes)return false;pixels=encoded.bytes;}
    else {
        DECOMPRESSOR_HANDLE d{};
        if(!Cabinet().Available()||!Cabinet().createDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&d))return false;
        // Bound the output even for malicious/corrupt compression headers.
        std::vector<uint8_t> planes(kRawBytes);SIZE_T size{};
        bool ok=Cabinet().decompress(d,encoded.bytes.data(),encoded.bytes.size(),planes.data(),planes.size(),&size)!=FALSE;
        Cabinet().closeDecompressor(d);if(!ok||size!=kRawBytes)return false;
        pixels=Planes(planes,true);
    }
    if(Checksum(pixels)!=encoded.crc||!FinitePixels(pixels))return false;
    out.key=encoded.key;out.revision=encoded.revision;out.meta=encoded.meta;out.pixels=std::move(pixels);return true;
}
}

namespace portal::homecache {
std::vector<uint8_t> EncodeFile(const Key& key,const sharing::Image& image) {
    if(image.pixels.size()!=sharing::kRawBytes||!sharing::ValidMetadata(image.meta)||!sharing::FinitePixels(image.pixels))return {};
    std::vector<uint8_t> packed(kPackedBytes);
    const uint8_t* in=image.pixels.data();uint8_t* p=packed.data();
    for(size_t i=0;i<kPackedBytes/6;++i,in+=8,p+=6){
        uint16_t h[4];std::memcpy(h,in,8);
        const uint32_t colour=PackColour(h[0],h[1],h[2]);
        std::memcpy(p,&colour,4);std::memcpy(p+4,&h[3],2);
    }
    FileHeader header{};header.account=key.account;std::memcpy(header.altar,key.altar,sizeof(header.altar));
    header.meta=image.meta;header.meta.codec=1;header.crc=sharing::Checksum(packed);
    const auto planes=sharing::Planes(packed,false,6);
    COMPRESSOR_HANDLE c{};
    if(!Cabinet().Available()||!Cabinet().createCompressor(COMPRESS_ALGORITHM_LZMS,nullptr,&c))return {};
    SIZE_T size{};Cabinet().compress(c,planes.data(),planes.size(),nullptr,0,&size);
    std::vector<uint8_t> out;
    if(size){
        out.resize(sizeof(header)+size);
        if(Cabinet().compress(c,planes.data(),planes.size(),out.data()+sizeof(header),size,&size)){
            out.resize(sizeof(header)+size);header.size=uint32_t(size);
            std::memcpy(out.data(),&header,sizeof(header));
        } else out.clear();
    }
    Cabinet().closeCompressor(c);return out;
}
bool DecodeFile(const Key& key,std::span<const uint8_t> file,sharing::Image& out) {
    if(file.size()<sizeof(FileHeader))return false;
    FileHeader h{};std::memcpy(&h,file.data(),sizeof(h));
    if(h.magic!=kFileMagic||(h.version!=kFileVersion&&h.version!=2)||h.account!=key.account||std::memcmp(h.altar,key.altar,sizeof(h.altar))||
       !h.size||h.size>sharing::kMaxEncoded||file.size()!=sizeof(h)+h.size||!sharing::ValidMetadata(h.meta))return false;
    DECOMPRESSOR_HANDLE d{};
    if(!Cabinet().Available()||!Cabinet().createDecompressor(h.version==2?COMPRESS_ALGORITHM_XPRESS_HUFF:COMPRESS_ALGORITHM_LZMS,nullptr,&d))return false;
    // Bounded output even for a damaged compression header.
    std::vector<uint8_t> planes(kPackedBytes);SIZE_T size{};
    const bool ok=Cabinet().decompress(d,file.data()+sizeof(h),h.size,planes.data(),planes.size(),&size)!=FALSE;
    Cabinet().closeDecompressor(d);
    if(!ok||size!=kPackedBytes)return false;
    const auto packed=sharing::Planes(planes,true,6);
    if(sharing::Checksum(packed)!=h.crc)return false;
    std::vector<uint8_t> pixels(sharing::kRawBytes);
    const uint8_t* p=packed.data();uint8_t* o=pixels.data();
    for(size_t i=0;i<kPackedBytes/6;++i,p+=6,o+=8){
        uint32_t colour;uint16_t depth;std::memcpy(&colour,p,4);std::memcpy(&depth,p+4,2);
        const uint16_t px[4]={SmallToHalf(colour&0x7ff,4),SmallToHalf(colour>>11&0x7ff,4),SmallToHalf(colour>>22,5),depth};
        std::memcpy(o,px,8);
    }
    if(!sharing::FinitePixels(pixels))return false;
    out={};out.meta=h.meta;out.pixels=std::move(pixels);
    return true;
}
}
