#pragma once
// Versioned, bounded snapshot transport. No pointers, native handles or image codecs
// cross the wire. RGB and camera-depth are preserved together as RGBA16F.
#include "portal_types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace portal::sharing {
constexpr uint32_t kWidth=2048,kHeight=1024,kRawBytes=kWidth*kHeight*8;
constexpr uint32_t kChunk=32*1024,kMaxEncoded=kRawBytes+65536;
constexpr uint32_t kMagic=0x31535054; // TPS1
constexpr int kChannel=0x54504f; // private channel; never close a game's entire Steam session
constexpr size_t kMaxPeers=16,kMaxViews=kMaxPortals*2;
constexpr uint32_t kRequestWindow=16,kMaxChunks=(kMaxEncoded+kChunk-1)/kChunk;
constexpr uint32_t kWindowCapability=1;
constexpr double kPeerBytesPerMs=1024.,kTotalBytesPerMs=2048.;
constexpr double kPeerBurst=kChunk*4.,kTotalBurst=kChunk*8.;
enum class Kind:uint16_t {Hello=1,Manifest,Want,Data};
#pragma pack(push,1)
struct Header {
    uint32_t magic=kMagic; uint16_t version=1; Kind kind{};
    uint64_t from{},to{},portal{},revision{};
    uint32_t side{},offset{},total{},crc{};
};
struct Metadata {
    float x{},y{},z{},nx{},nz{},cx{},cy{},cz{},exposure{};
    double clip[12]{};
    uint32_t shroud{},codec{}; // codec 0: raw, 1: byte-planar XPRESS_HUFF
};
#pragma pack(pop)
static_assert(sizeof(Header)==56 && sizeof(Metadata)==140);
struct Key {
    uint64_t owner{},portal{}; uint32_t side{}; // 0: field destination, 1: altar destination
    auto operator<=>(const Key&) const = default;
};
struct Image {
    Key key{}; uint64_t epoch{},revision{}; Metadata meta{};
    std::vector<uint8_t> pixels;
};
struct Encoded {
    Key key{};uint64_t revision{};Metadata meta{};uint32_t crc{};
    std::vector<uint8_t> bytes;
};
inline bool ValidMetadata(const Metadata& m) {
    const float positions[]={m.x,m.y,m.z,m.cx,m.cy,m.cz};
    for(float f:positions)if(!std::isfinite(f)||std::abs(f)>100000)return false;
    if(!std::isfinite(m.nx)||!std::isfinite(m.nz)||std::abs(m.nx*m.nx+m.nz*m.nz-1)>0.02f)return false;
    if(!std::isfinite(m.exposure)||m.exposure<1e-8f||m.exposure>1e8f||m.shroud>1||m.codec>1)return false;
    for(double f:m.clip)if(!std::isfinite(f)||std::abs(f)>1e8)return false;
    double length=m.clip[8]*m.clip[8]+m.clip[9]*m.clip[9]+m.clip[10]*m.clip[10];
    return length>0.5&&length<2;
}
inline uint32_t Checksum(std::span<const uint8_t> bytes) {
    static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t n=0;n<256;++n){uint32_t c=n;for(int i=0;i<8;++i)c=(c>>1)^((c&1)?0xedb88320u:0);t[n]=c;}return t;}();
    uint32_t c=~0u;for(auto b:bytes)c=(c>>8)^table[(c^b)&255];return ~c;
}
bool Encode(const Image& image,Encoded& out);
bool Decode(const Encoded& encoded,Image& out);

