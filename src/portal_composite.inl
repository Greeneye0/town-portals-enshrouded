// Included inside sceneprobe's private namespace. Native interfaces verified
// against build 1273043 in tools/composite-*-evidence.txt.
#include "portal_composite_shaders.h"
#include "portal_composite_signatures.h"
constexpr int kCompositeSlots=4;
// position 2, plane 4, settings 2, matrix 16, colour 4, frame 4, snapshot rows 12, snapshot info 4,
// cast 4
constexpr int kVertexFloats=52;
// The spell effect from the caster's hand while their portal forms (SetCast).
struct CastEffectState {uint64_t portalId{};float hand[3]{};int effect{};};
CastEffectState g_cast{};
// Snapshots: what stands in front of a portal endpoint, seen from a camera a little way out
// from it, taken while a loading screen hides the frame (see RequestSnapshot). The portal
// that leads there shows it with parallax (portal_composite.frag Window). Colour in RGB, and
// in alpha each pixel's depth along the snapshot camera's view (metres). The camera's own
// projection is kept, so a snapshot also serves a portal standing a little way from where it
// was taken (each new base portal by the same altar gets its own spot): it is re-aimed there.
constexpr int kSnapshots=4;
constexpr uint32_t kSnapshotWidth=2048,kSnapshotHeight=1024;
struct Snapshot {
    ImageView image{};
    float x{},y{},z{},nx{},nz{};   // the endpoint it was taken for: ground centre, outward normal
    float cx{},cy{},cz{};          // the camera, which looked along that normal
    double clip[12]{};             // world to the snapshot's clip x, y (NDC after /w) and w
    float exposure{};              // the frame's exposure
    uint64_t tick{};
    bool valid{};
};
// Snapshots taken in the Shroud. The game colours the Shroud dark blue after the point a
// snapshot is taken from, so these get a similar grade when shown (not kept across reloads).
bool g_snapshotShroud[kSnapshots]{};
const Snapshot* SharedSnapshot(const PortalInstance& p,bool destinationBase,bool& shroud);
// An altar-side portal drawn with no snapshot of its field end (MissingSnapshot).
struct MissingView {uint64_t portalId{},tick{};PortalEndpoint destination{};};
MissingView g_missing{};
// When each snapshot slot was last shown by a portal (an upload replaces the one unused longest).
uint64_t g_snapshotShown[kSnapshots]{};
bool SnapshotIsShroud(const Snapshot* s);
// Where a portal's window starts: this far in front of the endpoint (the shader traces from
// 0.3 m past it). Nearer than the snapshot camera stood there is nothing to see anyway.
constexpr float kWindowStart=0.5f;
// The window space of an endpoint (metres: its right, up and normal, from kWindowStart in
// front of its centre) to a snapshot's clip x, y, w: rows for portal_composite.frag.
void SnapshotRows(const Snapshot& s,const PortalEndpoint& end,float* rows) {
    const double basis[4][4]={{end.rx,0,end.rz,0},{0,1,0,0},{end.nx,0,end.nz,0},
        {end.wx+end.nx*kWindowStart,end.wy+1.62f,end.wz+end.nz*kWindowStart,1}};
    for(int row=0;row<3;++row)for(int col=0;col<4;++col){
        double v=0;for(int k=0;k<4;++k)v+=s.clip[row*4+k]*basis[col][k];
        rows[row*4+col]=float(v);
    }
}
struct CompositeState {
    ImageView image{};
    DisplayState display{};
    void* graphics{};void* layout{};void* bindings{};void* pipeline{};void* blendPipeline{};void* vertices{};
    // Blends into the game's scene colour (RGBA16F) before anti-aliasing; see InFrameDraw.
    void* framePipeline{};
    // The portal's light as a multiply of the scene colour: scene * (1 + light).
    void* lightPipeline{};
    uint64_t inFrameFrames{},inFrameTick{};uint32_t inFrameSkip{};
    // Snapshots have their own allocation, so a failure there leaves the portals as they were.
    void* capturePipeline{};
    Snapshot snapshots[kSnapshots]{};
    uint32_t snapshotTicket{},snapshotError{},captures{};
    // Fingerprint of the shaders the pipelines were built from: a hot reload with changed
    // shaders rebuilds them (CompositeRefreshPipelines).
    uint32_t shaderHash{};
    uint64_t frames{},tick{},portal{};
    uint32_t error{},stage{};
    bool field{},enabled{};
    // Endpoints drawn by the most recent pass; the HUD shows the image only for these.
    uint64_t renderedPortal[kCompositeSlots]{};
    bool renderedField[kCompositeSlots]{};
    uint32_t renderedCount{};
};
CompositeState* g_composite{};
// Separate kept value: changing the capture filter invalidates baked pixels
// without changing CompositeState's layout or freeing its in-flight GPU images.
uint32_t* g_snapshotCaptureHash{};
// Colour scale for portals drawn into the game's HDR scene colour (see InFrameDraw).
// Tuned by eye at night, dawn and day against the overlay renderer (2026-10-27).
constexpr float kInFrameBrightness=0.6f,kInFrameLight=3.0f,kInFrameContrast=2.0f,kInFrameSaturation=1.7f;
float g_inFrameBrightness=kInFrameBrightness,g_inFrameLight=kInFrameLight,g_inFrameContrast=kInFrameContrast,g_inFrameSaturation=kInFrameSaturation;
constexpr float kGroundLightGain=2.0f;
float g_groundLightGain=kGroundLightGain;
// The GPU compositor is on unless greeneye\town-portal\gpu-composite.disabled exists in the
// game folder; then every portal uses the tile renderer.
bool CompositeAllowed() {
    static int allowed=-1;
    if(allowed<0){
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr,path,MAX_PATH);
        if(auto* slash=wcsrchr(path,L'\\'))slash[1]=0;
        wcscat_s(path,L"greeneye\\town-portal\\gpu-composite.disabled");
        allowed=GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES;
    }
    return allowed!=0;
}
// Endpoints the HUD renderer asked for this frame (nearest first). Normal
// portals use these; the live-view probe keeps its own g_livePortal.
struct CompositeSlot {PortalInstance portal{};bool field{};uint32_t tint=colors::kUnknown;};
CompositeSlot g_compositeSlots[kCompositeSlots]{};
uint32_t g_compositeSlotCount{};
uint64_t g_compositeSlotTick{};
uint8_t* g_nativeBase{};
template<class F> F Native(size_t rva){return reinterpret_cast<F>(g_nativeBase+rva);}
template<class F> F DeviceMethod(void* device,size_t offset){return At<F>(At<void*>(device,0),offset);}

