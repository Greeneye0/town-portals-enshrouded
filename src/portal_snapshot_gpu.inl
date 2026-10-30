// Included after InspectImage, inside sceneprobe. Static ABI evidence (1273043):
// C:/temp/enshrouded-re/out/readback/{INDEX.txt,dis-dlRecordImage-1015860.asm,q6-out.txt}
// All native calls run on the existing scene/update/render hooks, never the worker.
constexpr uint32_t kShareRows=64,kShareStripeBytes=kSnapshotWidth*kShareRows*8;
struct SharedView {
    Snapshot snapshot{};sharing::Key key{};
    uint64_t epoch{},revision{};uint32_t ticket{};bool shroud{};
};
struct SharedGpuState {
    void* root{};void* download{};
    SharedView views[sharing::kMaxViews]{};
    uint64_t tag{},inFlight{},downloaded{},uploaded{};uint32_t error{};
};
SharedGpuState* g_sharedGpu{};
struct SnapshotDownload {
    std::shared_ptr<sharing::Image> image;
    int slot=-1;uint32_t row{};uint64_t tick{};
    homecache::Key home{}; // set: the home snapshot cache's readback, not sharing's
};
SnapshotDownload g_download;
// A picture read back with every pixel the same: the GPU kept nothing of it (seen as 295-byte
// shares and swirl-only portals on one player's PC, 2026-10-29). Logged once per kind.
void LogIfBlank(const sharing::Image& image,const char* what) {
    const auto& p=image.pixels;
    if(p.size()<16)return;
    uint64_t first;std::memcpy(&first,p.data(),8);
    for(size_t at=8;at+8<=p.size();at+=8*61){uint64_t v;std::memcpy(&v,p.data()+at,8);if(v!=first)return;}
    static bool logged[2]{};
    const int kind=std::strcmp(what,"home")==0;
    if(logged[kind])return;logged[kind]=true;
    char line[192]{};
    std::snprintf(line,sizeof(line),"Town Portal: the %s picture read back blank (every pixel %016llx); portals will show only their swirl",what,(unsigned long long)first);
    greeneye::Host()->log(line);
}
// The home snapshot cache (portal_snapshot_cache.h): a capture of the player's own altar to
// read back and save, and one loaded from disk to upload. Same native transfers as sharing.
struct HomeTransfer {
    int exportSlot=-1;uint64_t exportTick{};homecache::Key exportKey{};
    std::shared_ptr<sharing::Image> upload;int uploadResult{}; // 0 pending, 1 shown, -1 failed
    const char* uploadWhat="snapshot";
};
HomeTransfer g_homeTransfer;
void HomeExport(int slot,uint64_t tick,const homecache::Key& key) {
    g_homeTransfer.exportSlot=slot;g_homeTransfer.exportTick=tick;g_homeTransfer.exportKey=key;
}
// Gameplay thread, under g_lock: queue an image (or poll the one queued).
int CachedHomeStep(std::shared_ptr<sharing::Image> image,const char* what) {
    auto& h=g_homeTransfer;
    if(image){h.upload=std::move(image);h.uploadResult=0;h.uploadWhat=what?what:"snapshot";}
    return h.uploadResult;
}
std::shared_ptr<sharing::Image> g_sharedIncoming;
uint64_t g_exportedEpoch{},g_exportedPortal{},g_exportedTicks[2]{};