// Runs exclusively on the transfer worker. Tests inject two virtual transports.
class Exchange {
    struct Offer {Header h{};Metadata meta{};uint64_t seen{};};
    struct Peer {
        uint64_t nonce{},seen{},hello{},advertised{},budgetAt{};
        double budget=kPeerBurst;uint32_t window=1;
        std::deque<Header> uploads;
    };
    struct Chunk {uint64_t requested{};bool received{};};
    struct Download {
        Key key{};Offer offer{};uint64_t started{};
        uint32_t receivedBytes{},base{};
        std::array<Chunk,kMaxChunks> chunks{};
        std::vector<uint8_t> bytes;
    };
    std::map<uint64_t,Peer> peers_;
    std::map<Key,Offer> offers_;
    std::map<Key,uint64_t> received_;
    std::map<Key,std::shared_ptr<const Encoded>> owned_;
    Download download_{};
    std::shared_ptr<Image> ready_;
    std::vector<PortalInstance> portals_;
    uint64_t local_{},nonce_{},budgetAt_{},sendCursor_{};double budget_=kTotalBurst;
    bool Active(const Key& k) const {
        if(k.side>1)return false;
        for(const auto& p:portals_)if(p.active&&!p.closing&&p.portalId==k.portal&&p.ownerAccount==k.owner)return true;
        return false;
    }
    bool Send(uint64_t account,Header h,std::span<const uint8_t> payload={}) {
        auto i=peers_.find(account);if(i==peers_.end())return false;
        h.from=nonce_;h.to=i->second.nonce;
        std::vector<uint8_t> packet(sizeof(h)+payload.size());
        std::memcpy(packet.data(),&h,sizeof(h));if(!payload.empty())std::memcpy(packet.data()+sizeof(h),payload.data(),payload.size());
        const bool ok=send && send(account,packet);
        if(ok)++sentPackets;else ++sendFailures;
        return ok;
    }
    std::shared_ptr<const Encoded> Upload(const Header& h) const {
        auto i=owned_.find({local_,h.portal,h.side});
        if(i==owned_.end())return {};
        const auto& v=i->second;
        if(h.revision!=v->revision||h.total!=v->bytes.size()||h.crc!=v->crc||
           h.offset>=v->bytes.size()||h.offset%kChunk)return {};
        return v;
    }
    void TrimUploads(){
        for(auto& [id,p]:peers_){(void)id;std::erase_if(p.uploads,[&](const auto& h){return !Upload(h);});}
    }
    void SendUploads(uint64_t now){
        budget_=std::min(kTotalBurst,budget_+double(now-budgetAt_)*kTotalBytesPerMs);budgetAt_=now;
        for(auto& [id,p]:peers_){(void)id;
            p.budget=std::min(kPeerBurst,p.budget+double(now-p.budgetAt)*kPeerBytesPerMs);p.budgetAt=now;
        }
        // One chunk per peer in rotation. Keep requests queued while the rate
        // budget or Steam is busy, instead of imposing a half-second retry.
        for(size_t work=0;work<kMaxPeers*kRequestWindow&&!peers_.empty();++work){
            bool sent=false;auto i=peers_.upper_bound(sendCursor_);
            for(size_t checked=0;checked<peers_.size();++checked,++i){
                if(i==peers_.end())i=peers_.begin();
                auto& p=i->second;
                while(!p.uploads.empty()&&!Upload(p.uploads.front()))p.uploads.pop_front();
                if(p.uploads.empty())continue;
                auto h=p.uploads.front();auto v=Upload(h);
                const size_t count=std::min<size_t>(kChunk,v->bytes.size()-h.offset);
                if(p.budget<count||budget_<count)continue;
                h.kind=Kind::Data;
                if(!Send(i->first,h,std::span(v->bytes).subspan(h.offset,count)))continue;
                p.uploads.pop_front();p.budget-=count;budget_-=count;sentBytes+=count;
                sendCursor_=i->first;sent=true;break;
            }
            if(!sent)break;
        }
    }
public:
    std::function<bool(uint64_t,std::span<const uint8_t>)> send;
    std::function<void(uint64_t)> close;
    std::function<bool(std::shared_ptr<Image>)> deliver;
    uint64_t sentBytes{},receivedBytes{},rejected{},completed{};
    uint64_t sentPackets{},receivedPackets{},sendFailures{};
    uint64_t chunkRetries{},lastTransferMs{};uint32_t lastTransferBytes{};
    Exchange(uint64_t local,uint64_t nonce):local_(local),nonce_(nonce){}
    size_t PeerCount() const {return peers_.size();}
    size_t PublishedCount() const {return owned_.size();}
    size_t OfferedCount() const {return offers_.size();}
    size_t WindowedPeerCount() const {return std::count_if(peers_.begin(),peers_.end(),[](const auto& v){return v.second.window>1;});}
    size_t PendingUploads() const {size_t n=0;for(const auto& [id,p]:peers_){(void)id;n+=p.uploads.size();}return n;}
    uint32_t DownloadBytes() const {return download_.receivedBytes;}
    uint32_t DownloadTotal() const {return download_.started?download_.offer.h.total:0;}
    void Context(const std::vector<PortalInstance>& p) {
        portals_=p;if(portals_.size()>kMaxPortals)portals_.resize(kMaxPortals);
        std::erase_if(owned_,[&](auto& v){return !Active(v.first);});
        std::erase_if(offers_,[&](auto& v){return !Active(v.first);});
        std::erase_if(received_,[&](auto& v){return !Active(v.first);});
        if(!Active(download_.key))download_={};
        if(ready_&&!Active(ready_->key))ready_.reset();
        TrimUploads();
    }
    bool Authorize(uint64_t account,uint64_t nonce,uint64_t now) {
        if(!account||account==local_||!nonce)return false;
        auto i=peers_.find(account);
        if(i==peers_.end()&&peers_.size()>=kMaxPeers)return false;
        auto& p=peers_[account];
        const bool changed=p.nonce!=nonce;
        if(p.nonce!=nonce){
            if(p.nonce&&close)close(account);
            std::erase_if(offers_,[&](auto& v){return v.first.owner==account;});
            std::erase_if(received_,[&](auto& v){return v.first.owner==account;});
            if(download_.key.owner==account)download_={};
            if(ready_&&ready_->key.owner==account)ready_.reset();
            p={};p.nonce=nonce;
        }
        p.seen=now;
        return changed;
    }
    void Publish(std::shared_ptr<const Encoded> value) {
        if(!value||value->key.owner!=local_||!Active(value->key)||!ValidMetadata(value->meta)||
           value->bytes.empty()||value->bytes.size()>kMaxEncoded)return;
        owned_[value->key]=std::move(value);
        for(auto& [id,p]:peers_){(void)id;p.advertised=0;}
        TrimUploads();
    }
    void Receive(uint64_t account,std::span<const uint8_t> packet,uint64_t now) {
        ++receivedPackets;
        auto reject=[&]{++rejected;};
        auto i=peers_.find(account);Header h{};
        if(i==peers_.end()||packet.size()<sizeof(h)||packet.size()>sizeof(h)+kChunk){reject();return;}
        std::memcpy(&h,packet.data(),sizeof(h));
        if(h.magic!=kMagic||h.version!=1||h.from!=i->second.nonce||h.to!=nonce_||now-i->second.seen>20000){reject();return;}
        auto payload=packet.subspan(sizeof(h));
        if(h.kind==Kind::Hello){
            if(!payload.empty()){reject();return;}
            i->second.window=(h.total&kWindowCapability)&&h.offset>0&&h.offset<=kRequestWindow?h.offset:1;
            return;
        }
        Key key{h.kind==Kind::Want?local_:account,h.portal,h.side};
        if(!Active(key)||!h.revision){reject();return;}
        if(h.kind==Kind::Manifest){
            Metadata m{};
            if(payload.size()!=sizeof(m)||!h.total||h.total>kMaxEncoded||h.offset){reject();return;}
            std::memcpy(&m,payload.data(),sizeof(m));
            if(!ValidMetadata(m)||(m.codec==0&&h.total!=kRawBytes)){reject();return;}
            if(!offers_.contains(key)&&offers_.size()>=kMaxViews){reject();return;}
            auto old=offers_.find(key);
            if(old!=offers_.end()&&old->second.h.revision>h.revision)return;
            if(old!=offers_.end()&&old->second.h.revision==h.revision&&
               (old->second.h.total!=h.total||old->second.h.crc!=h.crc||std::memcmp(&old->second.meta,&m,sizeof(m)))){reject();return;}
            if(download_.key==key&&download_.offer.h.revision!=h.revision)download_={};
            if(ready_&&ready_->key==key&&ready_->revision!=h.revision)ready_.reset();
            offers_[key]={h,m,now};return;
        }
        if(h.kind==Kind::Want){
            auto own=owned_.find(key);
            if(!payload.empty()||own==owned_.end()||own->second->revision!=h.revision){reject();return;}
            const auto& v=*own->second;
            if(h.offset>=v.bytes.size()||h.offset%kChunk||h.total!=v.bytes.size()||h.crc!=v.crc){reject();return;}
            auto& p=i->second;
            for(const auto& q:p.uploads)if(q.portal==h.portal&&q.side==h.side&&q.revision==h.revision&&q.offset==h.offset)return;
            if(p.uploads.size()>=kRequestWindow){reject();return;}
            p.uploads.push_back(h);
            return;
        }
        if(h.kind==Kind::Data){
            auto& d=download_;
            if(d.key!=key||d.offer.h.revision!=h.revision||d.offer.h.total!=h.total||d.offer.h.crc!=h.crc||
               !d.started||h.offset>=h.total||h.offset%kChunk||
               payload.size()!=std::min<size_t>(kChunk,h.total-h.offset)) {reject();return;}
            auto& chunk=d.chunks[h.offset/kChunk];
            if(!chunk.requested){reject();return;}
            if(chunk.received)return; // normal after a retry; never count or append it twice
            std::memcpy(d.bytes.data()+h.offset,payload.data(),payload.size());
            chunk.received=true;d.receivedBytes+=uint32_t(payload.size());receivedBytes+=payload.size();
            const uint32_t chunks=(h.total+kChunk-1)/kChunk;
            while(d.base<chunks&&d.chunks[d.base].received)++d.base;
            if(d.receivedBytes==h.total){
                Encoded e{key,h.revision,d.offer.meta,h.crc,std::move(d.bytes)};
                auto result=std::make_shared<Image>();
                if(Decode(e,*result)){result->epoch=nonce_;ready_=result;lastTransferMs=now-d.started;lastTransferBytes=h.total;}
                else {offers_.erase(key);reject();}
                download_={};
            }
            return;
        }
        reject();
    }
    void Step(uint64_t now) {
        for(auto i=peers_.begin();i!=peers_.end();){
            if(now-i->second.seen>20000){
                auto account=i->first;if(close)close(account);
                std::erase_if(offers_,[&](auto& v){return v.first.owner==account;});
                if(download_.key.owner==account)download_={};i=peers_.erase(i);
                if(ready_&&ready_->key.owner==account)ready_.reset();
            }else ++i;
        }
        for(auto& [id,p]:peers_){
            if(!p.hello||now-p.hello>=5000){Header h{};h.kind=Kind::Hello;
                h.offset=kRequestWindow;h.total=kWindowCapability;Send(id,h);p.hello=now;}
            if(!p.advertised||now-p.advertised>=5000){
                for(auto& [key,e]:owned_){Header h{};h.kind=Kind::Manifest;h.portal=key.portal;h.side=key.side;
                    h.revision=e->revision;h.total=uint32_t(e->bytes.size());h.crc=e->crc;
                    Send(id,h,{reinterpret_cast<const uint8_t*>(&e->meta),sizeof(Metadata)});}
                p.advertised=now;
            }
        }
        SendUploads(now);
        if(ready_){if(deliver&&deliver(ready_)){received_[ready_->key]=ready_->revision;ready_.reset();++completed;}else return;}
        if(download_.started&&now-download_.started>300000){offers_.erase(download_.key);download_={};}
        if(!download_.started){
            for(auto& [key,offer]:offers_)if(received_[key]!=offer.h.revision&&now-offer.seen<15000){
                download_.key=key;download_.offer=offer;download_.started=now;
                download_.bytes.resize(offer.h.total);break;
            }
        }
        if(download_.started){
            auto peer=peers_.find(download_.key.owner);if(peer==peers_.end())return;
            const uint32_t end=std::min((download_.offer.h.total+kChunk-1)/kChunk,download_.base+peer->second.window);
            for(uint32_t n=download_.base;n<end;++n){
                auto& chunk=download_.chunks[n];
                if(chunk.received||(chunk.requested&&now-chunk.requested<1000))continue;
                auto h=download_.offer.h;h.kind=Kind::Want;h.offset=n*kChunk;
                if(Send(download_.key.owner,h)){if(chunk.requested)++chunkRetries;chunk.requested=now;}
            }
        }
    }
    void Close(){for(auto& [id,p]:peers_){(void)p;if(close)close(id);}peers_.clear();}
};
}
