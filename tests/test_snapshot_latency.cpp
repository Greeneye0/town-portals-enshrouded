#include "../src/portal_snapshot_wire.h"
#include <cstdio>
#include <cstdlib>
#include <map>
using namespace portal;
using namespace portal::sharing;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
constexpr uint64_t kOwner=76561197993547076ull,kGuest=76561198000000001ull;
struct Packet {uint64_t from{},to{};std::vector<uint8_t> bytes;};
struct Link {
    Exchange owner{kOwner,101},guest{kGuest,202};
    std::multimap<uint64_t,Packet> packets;
    std::map<uint64_t,uint64_t> linkFree;
    std::vector<std::shared_ptr<Image>> received;
    uint64_t now=1000,first{},delay=80;
    size_t highWater{},dropped{},duplicated{},backpressure{};
    bool faults{},legacy{};
    Link(){
        owner.send=[&](uint64_t to,std::span<const uint8_t> bytes){return Send(kOwner,to,bytes);};
        guest.send=[&](uint64_t to,std::span<const uint8_t> bytes){return Send(kGuest,to,bytes);};
        guest.deliver=[&](std::shared_ptr<Image> image){if(!first)first=now;received.push_back(image);return true;};
    }
    bool Send(uint64_t from,uint64_t to,std::span<const uint8_t> bytes){
        Header h{};std::memcpy(&h,bytes.data(),sizeof(h));
        if(faults&&h.kind==Kind::Data&&h.offset==kChunk&&!backpressure){++backpressure;return false;}
        if(faults&&h.kind==Kind::Data&&h.offset==kChunk*2&&!dropped){++dropped;return true;}
        Packet p{from,to,{bytes.begin(),bytes.end()}};
        // r2 peers send Hello with no capability fields.
        if(legacy&&h.kind==Kind::Hello){h.offset=h.total=0;std::memcpy(p.bytes.data(),&h,sizeof(h));}
        auto& available=linkFree[from];available=std::max(available,now)+(bytes.size()+8191)/8192;
        uint64_t due=available+delay;
        if(faults&&h.kind==Kind::Data&&h.offset==0)due+=160; // out-of-order data
        packets.emplace(due,p);
        if(faults&&h.kind==Kind::Data&&h.offset==kChunk*3&&!duplicated){++duplicated;packets.emplace(due+20,p);}
        highWater=std::max(highWater,packets.size());return true;
    }
    void Step(){
        owner.Authorize(kGuest,202,now);guest.Authorize(kOwner,101,now);
        while(!packets.empty()&&packets.begin()->first<=now){
            auto node=packets.extract(packets.begin());auto& p=node.mapped();
            (p.to==kOwner?owner:guest).Receive(p.from,p.bytes,now);
        }
        owner.Step(now);guest.Step(now);now+=20;
    }
    uint64_t Run(size_t count){const auto start=now;for(int i=0;i<20000&&received.size()<count;++i)Step();CHECK(received.size()==count);return now-start;}
};
Image Fixture(){
    Image image{};image.key={kOwner,300,0};image.epoch=101;image.revision=1;
    auto& m=image.meta;m.nz=1;m.exposure=1;m.clip[0]=m.clip[5]=m.clip[10]=1;
    image.pixels.resize(kRawBytes);uint32_t seed=72643;
    for(size_t i=0;i<image.pixels.size();i+=2){seed=seed*1664525+1013904223;uint16_t half=uint16_t((seed>>16)%0x7bff);std::memcpy(image.pixels.data()+i,&half,2);}
    return image;
}
#ifndef SNAPSHOT_BASELINE
void ReceiverBounds(const PortalInstance& portal,const std::shared_ptr<Encoded>& encoded){
    Exchange receiver(kGuest,202);receiver.Context({portal});receiver.Authorize(kOwner,101,1000);
    receiver.send=[](uint64_t,std::span<const uint8_t>){return true;};
    Header h{};h.kind=Kind::Manifest;h.from=101;h.to=202;h.portal=portal.portalId;
    h.revision=encoded->revision;h.total=uint32_t(encoded->bytes.size());h.crc=encoded->crc;
    auto put=[&](const Header& header,std::span<const uint8_t> payload){
        std::vector<uint8_t> packet(sizeof(header)+payload.size());std::memcpy(packet.data(),&header,sizeof(header));
        if(!payload.empty())std::memcpy(packet.data()+sizeof(header),payload.data(),payload.size());
        receiver.Receive(kOwner,packet,1000);
    };
    put(h,{reinterpret_cast<const uint8_t*>(&encoded->meta),sizeof(Metadata)});receiver.Step(1000);
    h.kind=Kind::Data;const auto chunk=std::span(encoded->bytes).first(kChunk);
    for(auto offset:{UINT32_MAX,h.total,1u,kChunk}){auto bad=h;bad.offset=offset;put(bad,chunk);}
    auto bad=h;bad.total++;put(bad,chunk);put(h,chunk.first(kChunk-1));
    CHECK(receiver.rejected==6&&receiver.DownloadBytes()==0);
    put(h,chunk);put(h,chunk);CHECK(receiver.DownloadBytes()==kChunk&&receiver.receivedBytes==kChunk);
    CHECK(receiver.rejected==6); // harmless duplicate must not reset the download
    bad=h;bad.kind=Kind::Manifest;bad.crc++;
    put(bad,{reinterpret_cast<const uint8_t*>(&encoded->meta),sizeof(Metadata)});
    CHECK(receiver.rejected==7&&receiver.DownloadBytes()==kChunk);
    receiver.Context({});put(h,chunk);CHECK(receiver.rejected==8&&receiver.DownloadTotal()==0);
    std::puts("Receiver bounds: invalid offsets, unsolicited chunks, duplicates and changed manifests rejected safely");
}
void UploadPacing(const PortalInstance& portal,const std::shared_ptr<Encoded>& encoded){
    // All sixteen players keep requests pending. Global bandwidth must be
    // bounded and low-numbered Steam IDs must not monopolize the upload queue.
    Exchange hub(kOwner,101);hub.Context({portal});hub.Publish(encoded);
    std::array<uint64_t,kMaxPeers> bytes{};
    std::array<uint32_t,kMaxPeers> next{};
    struct Request {size_t peer;uint32_t offset;};
    std::multimap<uint64_t,Request> pending;
    uint64_t now=1000,total{};
    auto request=[&](size_t peer,uint32_t offset){
        Header h{};h.kind=Kind::Want;h.from=200+peer;h.to=101;h.portal=portal.portalId;
        h.revision=encoded->revision;h.offset=offset;h.total=uint32_t(encoded->bytes.size());h.crc=encoded->crc;
        hub.Receive(kGuest+peer,{reinterpret_cast<const uint8_t*>(&h),sizeof(h)},now);
    };
    hub.send=[&](uint64_t to,std::span<const uint8_t> packet){
        Header h{};std::memcpy(&h,packet.data(),sizeof(h));
        if(h.kind==Kind::Data){
            const auto peer=size_t(to-kGuest);CHECK(peer<bytes.size());
            bytes[peer]+=packet.size()-sizeof(h);total+=packet.size()-sizeof(h);
            pending.emplace(now+160,Request{peer,next[peer]++*kChunk});
        }return true;
    };
    for(size_t peer=0;peer<kMaxPeers;++peer){
        hub.Authorize(kGuest+peer,200+peer,now);
        for(uint32_t chunk=0;chunk<kRequestWindow;++chunk)request(peer,chunk*kChunk);
        next[peer]=kRequestWindow;
    }
    CHECK(hub.PendingUploads()==kRequestWindow*kMaxPeers);
    // Repeated requests are deduplicated; a flood cannot enlarge the queues.
    for(int repeat=0;repeat<100;++repeat)request(0,0);
    for(uint32_t chunk=kRequestWindow;chunk<kRequestWindow*3;++chunk)request(0,chunk*kChunk);
    CHECK(hub.PendingUploads()==kRequestWindow*kMaxPeers);
    for(int frame=0;frame<500;++frame){
        while(!pending.empty()&&pending.begin()->first<=now){auto r=pending.extract(pending.begin()).mapped();request(r.peer,r.offset);}
        hub.Step(now);
        CHECK(hub.PendingUploads()<=kRequestWindow*kMaxPeers);
        CHECK(total<=uint64_t(kTotalBurst+(now-1000)*kTotalBytesPerMs));
        now+=20;
    }
    const auto [least,most]=std::minmax_element(bytes.begin(),bytes.end());
    CHECK(*least>800000&&*most-*least<=kChunk*2);
    for(auto n:bytes)CHECK(n<=uint64_t(kPeerBurst+10000*kPeerBytesPerMs));
    // Cancelling or replacing a portal cancels already queued image chunks.
    auto replacement=std::make_shared<Encoded>(*encoded);replacement->revision++;
    hub.Publish(replacement);CHECK(hub.PendingUploads()==0);
    hub.Context({});hub.Step(now);CHECK(hub.PendingUploads()==0);
    std::printf("16-player pacing: total=%llu bytes/10 s, each=%llu..%llu bytes, bounded/fair queues\n",total,*least,*most);
}
#endif
int main(){
    const Image image=Fixture();auto encoded=std::make_shared<Encoded>();CHECK(Encode(image,*encoded));
    PortalInstance p{};p.active=true;p.portalId=300;p.ownerAccount=kOwner;
    auto second=std::make_shared<Encoded>(*encoded);second->key.side=1;
    Link normal;normal.owner.Context({p});normal.guest.Context({p});normal.owner.Publish(encoded);normal.owner.Publish(second);
    const auto normalMs=normal.Run(2);
    for(const auto& got:normal.received)CHECK(got->pixels==image.pixels);
    std::printf("160 ms round-trip, %zu bytes/endpoint: first=%llu ms, both=%llu ms, network queue peak=%zu\n",encoded->bytes.size(),normal.first-1000,normalMs,normal.highWater);
#ifndef SNAPSHOT_BASELINE
    CHECK(normalMs<40000);CHECK(normal.highWater<=128);
    ReceiverBounds(p,encoded);
    UploadPacing(p,encoded);
    Link lossy;lossy.faults=true;lossy.owner.Context({p});lossy.guest.Context({p});lossy.owner.Publish(encoded);
    const auto lossyMs=lossy.Run(1);CHECK(lossy.received[0]->pixels==image.pixels);
    CHECK(lossy.dropped==1&&lossy.duplicated==1&&lossy.backpressure==1);
    CHECK(lossyMs<25000&&lossy.highWater<=128);
    std::printf("Loss/reorder/duplicate/send backpressure: %llu ms, exact pixels preserved\n",lossyMs);
    // No advertised window capability means single-request compatibility mode.
    Link old;old.legacy=true;old.owner.Context({p});old.guest.Context({p});old.owner.Publish(encoded);
    auto oldMs=old.Run(1);CHECK(old.received[0]->pixels==image.pixels);
    CHECK(oldMs>normalMs/2&&old.highWater<=16);
    std::printf("Legacy single-request peer: %llu ms, exact pixels preserved\n",oldMs);
#endif
}
