#include "../src/portal_snapshot_wire.h"
#include "../src/portal_snapshot_identity.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <random>
using namespace portal;
using namespace portal::sharing;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
constexpr uint64_t kOwner=76561197993547076ull,kGuest=76561198000000001ull;
constexpr uint64_t kOwnerGameAccount=0xdeb5047ae1c664e7ull; // observed native chat sender
static_assert(SteamGameAccount(kOwner)==kOwnerGameAccount && kOwner!=kOwnerGameAccount);
void AuthorizeChat(Exchange& receiver,uint64_t steam,uint64_t nonce,uint64_t now){
    char line[64]{};FormatAnnouncement(line,sizeof(line),steam,nonce);
    Announcement parsed{};CHECK(ParseAnnouncement(SteamGameAccount(steam),line,parsed));
    CHECK(parsed.steam==steam&&parsed.nonce==nonce);
    receiver.Authorize(parsed.steam,parsed.nonce,now);
}
void IdentityTests(){
    Announcement a{123,456};
    CHECK(ParseAnnouncement(kOwnerGameAccount,"/tpi2 76561197993547076 101",a));
    CHECK(a.steam==kOwner&&a.nonce==101);
    CHECK(!ParseAnnouncement(kOwner,"/tpi2 76561197993547076 101",a));
    CHECK(!ParseAnnouncement(SteamGameAccount(kGuest),"/tpi2 76561197993547076 101",a));
    CHECK(!ParseAnnouncement(0,"/tpi2 76561197993547076 101",a));
    for(const auto* bad:{"/tpi1 101","/tpi2 76561197993547076 0","/tpi2 76561197993547076 -1",
        "/tpi2 76561197993547076 1 junk","/tpi2 76561197993547076 1 2","/tpi2 76561197993547076",
        "/tpi2 76561197993547076 18446744073709551616","/tpi2 76561197993547076  101",
        "/tpi2 -1 101","/tpi2 18446744073709551616 101","/tpi2 11 101"}){
        CHECK(!ParseAnnouncement(kOwnerGameAccount,bad,a));CHECK(a.steam==kOwner&&a.nonce==101);
    }
    CHECK(!SteamPlayer(0)&&!SteamPlayer(11)&&!SteamPlayer(0x7072746c00000001ull));
    CHECK(!SteamPlayer(0x0110000100000000ull)&&!SteamPlayer(0x0170000000000001ull));
}
struct Packet{uint64_t from,to;std::vector<uint8_t> bytes;};
struct Pair {
    Exchange owner{kOwner,101},guest{kGuest,202};
    std::deque<Packet> queue;std::vector<std::shared_ptr<Image>> received;
    uint64_t now=1000;size_t dropped{},duplicates{};
    Pair(){
        owner.send=[&](uint64_t to,std::span<const uint8_t> bytes){CHECK(to==kGuest);queue.push_back({kOwner,to,{bytes.begin(),bytes.end()}});return true;};
        guest.send=[&](uint64_t to,std::span<const uint8_t> bytes){CHECK(to==kOwner);queue.push_back({kGuest,to,{bytes.begin(),bytes.end()}});return true;};
        guest.deliver=[&](std::shared_ptr<Image> image){received.push_back(image);return true;};
    }
    void Step(bool loss=false){
        AuthorizeChat(owner,kGuest,202,now);AuthorizeChat(guest,kOwner,101,now);
        owner.Step(now);guest.Step(now);
        size_t n=queue.size();
        while(n--){auto p=std::move(queue.front());queue.pop_front();Header h{};std::memcpy(&h,p.bytes.data(),sizeof(h));
            if(loss&&h.kind==Kind::Data&&h.offset==kChunk&&!dropped){++dropped;continue;}
            auto& dest=p.to==kOwner?owner:guest;dest.Receive(p.from,p.bytes,now);
            if(loss&&h.kind==Kind::Data&&!duplicates){dest.Receive(p.from,p.bytes,now);++duplicates;}
        }now+=20;
    }
    void Until(size_t count,bool loss=false){for(int i=0;i<30000&&received.size()<count;++i)Step(loss);CHECK(received.size()==count);}
};
Image Fixture(){
    Image image{};image.key={kOwner,300,0};image.epoch=101;image.revision=9;
    auto& m=image.meta;m.nx=0;m.nz=1;m.exposure=1;m.x=200;m.y=70;m.z=300;m.cx=200;m.cy=72;m.cz=300;
    m.clip[0]=1;m.clip[5]=1;m.clip[10]=1;m.clip[11]=-300;
    image.pixels.resize(kRawBytes);std::mt19937 random(2386);
    // Exact finite half-float RGB/depth, including sharp foreground/background edges.
    for(size_t i=0;i<image.pixels.size();i+=2){uint16_t half=uint16_t(random()%0x7bff);std::memcpy(image.pixels.data()+i,&half,2);}
    return image;
}
int main(){
    IdentityTests();
    Image image=Fixture();Encoded encoded{};CHECK(Encode(image,encoded));
    Image decoded{};CHECK(Decode(encoded,decoded));CHECK(decoded.pixels==image.pixels);
    CHECK(std::memcmp(&decoded.meta,&image.meta,offsetof(Metadata,codec))==0);
    auto corrupted=encoded;corrupted.crc^=1;CHECK(!Decode(corrupted,decoded));
    corrupted=encoded;corrupted.bytes.resize(9);CHECK(!Decode(corrupted,decoded));
    corrupted=encoded;corrupted.meta.clip[0]=std::numeric_limits<double>::quiet_NaN();CHECK(!Decode(corrupted,decoded));
    corrupted=encoded;corrupted.meta.codec=99;CHECK(!Decode(corrupted,decoded));
    corrupted=encoded;corrupted.bytes.resize(kMaxEncoded+1);CHECK(!Decode(corrupted,decoded));
    Image nonfinite=image;nonfinite.pixels[0]=0;nonfinite.pixels[1]=0x7c;Encoded ignored{};CHECK(!Encode(nonfinite,ignored));
    auto flat=image;std::fill(flat.pixels.begin(),flat.pixels.end(),uint8_t(0));CHECK(Encode(flat,ignored));
    CHECK(ignored.meta.codec==1&&ignored.bytes.size()<kRawBytes/8);CHECK(Decode(ignored,decoded));CHECK(decoded.pixels==flat.pixels);

    PortalInstance p{};p.active=true;p.portalId=300;p.ownerAccount=kOwner;
    Pair pair;pair.owner.Context({p});pair.guest.Context({p});pair.owner.Publish(std::make_shared<Encoded>(encoded));
    pair.Until(1,true);CHECK(pair.received[0]->pixels==image.pixels);CHECK(pair.dropped==1&&pair.duplicates==1);
    CHECK(pair.received[0]->epoch==202&&pair.received[0]->key==image.key);
    for(int i=0;i<400;++i)pair.Step();CHECK(pair.received.size()==1); // repeated ads don't resend
    auto second=std::make_shared<Encoded>(encoded);second->key.side=1;second->revision=10;
    pair.owner.Publish(second);pair.Until(2);CHECK(pair.received.back()->key.side==1);
    CHECK(pair.received.back()->pixels==image.pixels);
    // Late join gets both sides from the owner's existing cache.
    pair.guest=Exchange(kGuest,202);pair.guest.Context({p});pair.received.clear();
    pair.guest.send=[&](uint64_t to,std::span<const uint8_t> bytes){CHECK(to==kOwner);pair.queue.push_back({kGuest,to,{bytes.begin(),bytes.end()}});return true;};
    pair.guest.deliver=[&](std::shared_ptr<Image> v){pair.received.push_back(v);return true;};
    pair.Until(2);CHECK(pair.received[0]->pixels==image.pixels&&pair.received[1]->pixels==image.pixels);
    // A visitor with no images and no portal of their own receives BOTH cached
    // endpoints purely through discovery/transfer. No trip or capture occurs.
    CHECK(pair.guest.PublishedCount()==0&&pair.owner.PublishedCount()==2);
    CHECK(pair.guest.PeerCount()==1&&pair.owner.PeerCount()==1);
    CHECK(pair.guest.completed==2&&pair.guest.receivedBytes>0&&pair.guest.sendFailures==0);
    CHECK(pair.received[0]->key.side!=pair.received[1]->key.side);

    Exchange unavailable(kOwner,101);unavailable.Authorize(kGuest,202,1000);
    unavailable.send=[](uint64_t,std::span<const uint8_t>){return false;};unavailable.Step(1000);
    CHECK(unavailable.sendFailures==1&&unavailable.sentPackets==0);
    unavailable.send=[](uint64_t,std::span<const uint8_t>){return true;};unavailable.Step(6000);
    CHECK(unavailable.sendFailures==1&&unavailable.sentPackets==1);

    // An owner recapturing mid-transfer supersedes the previous revision at
    // once, instead of leaving the receiver waiting for obsolete chunks.
    Pair changed;changed.owner.Context({p});changed.guest.Context({p});
    changed.owner.Publish(std::make_shared<Encoded>(encoded));
    for(int i=0;i<20;++i)changed.Step();
    auto newer=std::make_shared<Encoded>(ignored);newer->revision=100;
    changed.owner.Publish(newer);changed.Until(1);
    CHECK(changed.received[0]->revision==100&&changed.received[0]->pixels==flat.pixels);

    // Complete but corrupted data is discarded before it can reach the GPU.
    Pair bad;bad.owner.Context({p});bad.guest.Context({p});
    auto broken=std::make_shared<Encoded>(ignored);broken->crc^=1;bad.owner.Publish(broken);
    for(int i=0;i<500;++i)bad.Step();CHECK(bad.received.empty()&&bad.guest.rejected>0);

    Header h{};h.kind=Kind::Manifest;h.from=101;h.to=202;h.portal=300;h.revision=100;h.total=uint32_t(encoded.bytes.size());h.crc=encoded.crc;
    std::vector<uint8_t> packet(sizeof(h)+sizeof(Metadata));
    auto pack=[&]{std::memcpy(packet.data(),&h,sizeof(h));std::memcpy(packet.data()+sizeof(h),&encoded.meta,sizeof(Metadata));};pack();
    auto before=pair.guest.rejected;
    pair.guest.Receive(99,packet,pair.now); // unknown sender
    h.to=999;pack();pair.guest.Receive(kOwner,packet,pair.now); // old world
    h.to=202;h.from=999;pack();pair.guest.Receive(kOwner,packet,pair.now); // old remote session
    h.from=101;h.portal=301;pack();pair.guest.Receive(kOwner,packet,pair.now); // wrong portal
    h.portal=300;h.side=2;pack();pair.guest.Receive(kOwner,packet,pair.now);
    h.side=0;h.total=kMaxEncoded+1;pack();pair.guest.Receive(kOwner,packet,pair.now);
    CHECK(pair.guest.rejected==before+6);
    pair.guest.Context({});h.total=uint32_t(encoded.bytes.size());pack();pair.guest.Receive(kOwner,packet,pair.now);
    CHECK(pair.guest.rejected==before+7); // closed portal cannot accept images
    pair.guest.Context({p});pair.guest.Step(pair.now+21000);pair.guest.Receive(kOwner,packet,pair.now+21000);
    CHECK(pair.guest.rejected==before+8); // peer left the game
    // Malformed/future packet kinds and truncated buffers must remain bounded.
    std::mt19937 fuzz(45);
    for(int i=0;i<10000;++i){std::vector<uint8_t> junk(fuzz()%512);for(auto& b:junk)b=uint8_t(fuzz());pair.guest.Receive(kOwner,junk,pair.now);}
    std::printf("PASS: native account hash -> Steam identity, spoof rejection, cached endpoints without travel, lossless RGB/depth (%zu / %u bytes), late join, retry, diagnostics and session/owner isolation\n",encoded.bytes.size(),kRawBytes);
}