void CompositeReleaseObject(void*& object) {
    if(!object||!g_composite->graphics)return;
    auto* graphics=static_cast<uint8_t*>(g_composite->graphics);
    auto* lock=reinterpret_cast<CRITICAL_SECTION*>(graphics+0x710);
    EnterCriticalSection(lock);
    if(--At<uint16_t>(object,0)==0){
        auto* pending=At<uint8_t*>(graphics,0x580);
        Native<void(*)(void*,void**)>(0xfe9510)(pending?pending+0x20:graphics+0x510,&object);
    }
    LeaveCriticalSection(lock);object=nullptr;
}
void CompositeRelease() {
    if(!g_composite)return;
    if(g_composite->display.image)g_unregisterImage(g_composite->display.registry,g_composite->display.image);
    g_composite->display={};
    CompositeReleaseObject(g_composite->pipeline);
    CompositeReleaseObject(g_composite->blendPipeline);
    CompositeReleaseObject(g_composite->framePipeline);
    CompositeReleaseObject(g_composite->lightPipeline);
    CompositeReleaseObject(g_composite->capturePipeline);
    if(g_composite->snapshotTicket&&g_target&&g_target->root)
        g_release(At<void*>(reinterpret_cast<void*>(g_target->root),0x160),&g_composite->snapshotTicket);
    CompositeReleaseObject(g_composite->layout);
    if(g_composite->bindings&&g_composite->graphics)
        Native<void(*)(void*,void**)>(0xfcf2c0)(g_composite->graphics,&g_composite->bindings);
    *g_composite={};
}
bool CompositePipelines(void* device);
bool CompositeCreate() {
    auto& c=*g_composite;
    c.enabled=CompositeAllowed();
    if(!c.enabled)return true;
    if(!CompositeNativeMatches(g_nativeBase)){c.error=9;c.enabled=false;return false;}
    if(!c.image.context){c.error=10;c.enabled=false;return false;}
    c.stage=1;
    c.graphics=At<void*>(reinterpret_cast<void*>(g_target->color.context),8);
    auto* device=At<void*>(c.graphics,0x278);
    // Three ordinary bindings: destination image, primary scene depth, sampler.
    alignas(8) uint8_t entries[3][16]{};
    for(int i=0;i<3;++i){entries[i][1]=i==2?1:3;At<uint16_t>(entries[i],2)=2;}
    NativeSpan span{entries,3};
    c.bindings=Native<void*(*)(void*,const NativeSpan*)>(0xfbc3d0)(c.graphics,&span);
    if(!c.bindings){c.error=1;return false;}
    c.stage=2;
    alignas(8) uint8_t layout[0x30]{};
    At<void*>(layout,0)=c.bindings;At<uint32_t>(layout,0x20)=1;
    c.layout=DeviceMethod<void*(*)(void*,const void*)>(device,0x38)(device,layout);
    if(!c.layout){c.error=2;return false;}
    // Native attribute types: float2=1, float4=3; locations 0,6,7, the four
    // rows of the screen-to-portal matrix at 8-11, the owner's colour at 12 and at 13 the
    // frame: anti-aliasing jitter x, y and 1 when drawn into the game's scene colour. Then at
    // 1-4 the snapshot's rows and info, at 5 the cast effect (hand in portal metres, effect).
    const uint8_t attributes[]={0,1,6,3,7,1,8,3,9,3,10,3,11,3,12,3,13,3,1,3,2,3,3,3,4,3,5,3};span={attributes,14};
    c.vertices=Native<void*(*)(void*,const NativeSpan*)>(0xfcbed0)(c.graphics,&span);
    if(!c.vertices||At<uint8_t>(c.vertices,0x22)!=kVertexFloats*4){c.error=3;return false;}
    c.stage=3;
    if(!CompositePipelines(device))return false;
    c.stage=4;return true;
}
uint32_t ShaderHash() {
    uint32_t h=2166136261u;
    auto add=[&](const uint32_t* words,size_t count){for(size_t i=0;i<count;++i){h^=words[i];h*=16777619u;}};
    add(kComposite_vert,std::size(kComposite_vert));add(kComposite_frag,std::size(kComposite_frag));
    add(kSnapshot_frag,std::size(kSnapshot_frag));
    return h;
}
void RefreshSnapshotFormat() {
    if(!g_snapshotCaptureHash)return;
    uint32_t hash=2166136261u;
    for(uint32_t word:kSnapshot_frag){hash^=word;hash*=16777619u;}
    if(*g_snapshotCaptureHash==hash)return;
    bool hadImages=false;
    for(auto& snapshot:g_composite->snapshots){hadImages|=snapshot.valid;snapshot.valid=false;}
    *g_snapshotCaptureHash=hash;
    if(hadImages)greeneye::Host()->log("Town Portal: capture filter changed; old snapshots discarded, fresh views will be taken on travel");
}
// Every pipeline, from the current shaders (graphics, layout and vertex format already made).
bool CompositePipelines(void* device) {
    auto& c=*g_composite;
    alignas(8) uint8_t desc[0x118]{};
    At<const void*>(desc,8)=kComposite_vert;At<uint64_t>(desc,0x10)=sizeof(kComposite_vert);
    At<const void*>(desc,0x48)=kComposite_frag;At<uint64_t>(desc,0x50)=sizeof(kComposite_frag);
    At<void*>(desc,0x58)=c.layout;At<void*>(desc,0x60)=c.vertices;
    At<uint32_t>(desc,0x68)=0x01000001;desc[0x6d]=0x1f;
    for(int i=0;i<7;++i)desc[0x79+i*8]=0xf;
    desc[0x78]=0x25;
    At<uint32_t>(desc,0xb0)=0xff00;At<uint32_t>(desc,0xb4)=7;
    At<uint32_t>(desc,0xb8)=0x070000ff;At<uint32_t>(desc,0xbc)=0x01000000;
    At<uint32_t>(desc,0xc0)=0x00070000;
    const char* names[]={"main","tcs_main","tes_main","ms_main","main"};
    for(int i=0;i<5;++i){At<const char*>(desc,0xc8+i*16)=names[i];At<uint64_t>(desc,0xd0+i*16)=std::strlen(names[i]);}
    c.pipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    if(!c.pipeline){c.error=4;return false;}
    // Snapshot capture: the same full-screen triangle writing RGBA16F, no blending.
    {
        alignas(8) uint8_t capture[sizeof(desc)];std::memcpy(capture,desc,sizeof(desc));
        At<const void*>(capture,0x48)=kSnapshot_frag;At<uint64_t>(capture,0x50)=sizeof(kSnapshot_frag);
        capture[0x78]=0x61;
        c.capturePipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,capture);
    }
    // Same pipeline with premultiplied alpha-over blending for every endpoint
    // after the first (evidence: C:\temp\enshrouded-re\out\pipeline-blend).
    // Render-target entry bytes 2-7: color op, src, dst, alpha op, src, dst;
    // op 1 = add, factor 1 = one, 5 = one minus source alpha.
    const uint8_t blend[]={1,1,5,1,1,5};std::memcpy(desc+0x7a,blend,sizeof(blend));
    c.blendPipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    if(!c.blendPipeline){c.error=5;return false;}
    // The same blend into the game's scene colour, R16G16B16A16_SFLOAT (engine format
    // 0x61 = the VkFormat; evidence: C:\temp\enshrouded-re\out\pre-taa).
    desc[0x78]=0x61;
    c.framePipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    // Colour: add, destination colour (engine factor 8 = VK_BLEND_FACTOR_DST_COLOR, from
    // the factor table at 140fd9a60), one. Alpha: kept (zero, one).
    const uint8_t multiply[]={1,8,1,1,0,1};std::memcpy(desc+0x7a,multiply,sizeof(multiply));
    c.lightPipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    c.shaderHash=ShaderHash();
    if(c.capturePipeline)RefreshSnapshotFormat();
    return true;
}
// After a hot reload whose shaders differ from the ones the kept pipelines were built from,
// rebuild them (render thread, before the frame is recorded). Released pipelines go through
// the engine's deferred release, so frames still in flight keep theirs.
void CompositeRefreshPipelines() {
    auto& c=*g_composite;
    if(!c.enabled||!c.pipeline||!c.layout||!c.vertices||!c.graphics)return;
    if(c.shaderHash==ShaderHash()){RefreshSnapshotFormat();return;}
    for(void** pipeline:{&c.pipeline,&c.blendPipeline,&c.framePipeline,&c.lightPipeline,&c.capturePipeline})
        CompositeReleaseObject(*pipeline);
    if(!CompositePipelines(At<void*>(c.graphics,0x278))&&!c.error)c.error=4;
    greeneye::Host()->log(c.error?"Town Portal: portal shaders could not be rebuilt":"Town Portal: portal shaders rebuilt after a reload");
}
uint8_t* CompositeCommand(void* pass,uint8_t opcode,uint16_t size) {
    auto* commands=At<uint8_t*>(pass,0x10);auto* block=At<uint8_t*>(commands,0x28);
    if(At<uint64_t>(block,0x10)-At<uint64_t>(block,8)<size){
        auto* next=Native<uint8_t*(*)(void*)>(0xfad470)(At<void*>(commands,0));
        At<void*>(block,0)=next;At<void*>(commands,0x28)=next;block=next;
    }
    auto* out=At<uint8_t*>(block,8);At<uint8_t*>(block,8)+=size;++At<uint64_t>(block,0x18);
    std::memset(out,0,size);At<uint16_t>(out,0)=size;out[2]=opcode;return out;
}
// destination: the view frame of where the portal leads (see PortalEndpoint::Turned), for the
// snapshot shown in it.
bool CompositeCoordinates(void* state,const PortalEndpoint& end,bool live,float form,uint32_t tint,bool inFrame,
                          const Snapshot* snapshot,const PortalEndpoint& destination,float exposureScale,
                          const CastEffectState* cast,float* vertices) {
    math::PortalScreenPlane plane{};
    // The game's projection carries its temporal anti-aliasing jitter in [8] and [9] (a new
    // sub-pixel offset every frame, measured live). Drawn into the game's frame the portal
    // keeps it, like the rest of the scene, and the game's anti-aliasing resolves it. Drawn
    // over the finished frame it would shake in place, so there it uses the steady
    // projection and the shader reads the (jittered) depth buffer that far off.
    float projection[16];std::memcpy(projection,&At<float>(state,0x10c0),sizeof(projection));
    float jitter[2]={0,0};
    if(!inFrame){jitter[0]=projection[8];jitter[1]=projection[9];projection[8]=projection[9]=0;}
    const math::Vec3 center{end.wx,end.wy+1.62f,end.wz},horizontal{end.rx*1.02f*1.16f,0,end.rz*1.02f*1.16f},vertical{0,1.42f*1.16f,0};
    // The plane carries only the halo and the forming sparks. Right up close the portal's
    // centre can sit behind the camera's near limit and the plane cannot be projected; the
    // lens is still drawn (from the matrix below), just without those.
    const bool planar=math::ProjectPortalPlane(&At<float>(state,0x1000),projection,center,horizontal,vertical,plane);
    // Ambient light: the matrix places each depth pixel in portal space (normal axis in metres).
    float local[16]{};
    if(!math::PortalLocalFromScreen(&At<float>(state,0x1000),projection,center,horizontal,vertical,
        {end.nx,0,end.nz},local))return false;
    const float xy[3][2]={{-1,-1},{-1,3},{3,-1}};
    for(int i=0;i<3;++i){
        float* v=vertices+i*kVertexFloats;v[0]=xy[i][0];v[1]=xy[i][1];
        if(planar)math::EvaluatePortalPlane(plane,v[0],v[1],v+2);
        else v[2]=v[3]=v[4]=v[5]=0;
        // settings.x: formation 0..1, +10 when the live destination image is bound
        // (CompositeRender adds +20 to endpoints after the first).
        v[6]=form+(live?10.f:0.f);
        v[7]=float(GetTickCount64()%1000000)*.001f;
        std::memcpy(v+8,local,sizeof(local));
        colors::ToFloat(tint,v+24);v[27]=1;
        if(inFrame){v[28]=g_inFrameLight;v[29]=g_inFrameContrast;v[30]=g_inFrameSaturation;v[31]=g_inFrameBrightness;}
        else{v[28]=jitter[0];v[29]=jitter[1];v[30]=0;v[31]=0;}
        if(snapshot){
            SnapshotRows(*snapshot,destination,v+32);
            v[44]=kWindowStart;v[45]=exposureScale;v[46]=1;
            v[47]=SnapshotIsShroud(snapshot)?1.f:0.f;
        }
        // The caster's hand in this portal's space (metres: right, up, normal from its centre).
        if(cast&&cast->effect>0){
            const float hx=cast->hand[0]-end.wx,hy=cast->hand[1]-(end.wy+1.62f),hz=cast->hand[2]-end.wz;
            v[48]=hx*end.rx+hz*end.rz;v[49]=hy;v[50]=hx*end.nx+hz*end.nz;v[51]=float(cast->effect);
        }
    }
    return true;
}

