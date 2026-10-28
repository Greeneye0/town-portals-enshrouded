#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>
#include "portal_snapshot_cache.h"
#pragma comment(lib,"Cabinet.lib")

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
    if(!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&c))return true;
    auto planes=Planes(image.pixels,false);SIZE_T size{};
    Compress(c,planes.data(),planes.size(),nullptr,0,&size);
    // The sizing call reports capacity (which can exceed raw size), not the
    // eventual compressed length. Compare the actual size after compression.
    if(size&&size<=kMaxEncoded){
        std::vector<uint8_t> packed(size);
        if(Compress(c,planes.data(),planes.size(),packed.data(),packed.size(),&size)&&size<kRawBytes){
            packed.resize(size);out.bytes=std::move(packed);out.meta.codec=1;
        }
    }
    CloseCompressor(c);return true;
}
bool Decode(const Encoded& encoded,Image& out) {
    if(!ValidMetadata(encoded.meta)||encoded.bytes.empty()||encoded.bytes.size()>kMaxEncoded)return false;
    std::vector<uint8_t> pixels;
    if(!encoded.meta.codec){if(encoded.bytes.size()!=kRawBytes)return false;pixels=encoded.bytes;}
    else {
        DECOMPRESSOR_HANDLE d{};
        if(!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&d))return false;
        // Bound the output even for malicious/corrupt compression headers.
        std::vector<uint8_t> planes(kRawBytes);SIZE_T size{};
        bool ok=Decompress(d,encoded.bytes.data(),encoded.bytes.size(),planes.data(),planes.size(),&size)!=FALSE;
        CloseDecompressor(d);if(!ok||size!=kRawBytes)return false;
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
    if(!CreateCompressor(COMPRESS_ALGORITHM_LZMS,nullptr,&c))return {};
    SIZE_T size{};Compress(c,planes.data(),planes.size(),nullptr,0,&size);
    std::vector<uint8_t> out;
    if(size){
        out.resize(sizeof(header)+size);
        if(Compress(c,planes.data(),planes.size(),out.data()+sizeof(header),size,&size)){
            out.resize(sizeof(header)+size);header.size=uint32_t(size);
            std::memcpy(out.data(),&header,sizeof(header));
        } else out.clear();
    }
    CloseCompressor(c);return out;
}
bool DecodeFile(const Key& key,std::span<const uint8_t> file,sharing::Image& out) {
    if(file.size()<sizeof(FileHeader))return false;
    FileHeader h{};std::memcpy(&h,file.data(),sizeof(h));
    if(h.magic!=kFileMagic||(h.version!=kFileVersion&&h.version!=2)||h.account!=key.account||std::memcmp(h.altar,key.altar,sizeof(h.altar))||
       !h.size||h.size>sharing::kMaxEncoded||file.size()!=sizeof(h)+h.size||!sharing::ValidMetadata(h.meta))return false;
    DECOMPRESSOR_HANDLE d{};
    if(!CreateDecompressor(h.version==2?COMPRESS_ALGORITHM_XPRESS_HUFF:COMPRESS_ALGORITHM_LZMS,nullptr,&d))return false;
    // Bounded output even for a damaged compression header.
    std::vector<uint8_t> planes(kPackedBytes);SIZE_T size{};
    const bool ok=Decompress(d,file.data()+sizeof(h),h.size,planes.data(),planes.size(),&size)!=FALSE;
    CloseDecompressor(d);
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