bool ShareNativeMatches() {
    static int match=-1;
    if(match<0){
        auto* host=greeneye::Host();
        match=1;
        for(const auto& entry:sharing::native::entries)
            if(host->find(entry.signature,nullptr)!=g_nativeBase+entry.rva)match=0;
    }return match!=0;
}
sharing::Metadata ShareMetadata(const Snapshot& s,bool shroud){
    sharing::Metadata m{};m.x=s.x;m.y=s.y;m.z=s.z;m.nx=s.nx;m.nz=s.nz;
    m.cx=s.cx;m.cy=s.cy;m.cz=s.cz;m.exposure=s.exposure;m.shroud=shroud;
    std::memcpy(m.clip,s.clip,sizeof(m.clip));return m;
}
Snapshot ShareSnapshot(const sharing::Image& image,ImageView view){
    const auto& m=image.meta;Snapshot s{};s.image=view;
    s.x=m.x;s.y=m.y;s.z=m.z;s.nx=m.nx;s.nz=m.nz;s.cx=m.cx;s.cy=m.cy;s.cz=m.cz;
    s.exposure=m.exposure;s.tick=GetTickCount64();s.valid=true;std::memcpy(s.clip,m.clip,sizeof(s.clip));return s;
}
const Snapshot* SharedSnapshot(const PortalInstance& p,bool destinationBase,bool& shroud) {
    if(!g_sharedGpu||!sharing::Epoch()||g_sharedGpu->error)return nullptr;
    const sharing::Key key{p.ownerAccount,p.portalId,destinationBase?1u:0u};
    for(auto& v:g_sharedGpu->views)if(v.epoch==sharing::Epoch()&&v.key==key&&v.snapshot.valid){
        shroud=v.shroud;return &v.snapshot;
    }
    return nullptr;
}
bool SnapshotIsShroud(const Snapshot* s){
    for(int i=0;i<kSnapshots;++i)if(s==&g_composite->snapshots[i])return g_snapshotShroud[i];
    if(g_sharedGpu)for(const auto& v:g_sharedGpu->views)if(s==&v.snapshot)return v.shroud;
    return false;
}
// Poll only completed GPU entries. A stripe is 1 MiB, so the staging allocation
// fits the game's heap even with several frames in flight; never wait on the GPU.
bool EnsureDownload(uint8_t* root) {
    auto& gpu=*g_sharedGpu;
    if(gpu.download)return true;
    void* context=At<void*>(root,0x128);void* graphics=At<void*>(context,8);void* heap=At<void*>(graphics,0x588);
    const uint32_t frames=At<uint32_t>(graphics,0x440);
    if(!heap||!frames||frames>8||uint64_t(frames)*kShareStripeBytes>At<uint32_t>(heap,0x14)){gpu.error=4;return false;}
    alignas(8) uint8_t desc[24]{};At<void*>(desc,8)=heap;At<uint64_t>(desc,16)=kShareStripeBytes;
    // RootCreate (0xfff90, store at 0xffff3) saves its allocator at root+0.
    gpu.download=Native<void*(*)(void*,const void*)>(0xfc1550)(At<void*>(root,0),desc);
    if(!gpu.download){gpu.error=5;return false;}
    return true;
}
// Whole-image upload into an image made with transfer-destination usage. False (nothing
// written) when the native uploader cannot take a whole snapshot.
bool UploadPixels(uint8_t* root,const ImageView& target,const std::vector<uint8_t>& pixels) {
    // The native uploader cannot service a request larger than its chunk.
    void* uploadGraphics=At<void*>(At<void*>(root,0x128),8);
    void* uploadPool=At<void*>(uploadGraphics,0x5d0);
    if(!uploadPool||At<uint64_t>(uploadPool,0x10)<sharing::kRawBytes)return false;
    alignas(16) uint8_t upload[0x80]{};uint64_t access=0x400;
    Native<void*(*)(void*,void*,const ImageView*,uint8_t,const uint64_t*)>(0x1018150)
        (upload,At<void*>(root,0x128),&target,7,&access);
    // Native whole-image uploads return mapped pointer, byte size, row pitch.
    const size_t rowBytes=kSnapshotWidth*8;const uint64_t pitch=At<uint64_t>(upload,0x10),bytes=At<uint64_t>(upload,8);
    auto* mapped=At<uint8_t*>(upload,0);
    if(!mapped||pitch<rowBytes||pitch>rowBytes+65536||bytes<(kSnapshotHeight-1)*pitch+rowBytes)return false;
    for(uint32_t y=0;y<kSnapshotHeight;++y)std::memcpy(mapped+y*pitch,pixels.data()+y*rowBytes,rowBytes);
    return true;
}
// The home snapshot cache: show a loaded image in a free snapshot slot, and start reading
// back a fresh capture of the altar. Returns whether a readback is now running.
bool HomeTransferTick(uint8_t* root) {
    auto& h=g_homeTransfer;auto& c=*g_composite;
    if(h.upload){
        auto image=std::move(h.upload);h.upload.reset();
        // A free slot, else the one no portal has shown for longest (never one shown in the last
        // two seconds: a portal on screen keeps its picture).
        int slot=-1;
        const uint64_t now=GetTickCount64();
        for(int i=0;i<kSnapshots&&slot<0;++i)if(!c.snapshots[i].valid&&c.snapshots[i].image.context)slot=i;
        if(slot<0){
            uint64_t oldest=UINT64_MAX;
            for(int i=0;i<kSnapshots;++i){
                const uint64_t used=std::max(g_snapshotShown[i],c.snapshots[i].tick);
                if(c.snapshots[i].image.context&&i!=g_download.slot&&now-used>2000&&used<oldest){oldest=used;slot=i;}
            }
        }
        const Attachment info=slot<0?Attachment{}:InspectImage(c.snapshots[slot].image);
        // Images kept from a build before the cache lack transfer-destination usage.
        char line[160]{};
        if(slot<0||!info.valid||info.format!=0x61||!(info.usage&2)||!UploadPixels(root,c.snapshots[slot].image,image->pixels)){
            h.uploadResult=-1;
            std::snprintf(line,sizeof(line),"Town Portal: %s not shown (%s)",h.uploadWhat,slot<0?"every snapshot slot in use":
                !(info.usage&2)?"snapshot images need a fresh game launch":"native upload unavailable");
        } else {
            c.snapshots[slot]=ShareSnapshot(*image,c.snapshots[slot].image);
            g_snapshotShroud[slot]=image->meta.shroud!=0;
            h.uploadResult=1;
            std::snprintf(line,sizeof(line),"Town Portal: %s shown in snapshot slot %d (no capture needed)",h.uploadWhat,slot);
        }
        greeneye::Host()->log(line);
    }
    if(h.exportSlot<0||g_download.image||g_sharedGpu->inFlight)return false;
    const int slot=h.exportSlot;h.exportSlot=-1;
    const auto& s=c.snapshots[slot];
    if(!s.valid||s.tick!=h.exportTick)return false;
    const auto info=InspectImage(s.image);
    if(!info.valid||info.format!=0x61||!(info.usage&1)||!EnsureDownload(root))return false;
    auto image=std::make_shared<sharing::Image>();image->revision=s.tick;image->meta=ShareMetadata(s,g_snapshotShroud[slot]);
    if(!sharing::ValidMetadata(image->meta))return false;
    image->pixels.resize(sharing::kRawBytes);g_download={image,slot,0,s.tick,h.exportKey};
    return true;
}
void SharedGpuTick() {
    if(!g_sharedGpu||!g_world||!g_composite||!g_composite->enabled||g_sharedGpu->error)return;
    const bool sharingOn=sharing::Enabled();
    if(!sharingOn&&!g_download.image&&!g_sharedGpu->inFlight&&g_homeTransfer.exportSlot<0&&!g_homeTransfer.upload)return;
    auto& gpu=*g_sharedGpu;auto* root=reinterpret_cast<uint8_t*>(g_world->root);
    if(!root||!ShareNativeMatches())return;
    if(gpu.root&&gpu.root!=root){gpu.error=1;return;} // old native context must never be reused
    gpu.root=root;
    if(gpu.download){
        uint64_t result[5]{};
        if(Native<uint8_t(*)(void*,void*)>(0xfe6d80)(result,gpu.download)){
            const uint64_t tag=result[3];gpu.inFlight=0;
            if(g_download.image&&tag==gpu.tag&&result[0]&&result[1]>=kShareStripeBytes&&
               (homecache::Valid(g_download.home)||g_download.image->epoch==sharing::Epoch())){
                auto& job=g_download;
                std::memcpy(job.image->pixels.data()+size_t(job.row)*kSnapshotWidth*8,reinterpret_cast<void*>(result[0]),kShareStripeBytes);
                job.row+=kShareRows;
            }
        }
    }
    if(g_download.image&&homecache::Valid(g_download.home)){
        auto& job=g_download;
        if(!g_composite->snapshots[job.slot].valid||g_composite->snapshots[job.slot].tick!=job.tick)g_download={};
        else if(job.row==kSnapshotHeight){
            LogIfBlank(*job.image,"home");
            homecache::Save(job.home,std::move(job.image));
            greeneye::Host()->log("Town Portal: home snapshot read back; saving it for the next login");
            g_download={};
        }
    }
    else if(g_download.image){
        auto& job=g_download;
        if(job.image->epoch!=sharing::Epoch()||!sharing::Current(job.image->key)||
           !g_composite->snapshots[job.slot].valid||g_composite->snapshots[job.slot].tick!=job.tick)g_download={};
        else if(job.row==kSnapshotHeight&&(LogIfBlank(*job.image,"shared"),sharing::Submit(job.image))){
            greeneye::Host()->log("Town Portal sharing: snapshot readback complete; queued lossless encoding");
            ++gpu.downloaded;g_exportedTicks[job.image->key.side]=job.tick;g_download={};
        }
    }
    if(HomeTransferTick(root)||!sharingOn)return;
    if(!sharing::Epoch()){
        g_sharedIncoming.reset();for(auto& v:gpu.views)v.snapshot.valid=false;return;
    }
    for(auto& v:gpu.views)
        if(v.snapshot.valid&&(v.epoch!=sharing::Epoch()||!sharing::Current(v.key)))v.snapshot.valid=false;
    if(!g_sharedIncoming)g_sharedIncoming=sharing::TakeIncoming();
    if(g_sharedIncoming){
        auto& incoming=*g_sharedIncoming;
        if(incoming.epoch!=sharing::Epoch()||!sharing::Current(incoming.key)){g_sharedIncoming.reset();return;}
        SharedView* slot=nullptr;
        for(auto& v:gpu.views)if(v.key==incoming.key&&v.epoch==incoming.epoch){slot=&v;break;}
        if(!slot)for(auto& v:gpu.views)if(!v.snapshot.valid||v.epoch!=incoming.epoch||!sharing::Current(v.key)){slot=&v;break;}
        if(!slot)return;
        if(!slot->ticket){
            alignas(16) uint8_t scope[0x180]{},plan[0x20]{},desc[0x30]{},result[16]{};
            g_scope(scope,At<void*>(root,0x128),0);g_plan(plan,scope,At<uint32_t>(root,0x270),0,1);
            ImageDesc(desc,kSnapshotWidth,kSnapshotHeight,0x61,0x06); // sampled + transfer destination
            g_image(plan,&slot->snapshot.image,desc);g_commit(scope,result,&slot->ticket,At<void*>(root,0x160));g_end(scope);
            if(result[0]||!slot->ticket||!slot->snapshot.image.context){gpu.error=2;return;}
        }
        const ImageView target=slot->snapshot.image;
        if(!UploadPixels(root,target,incoming.pixels)){gpu.error=3;return;}
        slot->snapshot=ShareSnapshot(incoming,target);slot->key=incoming.key;
        slot->epoch=incoming.epoch;slot->revision=incoming.revision;slot->shroud=incoming.meta.shroud!=0;
        ++gpu.uploaded;
        greeneye::Host()->log("Town Portal sharing: received snapshot uploaded and ready for parallax");
        g_sharedIncoming.reset();
    }
    if(g_download.image||gpu.inFlight)return;
    PortalInstance owner{};if(!sharing::OwnPortal(owner))return;
    if(g_exportedEpoch!=sharing::Epoch()||g_exportedPortal!=owner.portalId){
        g_exportedEpoch=sharing::Epoch();g_exportedPortal=owner.portalId;g_exportedTicks[0]=g_exportedTicks[1]=0;
    }
    for(uint32_t side=0;side<2;++side){
        const auto end=side?owner.baseEnd:owner.fieldEnd.Turned();int index=FindSnapshot(end);
        if(index<0)continue;const auto& s=g_composite->snapshots[index];
        if(s.tick==g_exportedTicks[side])continue;
        auto info=InspectImage(s.image);
        if(!info.valid||info.format!=0x61||!(info.usage&1))continue; // old hot-kept images need a fresh launch
        if(!EnsureDownload(root))return;
        auto image=std::make_shared<sharing::Image>();image->key={owner.ownerAccount,owner.portalId,side};
        image->epoch=sharing::Epoch();image->revision=s.tick;image->meta=ShareMetadata(s,g_snapshotShroud[index]);
        if(!sharing::ValidMetadata(image->meta))continue;
        image->pixels.resize(sharing::kRawBytes);g_download={image,index,0,s.tick};break;
    }
}
void SharedGpuRecord(void* scope) {
    if(!g_sharedGpu||g_sharedGpu->error||g_sharedGpu->inFlight||!g_download.image||g_download.row>=kSnapshotHeight)return;
    auto& job=g_download;auto& gpu=*g_sharedGpu;
    if((!homecache::Valid(job.home)&&job.image->epoch!=sharing::Epoch())||g_composite->snapshots[job.slot].tick!=job.tick)return;
    // 40-byte image subresource/region, embedded at +16 in native BufferImageCopy.
    sharing::native::Region region{};uint8_t status[8]{};
    region.y=job.row;region.width=kSnapshotWidth;region.height=kShareRows;
    uint64_t cookie[2]={++gpu.tag,0};
    Native<void*(*)(void*,void*,void*,const ImageView*,const void*,const void*)>(0x1015860)
        (scope,status,gpu.download,&g_composite->snapshots[job.slot].image,&region,cookie);
    if(status[0]==0)gpu.inFlight=gpu.tag;
}
void SharedGpuTickGuard(){
    __try {SharedGpuTick();}
    __except(EXCEPTION_EXECUTE_HANDLER){if(g_sharedGpu)g_sharedGpu->error=0x80000001;}
    static uint32_t reported=0;
    if(g_sharedGpu&&g_sharedGpu->error&&g_sharedGpu->error!=reported){
        reported=g_sharedGpu->error;char line[128]{};
        std::snprintf(line,sizeof(line),"Town Portal sharing: GPU transfer disabled (error %u)",reported);greeneye::Host()->log(line);
    }
}
void SharedGpuRecordGuard(void* scope){
    __try {SharedGpuRecord(scope);}
    __except(EXCEPTION_EXECUTE_HANDLER){if(g_sharedGpu)g_sharedGpu->error=0x80000002;}
}
void SharedGpuStatus(char* out,uint32_t capacity){
    std::lock_guard lock(g_lock);
    if(!g_sharedGpu)return;
    size_t n=strnlen_s(out,capacity);if(!n||n>=capacity||out[n-1]!='}')return;
    uint32_t ready=0;for(auto& v:g_sharedGpu->views)if(v.snapshot.valid&&v.epoch==sharing::Epoch())++ready;
    std::snprintf(out+n-1,capacity-n+1,",\"snapshotGpuTransfer\":{\"error\":%u,\"inFlight\":%llu,\"readbackRow\":%u,\"downloaded\":%llu,\"uploaded\":%llu,\"remoteViews\":%u}}",
        g_sharedGpu->error,g_sharedGpu->inFlight,g_download.row,g_sharedGpu->downloaded,g_sharedGpu->uploaded,ready);
}
