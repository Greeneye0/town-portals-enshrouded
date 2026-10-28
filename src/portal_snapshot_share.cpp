#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib,"bcrypt.lib")
#include "portal_snapshot_share.h"
#include "portal_snapshot_identity.h"
#include "greeneye.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>

namespace portal::sharing {
namespace {
// Minimal x64 flat ABI, verified against Valve's steamnetworkingtypes.h.
// https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h
struct alignas(8) Identity {uint8_t opaque[136]{};};
struct MessagePrefix {void* data;int size;uint32_t connection;Identity peer;};
static_assert(offsetof(MessagePrefix,peer)==16);
struct Steam {
    void* api{};
    void (*setId)(Identity*,uint64_t){};uint64_t (*getId)(const Identity*){};
    int (*send)(void*,const Identity*,const void*,uint32_t,int,int){};
    int (*receive)(void*,int,MessagePrefix**,int){};
    bool (*close)(void*,const Identity*,int){};
    void (*release)(MessagePrefix*){};
    template<class F>static F Resolve(HMODULE m,const char* n){return reinterpret_cast<F>(GetProcAddress(m,n));}
    bool Bind(){
        auto m=GetModuleHandleW(L"steam_api64.dll");if(!m)return false;
        auto get=Resolve<void*(*)()>(m,"SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
        setId=Resolve<decltype(setId)>(m,"SteamAPI_SteamNetworkingIdentity_SetSteamID64");
        getId=Resolve<decltype(getId)>(m,"SteamAPI_SteamNetworkingIdentity_GetSteamID64");
        send=Resolve<decltype(send)>(m,"SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
        receive=Resolve<decltype(receive)>(m,"SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
        close=Resolve<decltype(close)>(m,"SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser");
        release=Resolve<decltype(release)>(m,"SteamAPI_SteamNetworkingMessage_t_Release");
        if(!get||!setId||!getId||!send||!receive||!close||!release)return false;
        api=get();return api!=nullptr;
    }
    int Send(uint64_t to,std::span<const uint8_t> data){Identity id{};setId(&id,to);return send(api,&id,data.data(),uint32_t(data.size()),8|32,kChannel);}
    void Close(uint64_t to){Identity id{};setId(&id,to);close(api,&id,kChannel);}
};
struct Auth {uint64_t account,nonce,at;};
std::mutex g_mutex;std::condition_variable g_wake;
// Deleted after joining in Stop/HotPrepare. Do not run std::thread's terminating
// destructor under Windows' process-detach loader lock during game shutdown.
std::thread* g_worker{};bool g_stop{};
std::atomic<uint64_t> g_epoch{},g_sent{},g_received{},g_completed{},g_rejected{};
std::atomic<uint64_t> g_sentPackets{},g_receivedPackets{},g_sendFailures{},g_controls{},g_badControls{},g_encodeFailures{};
std::atomic<uint64_t> g_legacyControls{};
std::atomic<uint32_t> g_peers{},g_published{},g_offered{},g_downloadBytes{},g_downloadTotal{};
std::atomic<uint32_t> g_windowPeers{},g_queuedChunks{},g_lastTransferBytes{};
std::atomic<uint64_t> g_chunkRetries{},g_lastTransferMs{};
std::atomic<int> g_sendResult{};
std::atomic<bool> g_available{};
void* g_world{};uint64_t g_account{},g_advertised{};
std::vector<PortalInstance> g_portals;
std::deque<Auth> g_auth;
std::deque<std::shared_ptr<Image>> g_outgoing,g_incoming;
bool Flag(const wchar_t* name){
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(auto* slash=wcsrchr(path,L'\\'))slash[1]=0;
    wcscat_s(path,L"greeneye\\town-portal\\");wcscat_s(path,name);
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}
void Work(uint64_t account,uint64_t epoch){
    Steam steam;
    if(!steam.Bind()){greeneye::Host()->log("Town Portal: Steam snapshot sharing unavailable");return;}
    g_available=true;
    Exchange exchange(account,epoch);
    uint64_t lastSendError{};
    exchange.send=[&](uint64_t id,std::span<const uint8_t> p){
        const int result=steam.Send(id,p);g_sendResult=result;
        const auto now=GetTickCount64();
        if(result!=1&&(!lastSendError||now-lastSendError>=10000)){
            char line[160]{};std::snprintf(line,sizeof(line),"Town Portal sharing: Steam send failed (result %d, peer %llu)",result,id);
            greeneye::Host()->log(line);lastSendError=now;
        }
        return result==1;
    };
    exchange.close=[&](uint64_t id){steam.Close(id);};
    exchange.deliver=[&](std::shared_ptr<Image> image){
        std::lock_guard lock(g_mutex);
        if(g_stop||g_epoch!=epoch)return false;
        if(g_incoming.size()>=2)return false;
        g_incoming.push_back(std::move(image));return true;
    };
    try {
        for(;;){
            std::deque<Auth> auth;std::vector<PortalInstance> portals;std::shared_ptr<Image> outgoing;
            {
                std::unique_lock lock(g_mutex);
                if(g_stop)break;
                g_wake.wait_for(lock,std::chrono::milliseconds(20));if(g_stop)break;
                auth.swap(g_auth);portals=g_portals;
                if(!g_outgoing.empty()){outgoing=std::move(g_outgoing.front());g_outgoing.pop_front();}
            }
            exchange.Context(portals);
            for(const auto& a:auth)if(exchange.Authorize(a.account,a.nonce,a.at)){
                char line[128]{};std::snprintf(line,sizeof(line),"Town Portal sharing: verified Steam peer %llu; exchanging cached portal images",a.account);
                greeneye::Host()->log(line);
            }
            if(outgoing&&outgoing->epoch==epoch){
                auto encoded=std::make_shared<Encoded>();
                if(Encode(*outgoing,*encoded)){
                    exchange.Publish(encoded);
                    char line[160]{};std::snprintf(line,sizeof(line),"Town Portal sharing: published portal %llu side %u (%zu bytes)",encoded->key.portal,encoded->key.side,encoded->bytes.size());
                    greeneye::Host()->log(line);
#ifdef TOWN_PORTAL_DEV
                    // Explicit solo GPU round-trip test: readback -> codec ->
                    // fresh native upload -> the normal parallax renderer.
                    // Never impersonate a Steam peer or synthesize game traffic.
                    if(Flag(L"share-snapshots-loopback.enabled")){
                        auto loopback=std::make_shared<Image>();
                        if(Decode(*encoded,*loopback)){loopback->epoch=epoch;exchange.deliver(loopback);}
                    }
#endif
                }else {++g_encodeFailures;greeneye::Host()->log("Town Portal sharing: snapshot encoding failed");}
            }
            MessagePrefix* messages[8]{};int count=steam.receive(steam.api,kChannel,messages,8);
            for(int i=0;i<count&&i<8;++i){auto* m=messages[i];
                try{if(m&&m->data&&m->size>0&&m->size<=int(sizeof(Header)+kChunk))
                    exchange.Receive(steam.getId(&m->peer),{static_cast<const uint8_t*>(m->data),size_t(m->size)},GetTickCount64());}
                catch(...){if(m)steam.release(m);for(int j=i+1;j<count&&j<8;++j)if(messages[j])steam.release(messages[j]);throw;}
                if(m)steam.release(m);
            }
            exchange.Step(GetTickCount64());
            if(exchange.completed!=g_completed.load()){
                char line[192]{};std::snprintf(line,sizeof(line),"Town Portal sharing: downloaded %u bytes in %llu ms (%llu completed; %zu windowed peers)",
                    exchange.lastTransferBytes,exchange.lastTransferMs,exchange.completed,exchange.WindowedPeerCount());
                greeneye::Host()->log(line);
            }
            g_sent=exchange.sentBytes;g_received=exchange.receivedBytes;g_completed=exchange.completed;g_rejected=exchange.rejected;
            g_sentPackets=exchange.sentPackets;g_receivedPackets=exchange.receivedPackets;g_sendFailures=exchange.sendFailures;
            g_peers=uint32_t(exchange.PeerCount());g_published=uint32_t(exchange.PublishedCount());g_offered=uint32_t(exchange.OfferedCount());
            g_downloadBytes=exchange.DownloadBytes();g_downloadTotal=exchange.DownloadTotal();
            g_windowPeers=uint32_t(exchange.WindowedPeerCount());g_queuedChunks=uint32_t(exchange.PendingUploads());
            g_chunkRetries=exchange.chunkRetries;g_lastTransferMs=exchange.lastTransferMs;g_lastTransferBytes=exchange.lastTransferBytes;
        }
    }catch(...){greeneye::Host()->log("Town Portal: snapshot sharing stopped after a transfer error");}
    exchange.Close();g_available=false;
}
}
bool Enabled(){
    static const bool enabled=Flag(L"share-snapshots.enabled");return enabled;
}
uint64_t Epoch(){return g_epoch.load();}
void Stop(){
    {std::lock_guard lock(g_mutex);g_stop=true;g_epoch=0;}g_wake.notify_all();
    if(g_worker){if(g_worker->joinable())g_worker->join();delete g_worker;g_worker=nullptr;}
    std::lock_guard lock(g_mutex);g_world=nullptr;g_account=g_advertised=0;
    g_portals.clear();g_auth.clear();g_outgoing.clear();g_incoming.clear();g_available=false;
    g_sent=g_received=g_completed=g_rejected=0;
    g_sentPackets=g_receivedPackets=g_sendFailures=g_controls=g_badControls=g_encodeFailures=g_legacyControls=0;
    g_peers=g_published=g_offered=g_downloadBytes=g_downloadTotal=0;g_sendResult=0;
    g_windowPeers=g_queuedChunks=g_lastTransferBytes=0;g_chunkRetries=g_lastTransferMs=0;
}
void Tick(void* world,void* client,uint64_t account,const std::vector<PortalInstance>& portals,Broadcast broadcast){
    if(!Enabled())return;
    if(world!=g_world||account!=g_account){
        Stop();if(!world||!SteamPlayer(account))return;
        uint64_t nonce{};
        if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&nonce),sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0||!nonce)return;
        {std::lock_guard lock(g_mutex);g_world=world;g_account=account;g_epoch=nonce;g_stop=false;g_portals=portals;}
        g_worker=new std::thread(Work,account,nonce);
    }
    if(!world)return;
    {std::lock_guard lock(g_mutex);g_portals=portals;}
    auto now=GetTickCount64();
    if(g_available&&(!g_advertised||now-g_advertised>=5000)){
        char line[64]{};FormatAnnouncement(line,sizeof(line),account,Epoch());
        broadcast(client,line);g_advertised=now;
    }
}
void Control(uint64_t sender,std::string_view line){
    if(!Epoch()||!sender)return;
    if(line.starts_with("/tpi1 ")){
        if(g_legacyControls.fetch_add(1)==0)greeneye::Host()->log("Town Portal sharing: older snapshot discovery received; both players need r2 or newer");
        return;
    }
    Announcement announcement{};
    if(!ParseAnnouncement(sender,line,announcement)){
        if(g_badControls.fetch_add(1)==0)greeneye::Host()->log("Town Portal sharing: rejected discovery with invalid game/Steam identity");
        return;
    }
    std::lock_guard lock(g_mutex);
    if(!g_epoch||g_stop||announcement.steam==g_account)return;
    ++g_controls;
    const Auth auth{announcement.steam,announcement.nonce,GetTickCount64()};
    for(auto& pending:g_auth)if(pending.account==auth.account){pending=auth;return;}
    if(g_auth.size()<32)g_auth.push_back(auth);
}
bool OwnPortal(PortalInstance& out){
    std::lock_guard lock(g_mutex);if(!g_epoch||g_stop)return false;
    for(const auto& p:g_portals)if(p.active&&!p.closing&&p.ownerAccount==g_account){out=p;return true;}return false;
}
bool Current(const Key& key){
    std::lock_guard lock(g_mutex);if(!g_epoch||g_stop)return false;
    for(const auto& p:g_portals)if(p.active&&!p.closing&&p.portalId==key.portal&&p.ownerAccount==key.owner)return true;return false;
}
bool Submit(std::shared_ptr<Image> image){
    std::lock_guard lock(g_mutex);if(!image||image->epoch!=g_epoch||g_stop||g_outgoing.size()>=2)return false;
    g_outgoing.push_back(std::move(image));g_wake.notify_all();return true;
}
std::shared_ptr<Image> TakeIncoming(){
    std::lock_guard lock(g_mutex);if(g_incoming.empty())return {};
    auto image=std::move(g_incoming.front());g_incoming.pop_front();return image;
}
void AppendStatus(char* out,uint32_t capacity){
    if(!out||!capacity)return;size_t n=strnlen_s(out,capacity);if(!n||n>=capacity||out[n-1]!='}')return;
    std::snprintf(out+n-1,capacity-n+1,",\"snapshotSharing\":{\"revision\":3,\"enabled\":%s,\"steam\":%s,\"peers\":%u,\"controls\":%llu,\"invalidControls\":%llu,\"legacyControls\":%llu,\"published\":%u,\"offers\":%u,\"sentPackets\":%llu,\"receivedPackets\":%llu,\"sendFailures\":%llu,\"lastSendResult\":%d,\"encodeFailures\":%llu,\"sentBytes\":%llu,\"receivedBytes\":%llu,\"downloadBytes\":%u,\"downloadTotal\":%u,\"completed\":%llu,\"rejected\":%llu,\"windowedPeers\":%u,\"queuedChunks\":%u,\"chunkRetries\":%llu,\"lastTransferMs\":%llu,\"lastTransferBytes\":%u}}",
        Enabled()?"true":"false",g_available?"true":"false",g_peers.load(),g_controls.load(),g_badControls.load(),g_legacyControls.load(),
        g_published.load(),g_offered.load(),g_sentPackets.load(),g_receivedPackets.load(),g_sendFailures.load(),g_sendResult.load(),g_encodeFailures.load(),
        g_sent.load(),g_received.load(),g_downloadBytes.load(),g_downloadTotal.load(),g_completed.load(),g_rejected.load(),
        g_windowPeers.load(),g_queuedChunks.load(),g_chunkRetries.load(),g_lastTransferMs.load(),g_lastTransferBytes.load());
}
}