// The endpoints the HUD asked for this frame (or the live-view probe's).
uint32_t CompositeGather(CompositeSlot* slots,uint64_t now) {
    uint32_t count=0;
    if(now<=g_compositeSlotTick||now-g_compositeSlotTick<250)
        for(uint32_t i=0;i<g_compositeSlotCount;++i)slots[count++]=g_compositeSlots[i];
    else if(g_livePortal.active&&now<=g_liveUntil)
        slots[count++]={g_livePortal,g_liveField};
    return count;
}
// The snapshot that best shows what stands in front of an endpoint: one taken for it, else
// one whose camera stood near where it would (2.6 m out, facing the same way, within 6 m and
// 60 degrees). -1 when none.
int FindSnapshot(const PortalEndpoint& end) {
    int best=-1;float bestScore=1e30f;
    const float ix=end.wx+end.nx*2.6f,iy=end.wy+1.62f,iz=end.wz+end.nz*2.6f;
    for(int i=0;i<kSnapshots;++i){
        const auto& s=g_composite->snapshots[i];
        if(!s.valid)continue;
        const float facing=s.nx*end.nx+s.nz*end.nz;
        const float dx=s.x-end.wx,dy=s.y-end.wy,dz=s.z-end.wz;
        float score;
        if(dx*dx+dy*dy+dz*dz<0.5f*0.5f&&facing>0.95f)score=-1e-9f*float(s.tick&0xffffffff);
        else {
            const float cx=s.cx-ix,cy=s.cy-iy,cz=s.cz-iz,distance=std::sqrt(cx*cx+cy*cy+cz*cz);
            if(distance>6.f||facing<0.5f)continue;
            score=distance+(1-facing)*4;
        }
        if(score<bestScore){bestScore=score;best=i;}
    }
    return best;
}
// Records every endpoint into an open pass whose colour target is already attached.
// Post mode (inFrame false): our own full-screen image, the first endpoint replacing it.
// In frame: the game's scene colour, every endpoint blended over it, each with the snapshot
// of where it leads (invExposure: the frame's 1/exposure; 0 shows none). A snapshot shows as
// it looked when taken: its colour is already exposed for that frame. (Scaling by its exposure
// over this frame's made a night snapshot seen by day ~18x too bright, 2026-10-28.)
void CompositeRecord(void* pass,void* state,const ImageView& depth,bool inFrame,
                     const CompositeSlot* slots,uint32_t count,uint64_t now,float invExposure=0) {
    auto& c=*g_composite;
    float vertices[kCompositeSlots][3*kVertexFloats]{};bool drawn[kCompositeSlots]{};
    const Snapshot* snapshot[kCompositeSlots]{};
    for(uint32_t i=0;i<count;++i){
        const auto& slot=slots[i];
        const PortalEndpoint destination=slot.field?slot.portal.baseEnd:slot.portal.fieldEnd.Turned();
        if(inFrame&&invExposure>0){
            bool shroud=false;snapshot[i]=SharedSnapshot(slot.portal,slot.field,shroud);
            if(!snapshot[i]){int local=FindSnapshot(destination);if(local>=0){snapshot[i]=&c.snapshots[local];g_snapshotShown[local]=now;}}
            if(!snapshot[i]&&!slot.field&&!slot.portal.closing)g_missing={slot.portal.portalId,now,destination};
        }
        bool live=!inFrame&&(g_ownedRenderFlags&0x7ff)==0x7ff&&g_renderedPortal==slot.portal.portalId&&g_renderedField==slot.field;
        // Opening: 0..1 over the channel. Closing: shrink back over 0.5 s.
        const auto& p=slot.portal;float form=1;
        if(p.closing)form=1-std::min(1.f,float(now-std::min(now,p.closedTick))/500.f);
        else form=std::min(1.f,float(now-std::min(now,p.openedTick))/float(kPortalFormMs));
        const Snapshot* shown=snapshot[i];
        const bool casting=inFrame&&slot.field&&g_cast.effect>0&&g_cast.portalId==p.portalId&&!p.closing;
        drawn[i]=CompositeCoordinates(state,slot.field?p.fieldEnd:p.baseEnd,live,form,slot.tint,inFrame,
                                      shown,destination,shown?1.f:0.f,
                                      casting?&g_cast:nullptr,vertices[i]);
    }
    Native<void(*)(void*,uint64_t)>(0x47770)(pass,0x7ffffffff);
    auto* graph=At<uint8_t*>(pass,0);
    // Bindings: the destination image (live view or snapshot), the scene depth, the sampler.
    auto makeDescriptor=[&](const ImageView& destination){
        alignas(8) uint8_t bindings[3][0x28]{};
        for(int i=0;i<2;++i){bindings[i][0]=3;At<ImageView>(bindings[i],8)=i?depth:destination;bindings[i][0x18]=7;}
        bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
        alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
        return Native<void*(*)(void*,void*,const void*)>(0x1010700)(At<void*>(graph,0),At<void*>(graph,0x10),set);
    };
    void* shared=makeDescriptor(g_target->color);
    void* descriptor=shared;
    auto bind=[&](void* pipeline){
        auto* command=CompositeCommand(pass,5,0x10);At<void*>(command,8)=pipeline;
        command=CompositeCommand(pass,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
    };
    bind(inFrame?c.framePipeline:c.pipeline);
    // 146470 allocates transient vertices and records its own draw, so each
    // endpoint is one more draw in the same pass with the same pipeline.
    // Slots are nearest first; draw the nearest last so it wins where they overlap.
    uint64_t renderedPortal[kCompositeSlots]{};bool renderedField[kCompositeSlots]{};uint32_t renderedCount=0;
    // Low graphics settings (portal_quality.h): the lighter shader path, and no light pass.
    const bool low=quality::Low([](const char* line){greeneye::Host()->log(line);});
    for(uint32_t i=count;i-->0;){
        if(!drawn[i])continue;
        const bool lightPass=inFrame&&c.lightPipeline&&!low;
        // In the frame each endpoint binds the snapshot of where it leads (the light pass
        // below rebinds, so the switch takes effect there).
        if(inFrame){
            void* wanted=snapshot[i]?makeDescriptor(snapshot[i]->image):shared;
            if(wanted&&wanted!=descriptor){descriptor=wanted;if(!lightPass)bind(c.framePipeline);}
        }
        // Post mode: the first endpoint replaces the whole image (clearing last frame);
        // later ones blend over it, so neither erases the other.
        if(!inFrame&&renderedCount==1)bind(c.blendPipeline);
        if(inFrame||renderedCount)for(int v=0;v<3;++v)vertices[i][v*kVertexFloats+6]+=20.f;
        if(low)for(int v=0;v<3;++v)vertices[i][v*kVertexFloats+6]+=100.f;
        // In the game's frame: the light first (multiplying the scene), then the surface.
        if(lightPass){
            bind(c.lightPipeline);
            if(auto* light=Native<float*(*)(void*,void*,int)>(0x146470)(pass,c.vertices,3)){
                std::memcpy(light,vertices[i],sizeof(vertices[i]));
                for(int v=0;v<3;++v){
                    light[v*kVertexFloats+6]+=40.f;
                    // Snapshot exposure is unused by the light-only branch.
                    light[v*kVertexFloats+45]=g_groundLightGain;
                }
            }
            bind(c.framePipeline);
        }
        auto* mapped=Native<float*(*)(void*,void*,int)>(0x146470)(pass,c.vertices,3);
        if(!mapped)continue;
        std::memcpy(mapped,vertices[i],sizeof(vertices[i]));
        renderedPortal[renderedCount]=slots[i].portal.portalId;
        renderedField[renderedCount]=slots[i].field;++renderedCount;
    }
    std::memcpy(c.renderedPortal,renderedPortal,sizeof(renderedPortal));
    std::memcpy(c.renderedField,renderedField,sizeof(renderedField));
    c.renderedCount=renderedCount;
    Native<void(*)(void*)>(0x6c050)(pass);
}

void CompositeRender(void* scene,void* context) {
    auto& c=*g_composite;
    if(!c.enabled||!c.pipeline||c.error||g_target->error||!c.image.context)return;
    const auto now=GetTickCount64();
    CompositeSlot slots[kCompositeSlots]{};
    const uint32_t count=CompositeGather(slots,now);
    if(!count)return;
    auto* state=At<void*>(scene,0xa0);
    // Native depth alternates between +120/+130 when temporal features are
    // enabled. A fixed +120 samples the previous frame every other frame.
    // Use the same selector as the engine's transparent/lighting passes.
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x123c50)(&depth,state);
    alignas(16) uint8_t pass[0x1200]{};
    c.stage=5;
    Native<void*(*)(void*,void*,const char*)>(0x100cee0)(pass,At<void*>(context,0x10),"portal depth composite");
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(pass,&g_target->color,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(pass,&depth,7,&access);
    const uint8_t attachment[]={0,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6ba30)(pass,&c.image,attachment,"portal depth composite");
    CompositeRecord(pass,state,depth,false,slots,count,now);
    Native<void(*)(void*)>(0x100d680)(pass);
    c.stage=6;++c.frames;c.tick=GetTickCount64();c.portal=slots[0].portal.portalId;c.field=slots[0].field;
}
void CompositeRenderGuard(void* scene,void* context) {
    __try {CompositeRender(scene,context);}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;}
}
