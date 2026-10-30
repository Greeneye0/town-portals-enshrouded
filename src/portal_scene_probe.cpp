#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "portal_build.h"
#include "portal_quality.h"
#include "portal_scene_probe.h"
#include "portal_snapshot_share.h"
#include "portal_snapshot_native.h"
#include "portal_snapshot_timing.h"
#include "portal_snapshot_hide.h"
#include <array>
#include <atomic>
#include <mutex>
#include <cstring>
#include "portal_math.h"
#include "portal_clip_math.h"
#include "portal_composite_math.h"
#include "portal_colors.h"
#include "portal_terrain_policy.h"
#include <intrin.h>
#include "portal_native_signatures.h"

namespace portal::sceneprobe {
namespace {
using RenderFn=void* (*)(void*,void*,void*,void*,void*);
RenderFn g_next{};
using ProjectionDescFn=void* (*)(void*,const void*,float);
using ProjectionMatrixFn=void (*)(float*,const void*);
ProjectionDescFn g_projectionDesc{};
ProjectionMatrixFn g_projectionMatrix{};
void* g_projectionCaller{};
struct ClipScope {bool active{};math::Plane plane{};};
thread_local ClipScope g_clipScope{};
std::atomic<uint64_t> g_exitClipFrames{},g_exitClipRejected{};
bool g_exitClipEnabled=true,g_forceCameraCut{};
struct NativeSpan {const void* data;uint64_t count;};
using InstanceCreateFn=uint32_t (*)(void*,const void*,const void*);
using InstanceRemoveFn=void (*)(void*,const NativeSpan*);
using InstanceTransformFn=void (*)(void*,const NativeSpan*,const NativeSpan*,uint8_t);
InstanceCreateFn g_instanceCreate{};InstanceRemoveFn g_instanceRemove{};
InstanceTransformFn g_instanceTransform{};
struct MirrorRecord {uint32_t source{},owned{};uint64_t guid[2]{};uint8_t transform[56]{};};
struct MirrorState {MirrorRecord records[256]{};uint64_t tick{};uint32_t count{},eligible{},created{},removed{},updated{},unchanged{};};
MirrorState* g_mirrors{};
math::Vec3 g_modelFocus{};bool g_haveModelFocus{};
std::atomic<bool> g_requested{true};
std::atomic<uint64_t> g_calls{};
std::mutex g_lock;
std::atomic<uint64_t> g_previewUntil{};
std::atomic<uint64_t> g_previewFrames{};
PortalInstance g_previewPortal{};
bool g_previewField=true;
bool g_wasPreview=false;
using ScopeFn=void* (*)(void*,void*,uint8_t);
using PlanFn=void* (*)(void*,void*,uint32_t,uint64_t,uint8_t);
using ImageFn=void (*)(void*,void*,const void*);
using CommitFn=void* (*)(void*,void*,uint32_t*,void*);
using EndFn=void (*)(void*);
using ReleaseFn=void (*)(void*,uint32_t*);
using DimensionsFn=uint64_t (*)(const void*);
using CreateFn=void* (*)(void*,void*,void*);
using DestroyFn=void (*)(void**);
using BindFn=void (*)(void*,const void*);
using ReadyFn=uint8_t (*)(void*);
ReadyFn g_sceneReady{};
BindFn g_sceneTick{};
CreateFn g_rootCreate{},g_viewCreate{};
using StateCreateFn=void* (*)(void*,const void*);
StateCreateFn g_stateCreate{};
DestroyFn g_viewDestroy{};
BindFn g_worldBind{};
BindFn g_sceneIbl{};
using TerrainRequestFn=uint8_t (*)(void*,const float*,uint32_t);
TerrainRequestFn g_terrainRequest{};
struct TerrainProgress {
    uint64_t refreshes{},tick{};
    uint32_t requests{},blockTouches{},ready[2]{},requested[2]{},rejected{},skipped{};
    uint32_t roots{},pendingUploads{},pendingCompletions{};
    uint8_t workerBusy{},nodesReady{},meshReady{};
    double refreshUs{},maxRefreshUs{};
    math::Vec3 focus{};
};
TerrainProgress g_terrainProgress{};
uint32_t g_isolationFaults{};
uint64_t g_isolationTick{};
struct WorldTemplate {
    uint64_t root{},scene{},ownedView{};
    uint32_t originalSlots{},reservedSlots{},worldBound{},ownedBound{};
    uint32_t creations{},destructions{};
    uint8_t creation[0xc4]{},binding[0x60]{};
};
WorldTemplate* g_world{};
struct NativeThreads {uint32_t create{},bind{},update{},render{},destroy{};};
NativeThreads* g_nativeThreads{};
std::atomic<bool> g_renderOnce{};
std::atomic<bool> g_ownedPrepared{};
uint64_t g_ownedRenderCalls{},g_ownedRenderTick{};
uint32_t g_ownedRenderFlags{};
ScopeFn g_scope{};PlanFn g_plan{};ImageFn g_image{};CommitFn g_commit{};
EndFn g_end{};ReleaseFn g_release{};DimensionsFn g_dimensions{};
struct ImageView {uint64_t context{},id{};};
using RegisterImageFn=uint32_t (*)(void*,const ImageView*,uint64_t,uint64_t,uint8_t);
using UnregisterImageFn=void (*)(void*,uint32_t);
using BindImageFn=void (*)(void*,uint32_t,const ImageView*);
BindImageFn g_bindImage{};
RegisterImageFn g_registerImage{};UnregisterImageFn g_unregisterImage{};
using ImportImageFn=ImageView* (*)(ImageView*,void*,void*,uint8_t,uint64_t);
using FreeImageFn=void (*)(void*,ImageView*);
ImportImageFn g_importImage{};FreeImageFn g_freeImage{};
ImageView* g_output{};
struct DisplayState {void* registry{};uint32_t image{};};
DisplayState* g_display{};
DisplayState* g_auxDisplays{};
PortalInstance g_livePortal{};
bool g_liveField{},g_renderedField{};
uint64_t g_liveUntil{},g_liveSelectTick{},g_renderedPortal{},g_mappedFrames{};
uint64_t g_displayProbeUntil{};
std::array<uint8_t,0x28> g_probePose{};
bool g_probePoseValid{};
std::array<uint8_t,0x28> g_destinationPose{};
bool g_destinationPoseValid{};
std::atomic<uint64_t> g_gameplayHudTick{};
// Loader-owned POD storage survives hot reload. Every GPU resource is owned by
// a native allocation ticket, never by a copy of the game's primary scene.
struct TargetState {
    uint64_t root{};
    ImageView color{},depth{};
    void* view{};
    uint32_t ticket{},width{},height{},allocations{},releases{},error{};
    uint32_t requested{},completed{};
};
TargetState* g_target{};
uint64_t g_faultIp{},g_faultAddress{},g_faultCx{},g_faultDx{};
int TargetFault(EXCEPTION_POINTERS* info) {
    g_target->error=info->ExceptionRecord->ExceptionCode;
    g_faultIp=info->ContextRecord->Rip;
    g_faultCx=info->ContextRecord->Rcx;g_faultDx=info->ContextRecord->Rdx;
    g_faultAddress=info->ExceptionRecord->NumberParameters>1?info->ExceptionRecord->ExceptionInformation[1]:0;
    // Preserve the exception even if a later GPU wait prevents MCP dispatch.
    char report[4096]{};auto base=reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    int length=std::snprintf(report,sizeof(report),"{\"code\":%u,\"rva\":\"0x%llx\",\"address\":\"0x%llx\",\"cx\":\"0x%llx\",\"dx\":\"0x%llx\"}",
        g_target->error,g_faultIp-base,g_faultAddress,g_faultCx,g_faultDx);
    CONTEXT cursor=*info->ContextRecord;
    length-=1;length+=std::snprintf(report+length,sizeof(report)-length,",\"stackRvas\":[");
    for(int i=0;i<24&&cursor.Rip;i++) {
        length+=std::snprintf(report+length,sizeof(report)-length,"%s\"0x%llx\"",i?",":"",cursor.Rip-base);
        DWORD64 imageBase{},establisher{};void* handler{};
        auto entry=RtlLookupFunctionEntry(cursor.Rip,&imageBase,nullptr);
        if(!entry) {cursor.Rip=*reinterpret_cast<DWORD64*>(cursor.Rsp);cursor.Rsp+=8;}
        else RtlVirtualUnwind(UNW_FLAG_NHANDLER,imageBase,cursor.Rip,entry,&cursor,&handler,&establisher,nullptr);
    }
    length+=std::snprintf(report+length,sizeof(report)-length,"]}");
    HANDLE file=CreateFileW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\native-render-fault.json",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written{};WriteFile(file,report,length,&written,nullptr);CloseHandle(file);}
    return EXCEPTION_EXECUTE_HANDLER;
}
std::atomic<uint32_t> g_targetRequest{};
template<class T> T& At(void* p,size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(p)+offset);
}
// The world scene's render state (view +a0), or null once ViewDestroyHook cleared the scene:
// leaving a world destroys it while the upscale pass keeps running. Read under g_lock.
uint8_t* WorldState() {
    return g_world&&g_world->scene?At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0xa0):nullptr;
}
#include "portal_composite.inl"
void ImageDesc(uint8_t* d,uint32_t width,uint32_t height,uint8_t format,uint8_t usage) {
    std::memset(d,0,0x30);
    At<uint32_t>(d,4)=width;At<uint32_t>(d,8)=height;At<uint32_t>(d,12)=1;
    At<uint32_t>(d,16)=0x1010001;
    d[20]=1;d[21]=format;d[22]=usage;
    if(format==0x7e) At<uint32_t>(d,0x2c)=0x10400;
}
// Snapshots get their own allocation ticket: if it fails, the portals draw as before.
void AllocateSnapshots(uint8_t* root) {
    auto& c=*g_composite;
    if(!c.enabled||!c.capturePipeline||c.snapshotTicket)return;
    alignas(16) uint8_t scope[0x180]{},plan[0x20]{},desc[0x30]{},result[16]{};
    g_scope(scope,At<void*>(root,0x128),0);
    g_plan(plan,scope,At<uint32_t>(root,0x270),0,1);
    for(auto& snapshot:c.snapshots){
        ImageDesc(desc,kSnapshotWidth,kSnapshotHeight,0x61,0x4f); // also readback and upload (the home snapshot cache)
        g_image(plan,&snapshot.image,desc);
    }
    g_commit(scope,result,&c.snapshotTicket,At<void*>(root,0x160));
    c.snapshotError=result[0];
    g_end(scope);
    bool ready=!c.snapshotError&&c.snapshotTicket;
    for(const auto& snapshot:c.snapshots)ready=ready&&snapshot.image.context;
    if(!ready){
        if(c.snapshotTicket)g_release(At<void*>(root,0x160),&c.snapshotTicket);
        for(auto& snapshot:c.snapshots)snapshot={};
        c.snapshotTicket=0;
    }
    char line[120]{};
    std::snprintf(line,sizeof(line),ready?"Town Portal: portal snapshots ready (%ux%u)":"Town Portal: portal snapshots unavailable (error %u)",
                  ready?kSnapshotWidth:c.snapshotError,kSnapshotHeight);
    greeneye::Host()->log(line);
}
void AllocateTarget(void* scene) {
    if(g_target->ticket) return;
    auto* root=At<uint8_t*>(scene,0);
    alignas(16) uint8_t scope[0x180]{},plan[0x20]{},desc[0x30]{},result[16]{};
    g_scope(scope,At<void*>(root,0x128),0);
    // 1273043 root constructor: +270 is the persistent region (policy 0),
    // +27c is the view-intermediate region (policy 1). The image handle's
    // retained bit alone does not establish independent backing storage.
    g_plan(plan,scope,At<uint32_t>(root,0x270),0,1);
    // Same native formats/usages as the world's color intermediate and D32
    // depth attachments (build 1273043: 1400f1cb0 / 140147750).
    ImageDesc(desc,1280,360,0x25,0x4c);
    g_image(plan,&g_target->color,desc);
    // The composite image is 2560x1440 whatever the screen size (at least the
    // screen's height on 1080p and 1440p, so outlines are not upscaled);
    // portal_composite.frag divides gl_FragCoord by the same size.
    if(CompositeAllowed()){
        ImageDesc(desc,2560,1440,0x25,0x4c);
        g_image(plan,&g_composite->image,desc);
    }
    ImageDesc(desc,1280,360,0x7e,0x14);
    g_image(plan,&g_target->depth,desc);
    g_commit(scope,result,&g_target->ticket,At<void*>(root,0x160));
    g_target->error=result[0];
    g_end(scope);
    g_target->root=reinterpret_cast<uint64_t>(root);
    if(!g_target->error&&g_target->ticket&&g_target->color.context&&g_target->depth.context) {
        uint64_t color=g_dimensions(&g_target->color),depth=g_dimensions(&g_target->depth);
        g_target->width=static_cast<uint32_t>(color);
        g_target->height=static_cast<uint32_t>(color>>32);
        if(color!=depth||g_target->width!=1280||g_target->height!=360) g_target->error=0x10001;
        ++g_target->allocations;
        CompositeCreate();
        AllocateSnapshots(root);
    }
}
void DestroyOwnedView() {
    g_viewDestroy(&g_target->view);
}
void ReleaseTarget() {
    CompositeRelease();
    g_destinationPoseValid=false;
    g_terrainProgress={};g_isolationTick=0;g_isolationFaults=0;
    g_renderOnce=false;g_ownedPrepared=false;g_liveUntil=0;g_displayProbeUntil=0;g_renderedPortal=0;g_ownedRenderTick=0;
    g_ownedRenderFlags=0;g_ownedRenderCalls=0;
    if(g_display&&g_display->image){g_unregisterImage(g_display->registry,g_display->image);g_display->image=0;g_display->registry=nullptr;}
    if(g_auxDisplays)for(size_t i=0;i<2;++i)if(g_auxDisplays[i].image){g_unregisterImage(g_auxDisplays[i].registry,g_auxDisplays[i].image);g_auxDisplays[i]={};}
    if(g_target->view) {
        if(!g_world||g_world->ownedView!=reinterpret_cast<uint64_t>(g_target->view)) {
            g_target->error=0x10003;return;
        }
        // Only the native constructor's result may enter its native destructor.
        DestroyOwnedView();
        g_world->ownedView=0;g_world->ownedBound=0;++g_world->destructions;
        if(g_mirrors)*g_mirrors={};g_haveModelFocus=false;
    }
    if(g_output&&g_output->context)g_freeImage(reinterpret_cast<void*>(g_output->context),g_output);
    if(g_target->ticket&&g_target->root) {
        g_release(At<void*>(reinterpret_cast<void*>(g_target->root),0x160),&g_target->ticket);
        ++g_target->releases;
    }
    g_target->root=0;g_target->color={};g_target->depth={};
    g_target->width=0;g_target->height=0;g_target->error=0;
}
void CreateOwnedView(void* scene) {
    if(g_target->view||!g_target->ticket||g_target->error) return;
    if(!g_world||g_world->scene!=reinterpret_cast<uint64_t>(scene)||g_world->reservedSlots<4) {
        g_target->error=0x10004;return;
    }
    auto* root=At<uint8_t*>(scene,0);
    auto capacity=At<uint64_t>(root,0x16a08+24)*At<uint64_t>(root,0x16a08+64);
    if(At<uint64_t>(root,0x16a08+88)>=capacity) {g_target->error=0x10005;return;}
    alignas(16) uint8_t description[0xc4]{},result[16]{};
    std::memcpy(description,g_world->creation,sizeof(description));
    // WaterBrickTree creates TWO extra task queues when this world's water
    // system is present (1024c0 -> 1f99e0). Budget only the terrain brick tree
    // and terrain mesher; don't let optional water exhaust the 32-slot pool.
    At<uint32_t>(description,4)=(At<uint32_t>(description,4)|0x100)&~0x80u;
    // Bounded world configuration: culling, shadows, VFX, fog, grass, SSAO,
    // heightmap, contact shadows, TAA and bricks, with GI dependencies.
    // No shared SSR, DLSS or UI frosted glass. f4140 maps public bit16
    // to internal feature17; clearing public bit17 would NOT exclude SSR.
    constexpr uint32_t features=0x0a3027af; // GI dependencies; Water disabled with WaterBrickTree.
    static_assert(!(features & 0x10000));
    At<uint32_t>(description,0x40)=features;
    // Development A/B only: isolate camera-local HZB culling from missing
    // destination geometry. Read once at creation, never mutate a live view.
    if(GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\occlusion.disabled")!=INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)&=~1u;
    At<uint32_t>(description,0x4c)=1280;At<uint32_t>(description,0x50)=360;
    At<float>(description,0x54)=1.0f; // Match output: avoid a globally aliased resolution intermediate.
    g_viewCreate(result,root,description);
    g_target->error=result[0];
    if(!g_target->error) {
        g_target->view=At<void*>(result,8);
        g_world->ownedView=reinterpret_cast<uint64_t>(g_target->view);
        ++g_world->creations;
        greeneye::Host()->log("Town Portal: native-owned experimental camera created");
    }
}
void* StateCreateHook(void* models,const void* description) {
    // Keep the startup hook registered across hot reloads. Every scene in the
    // root worker list requires its own camera link (1168a0); never replace it.
    return g_stateCreate(models,description);
}
void* CreateRootWithSlots(void* result,void* allocator,void* description) {
    uint32_t saved=At<uint32_t>(description,0x5c);
    // Alter the startup descriptor before any pools or scene-indexed systems
    // exist. Never resize or replace a live scene pool.
    At<uint32_t>(description,0x5c)=std::max(saved,4u);
    __try {return g_rootCreate(result,allocator,description);}
    __finally {At<uint32_t>(description,0x5c)=saved;}
}
void* RootCreateHook(void* result,void* allocator,void* description) {
    greeneye::Lease lease;
    if(!lease) return g_rootCreate(result,allocator,description);
    uint32_t saved=At<uint32_t>(description,0x5c);
    void* returned=CreateRootWithSlots(result,allocator,description);
    if(!At<uint8_t>(result,0)) {
        std::lock_guard lock(g_lock);
        g_world->root=At<uint64_t>(result,8);
        g_world->originalSlots=saved;g_world->reservedSlots=std::max(saved,4u);
        greeneye::Host()->log("Town Portal: native scene capacity reserved during renderer initialization");
    }
    return returned;
}
void* ViewCreateHook(void* result,void* root,void* description) {
    greeneye::Lease lease;
    void* returned=g_viewCreate(result,root,description);
    if(lease&&!At<uint8_t>(result,0)&&At<uint32_t>(description,4)==0xff) {
        std::lock_guard lock(g_lock);
        g_nativeThreads->create=GetCurrentThreadId();
        g_world->scene=At<uint64_t>(result,8);g_world->worldBound=0;
        std::memcpy(g_world->creation,description,sizeof(g_world->creation));
    }
    return returned;
}
void WorldBindHook(void* models,const void* description) {
    greeneye::Lease lease;
    g_worldBind(models,description);
    if(lease) {
        std::lock_guard lock(g_lock);
        if(At<uint64_t>(models,0)==g_world->scene) {
            g_nativeThreads->bind=GetCurrentThreadId();
            std::memcpy(g_world->binding,description,sizeof(g_world->binding));
            g_world->worldBound=1;
        }
    }
}
void ViewDestroyHook(void** view) {
    greeneye::Lease lease;
    if(lease) {
        std::lock_guard lock(g_lock);
        if(reinterpret_cast<uint64_t>(*view)==g_world->scene) {
            g_nativeThreads->destroy=GetCurrentThreadId();
            if(g_target) ReleaseTarget();
            g_world->scene=0;g_world->worldBound=0;
        }
    }
    g_viewDestroy(view);
}
bool QueueCapacity(void* scene,uint32_t needed) {
    // 1273043: 8df560 checks per-worker limits but dereferences a null global
    // pool allocation when all 32 slots are occupied. Releases are deferred
    // until workers acknowledge removal (8e0b70 -> 8e0730).
    auto* scheduler=At<void*>(At<void*>(scene,0),0x148);
    const auto used=At<uint32_t>(scheduler,0xe0);
    const auto capacity=At<uint32_t>(scheduler,0xf0);
    if(capacity<used||capacity-used<needed) {g_target->error=0x10006;return false;}
    return true;
}
void ProcessTargetRequest(void* scene,uint32_t action) {
    // Only the native rendering thread touches allocation plans or view state.
    __try {
        if(action==8) ReleaseTarget();
        else if(action==7||action==9) {
            AllocateTarget(scene);
            if(action==9&&QueueCapacity(scene,2)) CreateOwnedView(scene);
        }
        else if(action==10&&g_target->view&&g_world->worldBound&&!g_world->ownedBound) {
            if(!QueueCapacity(scene,1)) return;
            g_worldBind(At<void*>(g_target->view,0x98),g_world->binding);
            g_world->ownedBound=1;
            greeneye::Host()->log("Town Portal: world attached to native-owned camera");
        }
        else if((action==11||action==12)&&g_target->view&&g_world->ownedBound) {
            g_renderOnce=true;
            if(action==12){
                g_displayProbeUntil=GetTickCount64()+60000;g_probePoseValid=false;
                // Development-only A/B switch, read once per explicit MCP probe.
                const bool clip=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\exit-clip.disabled")==INVALID_FILE_ATTRIBUTES;
                g_forceCameraCut=g_forceCameraCut||clip!=g_exitClipEnabled;g_exitClipEnabled=clip;
            }
        }
    } __except(TargetFault(GetExceptionInformation())) {}
}
struct Attachment {
    ImageView image{};
    uint32_t width{},height{},format{},usage{};
    bool valid{};
};
Attachment InspectImage(const ImageView& image) {
    Attachment out{};
    if(!image.context||!image.id) return out;
    __try {
        // Build 1273043: native imageDimensions (141016cb0) and imageFormat
        // (141016b50) use this descriptor table. No GPU access or state changes.
        auto* context=reinterpret_cast<void*>(image.context);
        auto* descriptors=At<uint8_t*>(context,0x190);
        auto* desc=descriptors+(image.id&0xffff)*0x30;
        out.width=At<uint32_t>(desc,4);out.height=At<uint32_t>(desc,8);
        out.format=desc[0x15];out.usage=desc[0x16];
        out.valid=out.width>0&&out.height>0&&out.width<=16384&&out.height<=16384;
        if(out.valid) out.image=image;
    } __except(EXCEPTION_EXECUTE_HANDLER) {out={};}
    return out;
}
#include "portal_snapshot_gpu.inl"
struct Capture {
    uint64_t tick{},scene{},context{};
    uint32_t thread{},width{},height{};
    uint64_t modelScene{},cameraState{},sceneCapacity{},sceneCount{};
    uint32_t renderWidth{},renderHeight{};
    Attachment depth[2]{},intermediate{};
    std::array<uint8_t,0x420> input{};
    std::array<uint8_t,0x28> target{};
    std::array<uint8_t,0xf8> sceneData{};
} g_capture;

bool MapCamera(std::array<uint8_t,0x420>* out,const void* input,const PortalInstance* portal,bool field) {
    __try {
        std::memcpy(out->data(),input,out->size());
        auto* data=out->data();
        FixedPos fixed{};
        std::memcpy(&fixed,data+0x38,sizeof(fixed));
        math::Vec3 camera{};
        ToFloat(fixed,camera.x,camera.y,camera.z);
        if (!math::Finite(camera)||camera.x<100||camera.x>12000||camera.z<100||camera.z>12000) return false;
        const auto& from=field?portal->fieldEnd:portal->baseEnd;
        const auto& to=field?portal->baseEnd:portal->fieldEnd;
        math::Vec3 mapped=math::MapPortalPoint(camera,{from.wx,from.wy+1.62f,from.wz},
            {from.rx,0,from.rz},{from.nx,0,from.nz},{to.wx,to.wy+1.62f,to.wz},
            {to.rx,0,to.rz},{to.nx,0,to.nz});
        if (!math::Finite(mapped)) return false;
        float q[4]{};std::memcpy(q,data+0x50,sizeof(q));
        float length=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
        if (!std::isfinite(length)||std::fabs(length-1)>0.02f) return false;
        float turn=std::atan2(to.nx,to.nz)-std::atan2(from.nx,from.nz)+3.14159265359f;
        float sn=std::sin(turn*0.5f),cs=std::cos(turn*0.5f);
        float rotated[4]={cs*q[0]+sn*q[2],cs*q[1]+sn*q[3],cs*q[2]-sn*q[0],cs*q[3]-sn*q[1]};
        fixed=ToFixed(mapped.x,mapped.y,mapped.z);
        std::memcpy(data+0x38,&fixed,sizeof(fixed));
        std::memcpy(data+0x50,rotated,sizeof(rotated));
        data[0]=1; // Explicit camera cut; preserve the caller's original input block.
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* ProjectionDescHook(void* output,const void* camera,float aspect) {
    // Retain the already-installed descriptor hook across hot reloads. Native
    // culling/effects also consume this descriptor: leave its type/near/far intact.
    return g_projectionDesc(output,camera,aspect);
}
void ProjectionMatrixHook(float* output,const void* description) {
    const bool ownedCall=g_clipScope.active&&_ReturnAddress()==g_projectionCaller;
    greeneye::Lease lease;
    g_projectionMatrix(output,description);
    if(lease&&ownedCall&&*static_cast<const uint32_t*>(description)==0){
        if(math::ClipReverseZ(output,g_clipScope.plane))++g_exitClipFrames;
        else ++g_exitClipRejected;
    }
}

void RenderWithExitPlane(void* result,void* context,void* input,void* target,
                         const uint8_t* mapped,bool destination);

void* CallWithCamera(RenderFn fn,void* result,void* scene,void* context,void* input,void* target,
                     const uint8_t* mapped,bool offscreen=false) {
    // Keep the engine input at its original address. Only the verified camera
    // fields (pose, vertical field of view at +78) and UI-blur enable byte are patched,
    // and restored even on fault.
    auto* bytes=static_cast<uint8_t*>(input);
    uint8_t savedCamera[0x28]{};
    uint8_t savedCut=bytes[0],savedGlass=bytes[0x285],savedTerrainOverride=bytes[0x2b0];
    float savedFov;std::memcpy(&savedFov,bytes+0x78,sizeof(savedFov));
    std::memcpy(savedCamera,bytes+0x38,sizeof(savedCamera));
    __try {
        std::memcpy(bytes+0x38,mapped+0x38,sizeof(savedCamera));
        std::memcpy(bytes+0x78,mapped+0x78,sizeof(float));
        bytes[0]=mapped[0];
        // 1273043: input+285 enables interface frosted glass (13b466).
        // Independent views have no UI blur mip chain; native previews leave
        // this byte zero. Enabling it dereferences a null image at 07ea57 and
        // aborts recording, leaving timestamp queries unfinished.
        if(offscreen) {
            bytes[0x285]=0;
            // 124080 picks input+298 when optional flag +2b0 is set.
            // The player override would build high-detail terrain at the entry
            // while rendering the destination. Let the native selector use
            // this camera's +38 pose instead; restore the primary flag below.
            bytes[0x2b0]=0;
        }
        return fn(result,scene,context,input,target);
    } __finally {
        std::memcpy(bytes+0x38,savedCamera,sizeof(savedCamera));
        std::memcpy(bytes+0x78,&savedFov,sizeof(savedFov));
        bytes[0]=savedCut;bytes[0x285]=savedGlass;bytes[0x2b0]=savedTerrainOverride;
    }
}

void RenderWithExitPlane(void* result,void* context,void* input,void* target,
                         const uint8_t* mapped,bool destination) {
    const ClipScope saved=g_clipScope;
    __try {
        // Rendering consumes model/animation queues. Always use this camera's
        // own native model scene; substituting the primary scene is forbidden.
        g_clipScope={};
        if(destination&&g_exitClipEnabled){
            const auto& end=g_liveField?g_livePortal.baseEnd:g_livePortal.fieldEnd;
            FixedPos fixed{};std::memcpy(&fixed,mapped+0x38,sizeof(fixed));
            math::Vec3 position{};ToFloat(fixed,position.x,position.y,position.z);
            float q[4]{};std::memcpy(q,mapped+0x50,sizeof(q));
            const math::Vec3 normal{end.nx,0,end.nz},center{end.wx,end.wy+1.62f,end.wz};
            g_clipScope.plane={math::InverseRotate(normal,q),math::Dot(normal,position-center)-0.015f};
            g_clipScope.active=true;
        }
        CallWithCamera(g_next,result,g_target->view,context,input,target,mapped,true);
    } __finally {g_clipScope=saved;}
}

bool Copy(Capture* out,void* scene,void* context,void* input,void* target) {
    __try {
        auto* p=static_cast<uint8_t*>(scene);
        out->width=*reinterpret_cast<uint32_t*>(p+0x1c);
        out->height=*reinterpret_cast<uint32_t*>(p+0x20);
        if (out->width<640||out->height<360||out->width>16384||out->height>16384) return false;
        std::memcpy(out->sceneData.data(),scene,out->sceneData.size());
        std::memcpy(out->input.data(),input,out->input.size());
        std::memcpy(out->target.data(),target,out->target.size());
        out->scene=reinterpret_cast<uint64_t>(scene);
        out->context=reinterpret_cast<uint64_t>(context);
        out->thread=GetCurrentThreadId();
        out->tick=GetTickCount64();
        auto* root=At<uint8_t*>(scene,0);
        auto* state=At<uint8_t*>(scene,0xa0);
        out->modelScene=At<uint64_t>(scene,0x98);
        out->cameraState=reinterpret_cast<uint64_t>(state);
        out->sceneCapacity=At<uint64_t>(root,0x16a08+24)*At<uint64_t>(root,0x16a08+64);
        out->sceneCount=At<uint64_t>(root,0x16a08+88);
        if(state) {
            out->renderWidth=At<uint32_t>(state,0x9c);
            out->renderHeight=At<uint32_t>(state,0xa0);
            out->depth[0]=InspectImage(At<ImageView>(state,0x120));
            out->depth[1]=InspectImage(At<ImageView>(state,0x130));
        }
        if(auto* intermediate=At<uint8_t*>(scene,0xe8))
            out->intermediate=InspectImage(At<ImageView>(intermediate,8));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
terrain::Identity SceneIdentity(void* view) {
    terrain::Identity id{};
    if(!view)return id;
    id.view=reinterpret_cast<uintptr_t>(view);
    id.models=At<uintptr_t>(view,0x98);id.camera=At<uintptr_t>(view,0xa0);
    id.resources=At<uintptr_t>(view,0xb8);
    if(id.models){auto* m=reinterpret_cast<void*>(id.models);id.modelView=At<uintptr_t>(m,0);id.modelCamera=At<uintptr_t>(m,0x40);}
    if(id.camera){auto* c=reinterpret_cast<void*>(id.camera);id.cameraModels=At<uintptr_t>(c,8);id.mesher=At<uintptr_t>(c,0x978);}
    if(id.mesher){auto* m=reinterpret_cast<void*>(id.mesher);id.meshModels=At<uintptr_t>(m,0x378);id.queue=At<uintptr_t>(m,0x3a0);}
    return id;
}
bool CheckOwnedIsolation() {
    const auto primary=SceneIdentity(reinterpret_cast<void*>(g_world->scene));
    const auto owned=SceneIdentity(g_target->view);
    g_isolationTick=GetTickCount64();
    g_isolationFaults=terrain::IsolationFaults(primary,owned);
    if(owned.models&&At<void*>(reinterpret_cast<void*>(owned.models),0xa50)&&!owned.mesher)g_isolationFaults|=16;
    if(!g_isolationFaults)return true;
    g_target->error=0x10007;g_renderOnce=false;g_ownedPrepared=false;g_liveUntil=0;
    return false;
}
void RefreshDestinationTerrain() {
    const auto now=GetTickCount64();
    if(!g_haveModelFocus||!g_terrainRequest||now>=g_displayProbeUntil||now-g_gameplayHudTick.load()>=250)return;
    terrain::Bounds bounds{};
    if(!terrain::DestinationBounds(g_modelFocus,bounds))return;
    auto* state=At<void*>(g_target->view,0xa0);
    auto* mesher=At<uint8_t*>(state,0x978);
    if(!mesher)return;
    auto& progress=g_terrainProgress;
    progress.tick=now;progress.focus=g_modelFocus;progress.requests=0;progress.blockTouches=0;
    progress.ready[0]=progress.ready[1]=progress.requested[0]=progress.requested[1]=0;
    LARGE_INTEGER begin{},end{},frequency{};QueryPerformanceCounter(&begin);QueryPerformanceFrequency(&frequency);
    // Only read diagnostic counters under the mesher lock. Never wait for its
    // worker, drain its queue, or force its readiness bits from this thread.
    auto* lock=reinterpret_cast<CRITICAL_SECTION*>(mesher+0x338);
    if(TryEnterCriticalSection(lock)) {
        progress.roots=uint32_t(At<uint64_t>(mesher,0x398));
        progress.pendingUploads=At<uint32_t>(mesher,0xc43c);
        progress.pendingCompletions=uint32_t(At<uint64_t>(mesher,0xc4a8));
        progress.workerBusy=At<uint8_t>(mesher,0x368);
        progress.nodesReady=At<uint8_t>(mesher,0xc42b);progress.meshReady=At<uint8_t>(mesher,0xc42c);
        LeaveCriticalSection(lock);
    } else ++progress.skipped;
    // The native content loader ages untouched blocks after two update ticks
    // (10b4de0). A 10 Hz GPU render is too sparse to maintain remote requests
    // under cache pressure. Refresh bounded read interest every scene update;
    // let native meshing and GPU rendering retain their own schedules.
    auto* source0=At<uint8_t*>(mesher,0x3c8);
    for(uint32_t sourceIndex=0;sourceIndex<2;++sourceIndex) {
        auto* source=At<uint8_t*>(mesher,0x3c8+sourceIndex*8);
        if(!source||(sourceIndex&&source==source0)||!At<uint32_t>(source,0x14))continue;
        for(uint32_t level=0;level<terrain::kLevels;++level) {
            const auto grid=0x40+level*0x28;
            uint32_t blocks{};
            if(!terrain::BlockTouches(bounds,At<float>(source,0x30),At<float>(source,0x38),level,
               At<uint32_t>(source,grid),At<uint32_t>(source,grid+4),At<uint32_t>(source,grid+8),At<uint32_t>(source,grid+12),blocks)||
               blocks>terrain::kMaxBlockTouches-progress.blockTouches) {++progress.rejected;continue;}
            if(!blocks)continue;
            ++progress.requests;progress.blockTouches+=blocks;progress.requested[sourceIndex]|=1u<<level;
            if(g_terrainRequest(source,bounds.values,level))progress.ready[sourceIndex]|=1u<<level;
        }
    }
    ++progress.refreshes;QueryPerformanceCounter(&end);
    progress.refreshUs=frequency.QuadPart?double(end.QuadPart-begin.QuadPart)*1000000./double(frequency.QuadPart):0;
    progress.maxRefreshUs=std::max(progress.maxRefreshUs,progress.refreshUs);
}
void PrepareOwned() {
    __try {
        // Render and sample the same retained graph resource. Importing the
        // physical texture again hides the write/read dependency from the graph.
        // The native BeginFrame uploads every scene's pending data. Updating
        // here, alongside the primary view, keeps those uploads ahead of draw.
        g_ownedPrepared=g_sceneReady(At<void*>(g_target->view,0x98))!=0;

    } __except(TargetFault(GetExceptionInformation())) { g_renderOnce=false; }
}
void SyncOwnedInstances() {
    // Native instance creation acquires a reference to the shared model asset;
    // transforms and instance handles belong exclusively to the owned scene.
    // No primary GPU buffer, skeleton, linked-list node or destructor is copied.
    if(!g_mirrors||!g_haveModelFocus||GetTickCount64()-g_mirrors->tick<100)return;
    g_mirrors->tick=GetTickCount64();
    auto* source=At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0x98);
    auto* owned=At<void*>(g_target->view,0x98);
    if(!g_sceneReady(owned))return;
    auto capacity=At<uint64_t>(source,0x1d0);
    auto* active=At<uint64_t*>(source,0x208);
    auto* generations=At<uint8_t*>(source,0x1c8);
    auto* instances=At<uint8_t*>(source,0x1b0);
    if(capacity>65536||!active||!generations||!instances)return;
    auto distance=[&](uint32_t index)->float {
        if(index>=capacity||!(active[index/64]&(uint64_t(1)<<(index%64))))return 1e30f;
        auto* instance=instances+size_t(index)*0x1b0;
        // Skinned/cloth instances need a separate animation pipeline. Do not
        // present a frozen bind-pose actor as a live animated destination.
        if(At<uint32_t>(instance,0x130)||!At<void*>(instance,0x108)||!At<uint32_t>(instance,0x114))return 1e30f;
        FixedPos fixed{};std::memcpy(&fixed,instance+0x38,sizeof(fixed));
        math::Vec3 p{};ToFloat(fixed,p.x,p.y,p.z);
        return math::Finite(p)?math::LengthSq(p-g_modelFocus):1e30f;
    };
    // Prioritize objects nearest the destination, rather than whichever native
    // slots happened to be allocated first. Visit active bits only. The heap
    // and selected bitset are bounded regardless of the world's object count.
    struct Candidate {float distance;uint32_t index;};
    std::array<Candidate,256> candidates{};size_t count=0;
    auto nearer=[](const Candidate& a,const Candidate& b){return a.distance<b.distance||(a.distance==b.distance&&a.index<b.index);};
    g_mirrors->eligible=0;
    for(uint32_t word=0;word<(capacity+63)/64;++word){
        uint64_t bits=active[word];
        while(bits){
            unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
            const uint32_t index=word*64+bit;float d=distance(index);
            if(d>=160.0f*160.0f)continue;
            ++g_mirrors->eligible;Candidate candidate{d,index};
            if(count<candidates.size()){candidates[count++]=candidate;std::push_heap(candidates.begin(),candidates.begin()+count,nearer);}
            else if(nearer(candidate,candidates[0])){std::pop_heap(candidates.begin(),candidates.end(),nearer);candidates.back()=candidate;std::push_heap(candidates.begin(),candidates.end(),nearer);}
        }
    }
    std::sort_heap(candidates.begin(),candidates.begin()+count,nearer);
    std::array<uint64_t,1024> selected{};
    for(size_t i=0;i<count;++i)selected[candidates[i].index/64]|=uint64_t(1)<<(candidates[i].index%64);
    for(auto& entry:g_mirrors->records)if(entry.owned){
        const uint32_t index=entry.source&0xffffff;
        bool keep=index<capacity&&(selected[index/64]&(uint64_t(1)<<(index%64)))&&generations[index]==uint8_t(entry.source>>24);
        auto* instance=instances+size_t(index)*0x1b0;
        if(keep)keep=std::memcmp(At<uint8_t*>(instance,0x108)+0x18,entry.guid,16)==0;
        NativeSpan ids{&entry.owned,1};
        if(!keep){g_instanceRemove(owned,&ids);entry={};--g_mirrors->count;++g_mirrors->removed;}
        else if(std::memcmp(entry.transform,instance+0x38,56)!=0){
            NativeSpan transform{instance+0x38,1};g_instanceTransform(owned,&ids,&transform,0);
            std::memcpy(entry.transform,instance+0x38,56);++g_mirrors->updated;
        } else ++g_mirrors->unchanged;
    }
    uint32_t budget=8;
    for(size_t candidate=0;candidate<count&&budget&&g_mirrors->count<256;++candidate){
        const uint32_t index=candidates[candidate].index;
        const uint32_t handle=(uint32_t(generations[index])<<24)|index;
        MirrorRecord* empty=nullptr;bool exists=false;
        for(auto& entry:g_mirrors->records){if(entry.source==handle&&entry.owned){exists=true;break;}if(!entry.owned&&!empty)empty=&entry;}
        if(exists||!empty)continue;
        auto* instance=instances+size_t(index)*0x1b0;
        auto* guid=At<uint8_t*>(instance,0x108)+0x18;
        uint32_t created=g_instanceCreate(owned,guid,instance+0x38);--budget;
        if(created){empty->source=handle;empty->owned=created;std::memcpy(empty->guid,guid,16);std::memcpy(empty->transform,instance+0x38,56);++g_mirrors->count;++g_mirrors->created;}
    }
}
void TickOwned(const void* description) {
    __try {
        if(!CheckOwnedIsolation())return;
        auto* primary=At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0x98);
        auto* owned=At<uint8_t*>(g_target->view,0x98);
        // The client separately updates IblResource (1100d0) after binding its
        // terrain world. Copy only the asset GUID; native code acquires/clears
        // the owned scene's handle and update tick uploads its own resources.
        // Do not copy handles, GPU buffers or the primary dirty queues.
        if(g_sceneIbl&&std::memcmp(primary+0x58,owned+0x58,16))
            g_sceneIbl(owned,primary+0x58);
        RefreshDestinationTerrain();
        SyncOwnedInstances();
        g_sceneTick(g_target->view,description);
    }
    __except(TargetFault(GetExceptionInformation())) {g_renderOnce=false;}
}
void CompositeRefreshGuard() {
    __try {CompositeRefreshPipelines();}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;}
}
void SceneTickHook(void* scene,const void* description) {
    greeneye::Lease lease;
    // Allocation, binding and destruction must precede BeginFrame and its
    // command recording; never perform them inside the main render pass.
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        {
            std::lock_guard lock(g_lock);
            // End a bounded live probe by releasing its resources as well as
            // stopping draws. Otherwise native model/terrain updates continue
            // consuming CPU and GPU memory after the diagnostic disappears.
            const auto now=GetTickCount64();
            if(g_displayProbeUntil&&now>g_displayProbeUntil+1000&&now>g_liveUntil&&g_target->view)
                ReleaseTarget();
        }
        if(g_composite&&g_composite->pipeline){
            std::lock_guard lock(g_lock);
            CompositeRefreshGuard();
            SharedGpuTickGuard();
        }
        if(auto action=g_targetRequest.exchange(0)) {
            std::lock_guard lock(g_lock);
            ++g_target->requested;
            ProcessTargetRequest(scene,action);
            ++g_target->completed;
        }
    }
    g_sceneTick(scene,description);
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        std::lock_guard lock(g_lock);g_nativeThreads->update=GetCurrentThreadId();
        // Keep native scene time, asset transfers and world subscriptions current
        // even when GPU draws are throttled. The update description is a delta.
        if(g_target->view&&g_world->ownedBound&&!g_target->error)
            TickOwned(description);
    }
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        std::lock_guard lock(g_lock);
        const auto now=GetTickCount64();
        if(now-g_gameplayHudTick.load()<250&&(now<g_liveUntil||now<g_displayProbeUntil)&&now-g_ownedRenderTick>=100&&g_world->ownedBound)
            g_renderOnce=true;
    }
    if(!lease||!g_world||reinterpret_cast<uint64_t>(scene)!=g_world->scene||
       !g_renderOnce.load()||g_ownedPrepared.load()) return;
    std::lock_guard lock(g_lock);
    if(!g_target->view||!g_world->ownedBound||g_target->error) return;
    PrepareOwned();
}
void RenderOwnedOnce(void* context,void* input) {
    __try {
        // Native readiness owns world-dependent allocations and clears the
        // dirty flag only once its resource dependencies are available.
        // Readiness and all world allocations were completed before BeginFrame.
        alignas(16) uint8_t result[0x40]{},target[0x28]{},mapped[0x420]{};
        At<ImageView>(target,0)=g_target->color;
        At<uint32_t>(target,0x18)=0; // Engine color conversion into the output target.
        target[0x1c]=7;At<uint64_t>(target,0x20)=0x400;
        // Same descriptor convention as the engine's model-preview output.
        // Preserve the original self-relative render-input address.
        std::memcpy(mapped,input,sizeof(mapped));mapped[0]=g_ownedRenderTick==0;
        // A diagnostic camera stays at its initial pose while the player moves.
        // Portal-selected rendering below still uses the transformed live pose.
        if(GetTickCount64()<g_displayProbeUntil){
            if(!g_probePoseValid){std::memcpy(g_probePose.data(),mapped+0x38,g_probePose.size());g_probePoseValid=true;}
            std::memcpy(mapped+0x38,g_probePose.data(),g_probePose.size());
        }
        bool destination=false;
        if(GetTickCount64()<g_liveUntil) {
            std::array<uint8_t,0x420> pose{};
            if(!MapCamera(&pose,input,&g_livePortal,g_liveField)) return;
            std::memcpy(mapped,pose.data(),sizeof(mapped));destination=true;
            mapped[0]=g_renderedPortal!=g_livePortal.portalId||g_renderedField!=g_liveField;
            std::memcpy(g_destinationPose.data(),mapped+0x38,g_destinationPose.size());
            g_destinationPoseValid=true;
        } else if(g_destinationPoseValid&&GetTickCount64()<g_displayProbeUntil) {
            // Looking away must not send the terrain mesher back to the local
            // diagnostic camera. Keep warming the last destination pose during
            // this bounded probe; map/menu gating still pauses rendering.
            std::memcpy(mapped+0x38,g_destinationPose.data(),g_destinationPose.size());
            mapped[0]=0;destination=true;
        }
        if(g_forceCameraCut){mapped[0]=1;g_forceCameraCut=false;}
        FixedPos focus{};std::memcpy(&focus,mapped+0x38,sizeof(focus));
        ToFloat(focus,g_modelFocus.x,g_modelFocus.y,g_modelFocus.z);g_haveModelFocus=math::Finite(g_modelFocus);
        RenderWithExitPlane(result,context,input,target,mapped,destination);
        ++g_ownedRenderCalls;g_ownedRenderTick=GetTickCount64();
        g_ownedRenderFlags=At<uint32_t>(result,0);
        if(destination&&(g_ownedRenderFlags&1)){g_renderedPortal=g_livePortal.portalId;g_renderedField=g_liveField;++g_mappedFrames;}
        else if(!destination)g_renderedPortal=0;
        char message[100]{};
        std::snprintf(message,sizeof(message),"Town Portal: owned render returned flags=0x%x",g_ownedRenderFlags);
        if(g_ownedRenderCalls<4)greeneye::Host()->log(message);
    } __except(TargetFault(GetExceptionInformation())) {}
}
// Snapshots (render thread). RequestSnapshot queues one. For a few frames the world is drawn
// from the snapshot camera instead of the player's, then the scene colour of the last one is
// stored BEFORE the upscaler (SnapshotCapture from UpscaleHook), then a cut back. The game's
// anti-aliasing blends in earlier frames moved by the player's camera motion, not the
// snapshot camera swapped in here, and camera cuts did not stop it: every snapshot taken
// after it had a shifted copy of the scene (2026-10-27). The capture shader smooths the raw
// frame's edges instead. Every snapshot frame is still a cut, and the hold lets streaming
// and exposure settle on the new view.
constexpr uint32_t kSnapshotSettleFrames=0xffffffffu;
struct SnapshotRequest {
    PortalEndpoint end{};
    float distance{};
    bool shroud{};
    uint64_t notBefore{},deadline{},firstTick{};
    uint32_t frames{};
    snapshot::CaptureSettle settle{};
    bool active{},posed{};
    uint8_t pose[0x28]{};          // render input +38: position (fixed point), rotation
    float alignment{};             // how squarely the camera's rotation mapped its forward axis
    bool viewRotation{};           // the rotation was world-to-camera
    homecache::Key home{};         // MarkHomeSnapshot: saved for the next login once taken
    uint64_t bodyHiddenAt{};       // when the game first hid the character for this capture
};
// The game hides carried equipment only when there is some: with nothing equipped its hook never
// runs. Once the body has been hidden this long without it, there is nothing to hide (a capture
// used to wait for it and time out, 2026-10-28: "equipment hidden 0" with an empty hotbar).
constexpr uint64_t kNoEquipmentMs=500;
SnapshotRequest g_snapshotRequest{};
bool g_snapshotAreaReady{};
uint64_t g_snapshotAreaTick{};
// While a snapshot is pending the local player's character is hidden, so it is never in the
// picture. The game hides it by itself when the camera comes within a distance threshold
// (system local_player_hide, RVA 0x382ac0: Hide[1] = threshold > camera distance; the
// renderer then clears the character's render masks; carried weapon entities
// have their own local_visual_equipment_hide system (handled below). Evidence:
// C:\temp\enshrouded-re\out\hideplayer). The threshold comes from one small function
// (0.6 m) called from one place; the mod redirects that call and asks for a huge threshold
// meanwhile. The next tick after the snapshot recomputes it from the real camera.
using ThresholdFn=float (*)();
ThresholdFn g_hideThresholdNext{};
using EquipmentHideFn=uint8_t (*)(void*,uint8_t);
EquipmentHideFn g_equipmentHideNext{};
std::atomic<bool> g_hidePlayer{false};
std::atomic<bool> g_bodyHideSeen{false},g_equipmentHideSeen{false};
float HideThresholdHook() {
    greeneye::Lease lease;
    const float native=g_hideThresholdNext();
    if(lease&&g_hidePlayer.load()){
        g_bodyHideSeen=true;
        return 1e30f;
    }
    return native;
}
uint8_t EquipmentHideHook(void* camera,uint8_t mode) {
    greeneye::Lease lease;
    // Preserve the original/chained call and both arguments in every state.
    // Returning 1 asks the game's local-equipment system to hide its linked
    // weapon entities. Once capture ends, its own normal value restores them.
    const uint8_t native=g_equipmentHideNext(camera,mode);
    if(lease&&g_hidePlayer.load()){
        g_equipmentHideSeen=true;
        return 1;
    }
    return native;
}
bool g_snapshotCutBack{};
bool g_snapshotRendering{};        // includes the restore frame until its draw has returned
bool g_snapshotHolding{};          // this frame shows the snapshot camera: no portals in it
std::atomic<bool> g_snapshotCaptureNow{false};
// Quaternions (x, y, z, w): Hamilton product, and rotating a vector.
void QuatMul(const float* a,const float* b,float* out) {
    out[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    out[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    out[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    out[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
void QuatRotate(const float* q,const float* v,float* out) {
    const float p[4]={v[0],v[1],v[2],0},conj[4]={-q[0],-q[1],-q[2],q[3]};
    float t[4],u[4];QuatMul(q,p,t);QuatMul(t,conj,u);
    out[0]=u[0];out[1]=u[1];out[2]=u[2];
}
// The snapshot camera: the endpoint's centre height, distance metres out along its normal,
// looking along the normal tipped kSnapshotPitch down, level side to side. Built from the
// player's camera: which of its own axes are forward and up is read from it (it stores
// camera-to-world, logged as "camera rotation"; world-to-camera is handled too).
constexpr float kSnapshotPitch=-0.14f; // radians, about 8 degrees down
// The snapshot camera's vertical field of view (render input +78; the game's own is about 65
// degrees): wide, so the portal still shows the other side when seen from close or aslant.
constexpr float kSnapshotFov=1.92f;    // 110 degrees
bool SnapshotPose(const void* input,SnapshotRequest& r) {
    __try {
        auto* state=At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0xa0);
        const float* view=&At<float>(state,0x1000);
        const float* projection=&At<float>(state,0x10c0);
        // Forward: the world direction along which clip w grows.
        float f[3]{};
        for(int j=0;j<3;++j)for(int k=0;k<3;++k)f[j]+=projection[k*4+3]*view[j*4+k];
        if(!std::isfinite(f[0])||!std::isfinite(f[2])||f[0]*f[0]+f[2]*f[2]<1e-6f)return false;
        float q[4]{};std::memcpy(q,static_cast<const uint8_t*>(input)+0x50,sizeof(q));
        const float length=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
        if(!std::isfinite(length)||std::fabs(length-1)>0.02f)return false;
        const float fl=std::sqrt(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
        for(float& v:f)v/=fl;
        const float conj[4]={-q[0],-q[1],-q[2],q[3]};
        float toCamera[3],fromCamera[3];
        QuatRotate(conj,f,toCamera);  // if q is camera-to-world
        QuatRotate(q,f,fromCamera);   // if q is world-to-camera
        auto axis=[](const float* v){return std::max(std::fabs(v[0]),std::max(std::fabs(v[1]),std::fabs(v[2])));};
        r.viewRotation=axis(fromCamera)>axis(toCamera);
        r.alignment=r.viewRotation?axis(fromCamera):axis(toCamera);
        if(r.alignment<0.97f)return false;
        // The camera's own forward and up axes (L, U), from its camera-to-world rotation.
        const float toWorld[4]={r.viewRotation?-q[0]:q[0],r.viewRotation?-q[1]:q[1],r.viewRotation?-q[2]:q[2],q[3]};
        const float toLocal[4]={-toWorld[0],-toWorld[1],-toWorld[2],toWorld[3]};
        const float worldUp[3]={0,1,0};
        float l[3],u[3];QuatRotate(toLocal,f,l);QuatRotate(toLocal,worldUp,u);
        auto snap=[](float* v){int k=0;for(int i=1;i<3;++i)if(std::fabs(v[i])>std::fabs(v[k]))k=i;
            const float sign=v[k]<0?-1.f:1.f;v[0]=v[1]=v[2]=0;v[k]=sign;return k;};
        if(snap(l)==snap(u))return false;
        // Targets: forward along the normal tipped down, up perpendicular to it.
        const float cp=std::cos(kSnapshotPitch),sp=std::sin(kSnapshotPitch);
        const float F[3]={r.end.nx*cp,sp,r.end.nz*cp},Up[3]={-r.end.nx*sp,cp,-r.end.nz*sp};
        auto cross=[](const float* a,const float* b,float* o){o[0]=a[1]*b[2]-a[2]*b[1];o[1]=a[2]*b[0]-a[0]*b[2];o[2]=a[0]*b[1]-a[1]*b[0];};
        float side[3],localSide[3];cross(F,Up,side);cross(l,u,localSide);
        // R = [F Up side] [l u localSide]^T maps l to F and u to Up.
        float m[3][3]{};
        for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i][j]=F[i]*l[j]+Up[i]*u[j]+side[i]*localSide[j];
        float world[4];
        const float trace=m[0][0]+m[1][1]+m[2][2];
        if(trace>0){const float t=std::sqrt(trace+1)*2;world[3]=t/4;world[0]=(m[2][1]-m[1][2])/t;world[1]=(m[0][2]-m[2][0])/t;world[2]=(m[1][0]-m[0][1])/t;}
        else if(m[0][0]>m[1][1]&&m[0][0]>m[2][2]){const float t=std::sqrt(1+m[0][0]-m[1][1]-m[2][2])*2;world[3]=(m[2][1]-m[1][2])/t;world[0]=t/4;world[1]=(m[0][1]+m[1][0])/t;world[2]=(m[0][2]+m[2][0])/t;}
        else if(m[1][1]>m[2][2]){const float t=std::sqrt(1+m[1][1]-m[0][0]-m[2][2])*2;world[3]=(m[0][2]-m[2][0])/t;world[0]=(m[0][1]+m[1][0])/t;world[1]=t/4;world[2]=(m[1][2]+m[2][1])/t;}
        else{const float t=std::sqrt(1+m[2][2]-m[0][0]-m[1][1])*2;world[3]=(m[1][0]-m[0][1])/t;world[0]=(m[0][2]+m[2][0])/t;world[1]=(m[1][2]+m[2][1])/t;world[2]=t/4;}
        const float rotated[4]={r.viewRotation?-world[0]:world[0],r.viewRotation?-world[1]:world[1],r.viewRotation?-world[2]:world[2],world[3]};
        const FixedPos fixed=ToFixed(r.end.wx+r.end.nx*r.distance,r.end.wy+1.62f,r.end.wz+r.end.nz*r.distance);
        std::memcpy(r.pose,&fixed,sizeof(fixed));
        std::memcpy(r.pose+sizeof(fixed),rotated,sizeof(rotated));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool InFrameEnabled(uint64_t now);
// Whether this frame renders with *out: the snapshot camera, or the cut back to the player's.
bool SnapshotFrame(const void* input,uint8_t* out,uint64_t now) {
    auto& r=g_snapshotRequest;
    g_hidePlayer=r.active;
    const auto& c=*g_composite;
    if(r.active&&now>=r.notBefore){
        if(now>r.deadline||!c.capturePipeline||!c.snapshotTicket||c.error||g_target->error||!InFrameEnabled(now)){
            r.active=false;
            char line[320]{};
            std::snprintf(line,sizeof(line),"Town Portal: snapshot skipped; capture unavailable or area did not settle in time (previous image kept; "
                "%u frames, area ready %d %llu ms ago, body hidden %d, equipment hidden %d, pipeline %d, error %u/%u)",
                r.frames,g_snapshotAreaReady,static_cast<unsigned long long>(now-std::min(now,g_snapshotAreaTick)),
                g_bodyHideSeen.load(),g_equipmentHideSeen.load(),c.capturePipeline&&c.snapshotTicket,c.error,g_target->error);
            greeneye::Host()->log(line);
        }
        else if(!r.posed&&!(r.posed=SnapshotPose(input,r)))r.active=false;
        if(r.active){
            std::memcpy(out,input,0x420);
            std::memcpy(out+0x38,r.pose,sizeof(r.pose));
            At<float>(out,0x78)=kSnapshotFov;
            out[0]=r.frames<kSnapshotSettleFrames;
            if(!r.frames++)r.firstTick=now;
            if(g_bodyHideSeen.load()&&!r.bodyHiddenAt)r.bodyHiddenAt=now;
            const bool equipment=g_equipmentHideSeen.load()||(r.bodyHiddenAt&&now-r.bodyHiddenAt>=kNoEquipmentMs);
            const bool ready=g_snapshotAreaReady&&g_bodyHideSeen.load()&&equipment;
            if(r.settle.Frame(now,ready,g_snapshotAreaTick))g_snapshotCaptureNow=true;
            g_snapshotHolding=true;g_snapshotCutBack=true;
            return true;
        }
    }
    if(g_snapshotCutBack){
        std::memcpy(out,input,0x420);out[0]=1;g_snapshotCutBack=false;
        return true;
    }
    return false;
}
void SnapshotCapture(void* scope,uint8_t* state,ImageView frame,const ImageView& sceneColor,float exposure) {
    auto& c=*g_composite;
    auto& r=g_snapshotRequest;
    r.active=false; // one attempt either way
    const uint64_t now=GetTickCount64();
    if(!g_snapshotAreaReady||now<g_snapshotAreaTick||now-g_snapshotAreaTick>snapshot::kReadinessMaxAgeMs)return;
    if(!c.capturePipeline||!c.snapshotTicket||!g_world)return;
    if(state!=WorldState())return;
    // The scene colour before anti-aliasing (see SnapshotFrame).
    const auto upscaled=InspectImage(frame);
    frame=sceneColor;
    const auto info=InspectImage(frame);
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x123c50)(&depth,state);
    const auto depthInfo=InspectImage(depth);
    // The capture pairs exact colour/depth texels; a different-size image is
    // not the matching scene buffer and would bake displaced silhouettes.
    if(!info.valid||!depthInfo.valid||info.width!=depthInfo.width||info.height!=depthInfo.height||
       !(exposure>0)||!std::isfinite(exposure))return;
    // The same viewpoint's slot (camera within 1.5 m, facing the same way), else an empty
    // one, else the oldest.
    const math::Vec3 origin{r.end.wx+r.end.nx*r.distance,r.end.wy+1.62f,r.end.wz+r.end.nz*r.distance};
    int slot=-1;
    for(int i=0;i<kSnapshots&&slot<0;++i){
        const auto& s=c.snapshots[i];
        const float dx=s.cx-origin.x,dy=s.cy-origin.y,dz=s.cz-origin.z;
        if(s.valid&&dx*dx+dy*dy+dz*dz<1.5f*1.5f&&s.nx*r.end.nx+s.nz*r.end.nz>0.9f)slot=i;
    }
    if(slot<0){
        slot=0;
        for(int i=0;i<kSnapshots;++i){
            if(!c.snapshots[i].valid){slot=i;break;}
            if(c.snapshots[i].tick<c.snapshots[slot].tick)slot=i;
        }
    }
    auto& snapshot=c.snapshots[slot];
    if(!snapshot.image.context)return;
    const float* view=&At<float>(state,0x1000);
    const float* projection=&At<float>(state,0x10c0);
    const math::Vec3 right{r.end.rx,0,r.end.rz},up{0,1,0},normal{r.end.nx,0,r.end.nz};
    float local[16]{};
    if(!math::PortalLocalFromScreen(view,projection,origin,right,up,normal,local))return;
    // Keep the middle of the wide frame: about 70 degrees either side and 55 up and down.
    const float cropX=std::min(1.f,2.75f*std::fabs(projection[0])),cropY=std::min(1.f,1.43f*std::fabs(projection[5]));
    if(!(cropX>0.05f&&cropY>0.05f))return;
    // World to the snapshot's clip x, y (scaled to the stored part) and w: projection * view.
    double clip[12]{};
    for(int col=0;col<4;++col){
        double v[4]{};
        for(int k=0;k<4;++k)v[k]=view[col*4+k];
        double out[4]{};for(int row=0;row<4;++row)for(int k=0;k<4;++k)out[row]+=double(projection[k*4+row])*v[k];
        clip[col]=out[0]/cropX;clip[4+col]=out[1]/cropY;clip[8+col]=out[3];
    }
    for(double value:clip)if(!std::isfinite(value))return;
    Snapshot taken{};
    std::memcpy(taken.clip,clip,sizeof(clip));
    taken.x=r.end.wx;taken.y=r.end.wy;taken.z=r.end.wz;taken.nx=r.end.nx;taken.nz=r.end.nz;
    // The capture stores depth along the camera: its w row in the capture's window space.
    float rows[12]{};
    {
        PortalEndpoint at=r.end;
        at.wx+=r.end.nx*(r.distance-kWindowStart);at.wz+=r.end.nz*(r.distance-kWindowStart);
        SnapshotRows(taken,at,rows);
    }
    float vertices[3*kVertexFloats]{};
    const float xy[3][2]={{-1,-1},{-1,3},{3,-1}};
    for(int i=0;i<3;++i){
        float* v=vertices+i*kVertexFloats;v[0]=xy[i][0];v[1]=xy[i][1];
        std::memcpy(v+8,local,sizeof(local));std::memcpy(v+40,rows+8,4*sizeof(float));v[44]=cropX;v[45]=cropY;
    }
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&frame,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&depth,7,&access);
    const uint8_t attachment[]={0,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6ba30)(scope,&snapshot.image,attachment,"town portal snapshot");
    Native<void(*)(void*,uint64_t)>(0x47770)(scope,0x7ffffffff);
    alignas(8) uint8_t bindings[3][0x28]{};
    for(int i=0;i<2;++i){bindings[i][0]=3;At<ImageView>(bindings[i],8)=i?depth:frame;bindings[i][0x18]=7;}
    bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
    alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
    auto* graph=At<uint8_t*>(scope,0);
    void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x1010700)(At<void*>(graph,0),At<void*>(graph,0x10),set);
    auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=c.capturePipeline;
    command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
    bool drawn=false;
    if(auto* mapped=Native<float*(*)(void*,void*,int)>(0x146470)(scope,c.vertices,3)){
        std::memcpy(mapped,vertices,sizeof(vertices));drawn=true;
    }
    Native<void(*)(void*)>(0x6c050)(scope);
    if(!drawn)return;
    g_snapshotShroud[slot]=r.shroud;
    taken.image=snapshot.image;taken.cx=origin.x;taken.cy=origin.y;taken.cz=origin.z;
    taken.exposure=exposure;taken.tick=GetTickCount64();taken.valid=true;
    snapshot=taken;
    ++c.captures;
    if(homecache::Valid(r.home))HomeExport(slot,taken.tick,r.home);
    // Which way the camera really looked, against the way it was meant to.
    float f[3]{};
    for(int j=0;j<3;++j)for(int k=0;k<3;++k)f[j]+=projection[k*4+3]*view[j*4+k];
    const float fh=std::sqrt(f[0]*f[0]+f[2]*f[2]);
    const float facing=fh>1e-6f?(f[0]*r.end.nx+f[2]*r.end.nz)/fh:0.f;
    char line[320]{};
    std::snprintf(line,sizeof(line),"Town Portal: portal snapshot %d taken after %u frames (frame %ux%u format %u; upscaled %ux%u format %u; exposure %.4g, field of view %.0f degrees, keeps %.2f x %.2f; facing %.2f, %s rotation %.3f)",
                  slot,r.frames,info.width,info.height,info.format,upscaled.width,upscaled.height,upscaled.format,exposure,
                  2.f*std::atan(1.f/std::fabs(projection[5]))*57.2958f,cropX,cropY,facing,r.viewRotation?"view":"camera",r.alignment);
    greeneye::Host()->log(line);
}
void SnapshotCaptureGuard(void* scope,uint8_t* state,const ImageView& frame,const ImageView& sceneColor,float exposure) {
    __try {SnapshotCapture(scope,state,frame,sceneColor,exposure);}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;g_snapshotRequest.active=false;}
}
// In-frame drawing: the portal is recorded into the game's own frame, into its HDR scene
// colour at render resolution, just before the upscaler (FSR / DLSS) or the final
// resolve, so the game anti-aliases its edges against characters, hair and grass like any
// other scene content. Evidence: C:\temp\enshrouded-re\out\pre-taa (renderView call
// 0x13b0dd to 0x118610: rcx renderView's recording scope, rdx state+0xf38, r8 the
// upscaler inputs). On unless greeneye\town-portal\inframe.disabled exists; then portals
// are drawn over the finished frame.
using UpscaleFn=void* (*)(void*,uint8_t*,uint8_t*);
UpscaleFn g_upscaleNext{};
std::atomic<bool> g_inFrameDrawn{false};
bool InFrameEnabled(uint64_t now) {
    static uint64_t checked=0;static bool enabled=true;
    if(now-checked>1000){
        checked=now;
        wchar_t folder[MAX_PATH]{},path[MAX_PATH]{};
        GetModuleFileNameW(nullptr,folder,MAX_PATH);
        if(auto* slash=wcsrchr(folder,L'\\'))slash[1]=0;
        wcscat_s(folder,L"greeneye\\town-portal\\");
        wcscpy_s(path,folder);wcscat_s(path,L"inframe.disabled");
        enabled=GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES;
#ifdef TOWN_PORTAL_DEV
        // Dev tuning: inframe.tune may hold surface brightness, light strength, contrast,
        // saturation, ground-light gain.
        wcscpy_s(path,folder);wcscat_s(path,L"inframe.tune");
        g_inFrameBrightness=kInFrameBrightness;g_inFrameLight=kInFrameLight;g_inFrameContrast=kInFrameContrast;g_inFrameSaturation=kInFrameSaturation;
        g_groundLightGain=kGroundLightGain;
        if(FILE* f=_wfopen(path,L"r")){
            float v[5]{};
            const int n=std::fscanf(f,"%f %f %f %f %f",&v[0],&v[1],&v[2],&v[3],&v[4]);
            if(n>=1&&v[0]>0&&v[0]<50)g_inFrameBrightness=v[0];
            if(n>=2&&v[1]>=0&&v[1]<50)g_inFrameLight=v[1];
            if(n>=3&&v[2]>0&&v[2]<8)g_inFrameContrast=v[2];
            if(n>=4&&v[3]>=1&&v[3]<8)g_inFrameSaturation=v[3];
            if(n>=5&&v[4]>=0&&v[4]<=32)g_groundLightGain=v[4];
            std::fclose(f);
        }
#endif
    }
    return enabled;
}
bool InFrameDraw(void* scope,uint8_t* state,uint8_t* inputs) {
    auto& c=*g_composite;
    if(!c.enabled||!c.framePipeline||c.error||!g_target||g_target->error||!g_world)return false;
    if(state!=WorldState())return false;
    const auto now=GetTickCount64();
    if(!InFrameEnabled(now))return false;
    CompositeSlot slots[kCompositeSlots]{};
    const uint32_t count=CompositeGather(slots,now);
    if(!count)return false;
    // The scene colour the upscaler reads (state+0x140). Depth comes from the selector the
    // engine's transparent and lighting passes use: the state holds two depth images that
    // swap every frame, and the upscaler's own depth entry is not always the current one.
    const ImageView color=At<ImageView>(inputs,0x10);
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x123c50)(&depth,state);
    const auto target=InspectImage(color),depthInfo=InspectImage(depth);
    if(!target.valid||target.format!=0x61||!depthInfo.valid||
       target.width!=depthInfo.width||target.height!=depthInfo.height){++c.inFrameSkip;return false;}
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&g_target->color,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&depth,7,&access);
    for(const auto& snapshot:c.snapshots)
        if(snapshot.valid)Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&snapshot.image,7,&access);
    if(g_sharedGpu)for(const auto& v:g_sharedGpu->views)
        if(v.snapshot.valid&&v.epoch==sharing::Epoch())
            Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cba0)(scope,&v.snapshot.image,7,&access);
    // Load and store: blend over what the scene drew.
    const uint8_t attachment[]={1,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6ba30)(scope,&color,attachment,"town portal");
    // Snapshots are scaled from their exposure to this frame's (inputs +9c: 1/exposure).
    const float invExposure=At<float>(inputs,0x9c);
    CompositeRecord(scope,state,depth,true,slots,count,now,std::isfinite(invExposure)&&invExposure>0?invExposure:0.f);
    ++c.inFrameFrames;c.inFrameTick=now;c.tick=now;++c.frames;
    c.portal=slots[0].portal.portalId;c.field=slots[0].field;
    return true;
}
bool InFrameDrawGuard(void* scope,uint8_t* state,uint8_t* inputs) {
    __try {return InFrameDraw(scope,state,inputs);}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;return false;}
}
std::atomic<float> g_frameExposure{0.f};
void* UpscaleHook(void* scope,uint8_t* upscalers,uint8_t* inputs) {
    greeneye::Lease lease;
    if(lease&&!g_snapshotHolding){
        // WorldState: null once the scene is destroyed (an unguarded read of the stale scene
        // here crashed the game at exe+0x13b0e2 on leaving a world, 2026-10-28).
        std::lock_guard lock(g_lock);
        if(auto* state=WorldState();state&&upscalers-0xf38==state){
            const float exposure=At<float>(inputs,0xa0);
            if(std::isfinite(exposure)&&exposure>0)g_frameExposure=exposure;
        }
    }
    // A snapshot frame shows the world without portals.
    if(lease&&!g_snapshotHolding&&g_composite&&g_target&&g_target->ticket){
        std::lock_guard lock(g_lock);
        if(InFrameDrawGuard(scope,upscalers-0xf38,inputs))g_inFrameDrawn=true;
    }
    // The snapshot from the scene colour before the upscaler / anti-aliasing runs (it writes
    // its result back in place), so no earlier frame is blended in.
    if(lease&&g_snapshotHolding&&g_snapshotCaptureNow.exchange(false)&&g_composite&&g_target){
        const ImageView sceneColor=At<ImageView>(inputs,0x10);
        const float exposure=At<float>(inputs,0xa0);
        std::lock_guard lock(g_lock);
        SnapshotCaptureGuard(scope,upscalers-0xf38,sceneColor,sceneColor,exposure);
    }
    if(lease&&g_world&&g_composite&&g_sharedGpu){
        std::lock_guard lock(g_lock);
        if(auto* state=WorldState();state&&upscalers-0xf38==state)SharedGpuRecordGuard(scope);
    }
    return g_upscaleNext(scope,upscalers,inputs);
}

void* RenderHook(void* result,void* scene,void* context,void* input,void* target) {
    greeneye::Lease lease;
    std::array<uint8_t,0x420> mappedInput{};
    void* actualInput=input;
    bool snapshotFrame=false;
    if (lease) {
        ++g_calls;
        if(g_world&&g_composite&&g_target&&reinterpret_cast<uint64_t>(scene)==g_world->scene){
            std::lock_guard lock(g_lock);
            snapshotFrame=SnapshotFrame(input,mappedInput.data(),GetTickCount64());
            if(snapshotFrame)g_snapshotRendering=true;
        }
        if (snapshotFrame) {
            actualInput=mappedInput.data();
        } else if (GetTickCount64()<g_previewUntil.load()) {
            PortalInstance portal{};bool field{};
            {
                std::lock_guard lock(g_lock);
                portal=g_previewPortal;field=g_previewField;
            }
            if (MapCamera(&mappedInput,input,&portal,field)) {
                actualInput=mappedInput.data();
                ++g_previewFrames;
                g_wasPreview=true;
            }
        } else if (g_wasPreview) {
            std::memcpy(mappedInput.data(),input,mappedInput.size());
            mappedInput[0]=1;
            actualInput=mappedInput.data();
            g_wasPreview=false;
        }
        if (g_requested.load()&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
            Capture capture{};
            if (Copy(&capture,scene,context,actualInput,target)) {
                std::lock_guard lock(g_lock);
                g_capture=capture;
                g_nativeThreads->render=GetCurrentThreadId();
                g_requested=false;
            }
        }
    }
    if(lease&&g_ownedPrepared.load()&&reinterpret_cast<uint64_t>(scene)==g_world->scene&&g_renderOnce.exchange(false)) {
        std::lock_guard lock(g_lock);
        g_ownedPrepared=false;
        if(g_target&&g_target->view&&g_world->ownedBound&&!g_target->error)
            RenderOwnedOnce(context,input);
    }
    g_inFrameDrawn=false;
    void* returned=actualInput!=input?CallWithCamera(g_next,result,scene,context,input,target,mappedInput.data()):
        g_next(result,scene,context,input,target);
    const bool holding=g_snapshotHolding;
    if(snapshotFrame){
        std::lock_guard lock(g_lock);
        g_snapshotHolding=false;g_snapshotCaptureNow=false;g_snapshotRendering=false;
    }
    // Drawn inside the frame already (or a snapshot frame): no overlay this frame.
    if(lease&&!holding&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene&&g_target->ticket&&!g_inFrameDrawn.exchange(false)){
        std::lock_guard lock(g_lock);
        CompositeRenderGuard(scene,context);
    }
    return returned;
}
void Hex(const uint8_t* data,size_t n,char* out) {
    constexpr char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;++i) { out[i*2]=digits[data[i]>>4];out[i*2+1]=digits[data[i]&15]; }
    out[n*2]=0;
}
void AttachmentJson(const Attachment& a,char* out,size_t capacity) {
    std::snprintf(out,capacity,
        "{\"valid\":%s,\"context\":\"0x%llx\",\"id\":\"0x%llx\",\"width\":%u,\"height\":%u,\"format\":%u,\"usage\":%u}",
        a.valid?"true":"false",static_cast<unsigned long long>(a.image.context),
        static_cast<unsigned long long>(a.image.id),a.width,a.height,a.format,a.usage);
}
}
bool Init(const GreeneyeHost* host) {
    // Layout probes must fail closed on an unverified executable, even if a
    // function signature happens to survive a game update.
    auto* module=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!build::Client()) {
        host->log("Town Portal scene probe: unsupported engine build; native research disabled");
        return false;
    }
    const uint8_t registerBytes[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x4c,0x89,0x4c,0x24,0x20,0x4c};
    const uint8_t unregisterBytes[]={0x85,0xd2,0x0f,0x84,0xd9,0,0,0,0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83};
    if(std::memcmp(module+0x11878d0,registerBytes,sizeof(registerBytes))||std::memcmp(module+0x118ae80,unregisterBytes,sizeof(unregisterBytes))) return false;
    g_registerImage=reinterpret_cast<RegisterImageFn>(module+0x11878d0);
    g_unregisterImage=reinterpret_cast<UnregisterImageFn>(module+0x118ae80);
    const uint8_t importImageBytes[]={0x40,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xd1,0x48,0x81,0xec};
    if(std::memcmp(module+0x1012f90,importImageBytes,sizeof(importImageBytes)))return false;
    g_importImage=reinterpret_cast<ImportImageFn>(module+0x1012f90);
    const uint8_t freeImageBytes[]={0x48,0x8b,0xc4,0x55,0x57,0x48,0x81,0xec,0x88,0x0,0x0,0x0,0x48,0x83,0x3a,0x0};
    if(std::memcmp(module+0x1014dc0,freeImageBytes,sizeof(freeImageBytes)))return false;
    g_freeImage=reinterpret_cast<FreeImageFn>(module+0x1014dc0);
    g_output=static_cast<ImageView*>(host->keep("portal-imported-output-v1",sizeof(ImageView)));
    if(!g_output)return false;
    const uint8_t bindImageBytes[]={0x41,0xf,0xb7,0x40,0x8,0x4c,0x8b,0x81,0x78,0x1,0x0,0x0,0x48,0x8b,0x49,0x8};
    if(std::memcmp(module+0x101a570,bindImageBytes,16))return false;
    g_bindImage=reinterpret_cast<BindImageFn>(module+0x101a570);
    g_display=static_cast<DisplayState*>(host->keep("portal-native-ui-v1",sizeof(DisplayState)));
    if(!g_display)return false;
    g_auxDisplays=static_cast<DisplayState*>(host->keep("portal-diagnostic-images-v1",2*sizeof(DisplayState)));
    if(!g_auxDisplays)return false;
    g_target=static_cast<TargetState*>(host->keep("portal-native-target-v1",sizeof(TargetState)));
    g_nativeBase=module;
    // Kept across hot reloads by name: change the name whenever CompositeState's layout
    // changes, or a reload reads the previous generation's fields at the wrong offsets.
    // v3: frame and light pipelines, in-frame counters. v4: snapshots, 48-float vertices.
    // v5: snapshots keep their camera projection. v6: shader fingerprint (shader changes
    // alone no longer need a new name: they rebuild the pipelines). v7: 52-float vertices.
    g_composite=static_cast<CompositeState*>(host->keep("portal-gpu-composite-v7",sizeof(CompositeState)));
    if(!g_composite)return false;
    g_sharedGpu=static_cast<SharedGpuState*>(host->keep("portal-shared-snapshot-gpu-v1",sizeof(SharedGpuState)));
    if(!g_sharedGpu)return false;
    g_snapshotCaptureHash=static_cast<uint32_t*>(host->keep("portal-snapshot-capture-hash-v1",sizeof(uint32_t)));
    if(!g_snapshotCaptureHash)return false;
    g_world=static_cast<WorldTemplate*>(host->keep("portal-owned-world-v1",sizeof(WorldTemplate)));
    g_mirrors=static_cast<MirrorState*>(host->keep("portal-owned-instances-v2",sizeof(MirrorState)));
    g_instanceCreate=reinterpret_cast<InstanceCreateFn>(host->find(signatures::instanceCreate,nullptr));
    g_instanceRemove=reinterpret_cast<InstanceRemoveFn>(host->find(signatures::instanceRemove,nullptr));
    g_instanceTransform=reinterpret_cast<InstanceTransformFn>(host->find(signatures::instanceTransform,nullptr));
    if(!g_mirrors||!g_instanceCreate||!g_instanceRemove||!g_instanceTransform)return false;
    g_nativeThreads=static_cast<NativeThreads*>(host->keep("portal-native-threads-v1",sizeof(NativeThreads)));
    g_scope=reinterpret_cast<ScopeFn>(host->find(signatures::scope,nullptr));
    g_plan=reinterpret_cast<PlanFn>(host->find(signatures::plan,nullptr));
    g_image=reinterpret_cast<ImageFn>(host->find(signatures::image,nullptr));
    g_commit=reinterpret_cast<CommitFn>(host->find(signatures::commit,nullptr));
    g_end=reinterpret_cast<EndFn>(host->find(signatures::end,nullptr));
    g_release=reinterpret_cast<ReleaseFn>(host->find(signatures::release,nullptr));
    g_dimensions=reinterpret_cast<DimensionsFn>(host->find(signatures::dimensions,nullptr));
    g_sceneReady=reinterpret_cast<ReadyFn>(host->find(signatures::sceneReady,nullptr));
    g_sceneIbl=reinterpret_cast<BindFn>(host->find(signatures::sceneIbl,nullptr));
    if(reinterpret_cast<void*>(g_sceneIbl)!=module+0x1100d0)return false;
    g_terrainRequest=reinterpret_cast<TerrainRequestFn>(host->find(signatures::terrainRequest,nullptr));
    if(reinterpret_cast<void*>(g_terrainRequest)!=module+0x10a8a30)return false;
    if(!g_target||!g_scope||!g_plan||!g_image||!g_commit||!g_end||!g_release||!g_dimensions) {
        host->log("Town Portal: independent target signatures unavailable");
        g_target=nullptr;
    }
    if(!g_world||!g_target||!g_sceneReady||!g_nativeThreads) return false;
    auto* rootEntry=host->find(signatures::rootCreate,nullptr);
    auto* createEntry=host->find(signatures::viewCreate,nullptr);
    auto* bindEntry=host->find(signatures::worldBind,nullptr);
    auto* destroyEntry=host->find(signatures::viewDestroy,nullptr);
    auto* tickEntry=host->find(signatures::sceneTick,nullptr);
    auto* projectionEntry=host->find(signatures::projectionDescriptor,nullptr);
    auto* projectionMatrixEntry=host->find(signatures::projectionMatrix,nullptr);
    auto* stateEntry=host->find(signatures::stateCreate,nullptr);
    if(!stateEntry)return false;
    // 15 bytes: three non-relative stack saves, verified against build 1273043.
    g_stateCreate=reinterpret_cast<StateCreateFn>(host->hook(stateEntry,reinterpret_cast<void*>(StateCreateHook),15,nullptr,0));
    if(!g_stateCreate)return false;
    if(!rootEntry||!createEntry||!bindEntry||!destroyEntry||!tickEntry||!projectionEntry||!projectionMatrixEntry) return false;
    g_projectionCaller=module+0x12af6c;
    // First 15 bytes end at movaps [rsp+20],xmm6; no relative operands.
    g_projectionDesc=reinterpret_cast<ProjectionDescFn>(host->hook(projectionEntry,reinterpret_cast<void*>(ProjectionDescHook),15,nullptr,0));
    if(!g_projectionDesc)return false;
    // Verified 16-byte prologue ends at lea rbp,[rax-38], no relative operands.
    g_projectionMatrix=reinterpret_cast<ProjectionMatrixFn>(host->hook(projectionMatrixEntry,reinterpret_cast<void*>(ProjectionMatrixHook),16,nullptr,0));
    if(!g_projectionMatrix)return false;
    // All five spans were disassembled from the pinned executable. Each ends
    // before a relative branch/load and contains only relocatable instructions.
    g_rootCreate=reinterpret_cast<CreateFn>(host->hook(rootEntry,reinterpret_cast<void*>(RootCreateHook),15,nullptr,0));
    g_viewCreate=reinterpret_cast<CreateFn>(host->hook(createEntry,reinterpret_cast<void*>(ViewCreateHook),13,nullptr,0));
    g_worldBind=reinterpret_cast<BindFn>(host->hook(bindEntry,reinterpret_cast<void*>(WorldBindHook),14,nullptr,0));
    g_viewDestroy=reinterpret_cast<DestroyFn>(host->hook(destroyEntry,reinterpret_cast<void*>(ViewDestroyHook),12,nullptr,0));
    g_sceneTick=reinterpret_cast<BindFn>(host->hook(tickEntry,reinterpret_cast<void*>(SceneTickHook),16,nullptr,0));
    if(!g_rootCreate||!g_viewCreate||!g_worldBind||!g_viewDestroy||!g_sceneTick) return false;
    // Build 1273043: instruction boundaries verified offline;
    // the first 15 bytes contain no RIP-relative instructions or branches.
    auto* entry=host->find("4C 89 44 24 18 55 53 56 57 41 55 41 56 41 57 48 8D AC 24 10 EF FF FF B8 F0 11 00 00",nullptr);
    if (!entry) { host->log("Town Portal scene probe: verified render entry missing");return false; }
    g_next=reinterpret_cast<RenderFn>(host->hook(entry,reinterpret_cast<void*>(RenderHook),15,nullptr,0));
    host->log(g_next?"Town Portal scene probe: native render observer active; camera preview is opt-in":"Town Portal scene probe: hook unavailable");
    // Optional: renderView's call into the upscaler (0x13b0dd -> 0x118610). Without it
    // portals are drawn over the finished frame.
    if(host->redirectCall){
        if(auto* found=host->find("00 00 83 BF 80 02 00 00 20 0F 94 85 64 4C 00 00 E8 ?? ?? ?? ??",nullptr)){
            auto* site=found+16;int32_t delta=0;std::memcpy(&delta,site+1,4);
            if(host->generation>1||site+5+delta==module+0x118610)
                g_upscaleNext=reinterpret_cast<UpscaleFn>(host->redirectCall(site,reinterpret_cast<void*>(UpscaleHook)));
        }
    }
    host->log(g_upscaleNext?"Town Portal: portals drawn inside the frame, before anti-aliasing":"Town Portal: in-frame portal drawing unavailable; portals drawn over the frame");
    // local_player_hide: movss xmm6,[rax+14a4] / subss xmm6,[rax+1494] / call threshold.
    if(host->redirectCall){
        if(auto* found=host->find("F3 0F 10 B0 A4 14 00 00 F3 0F 5C B0 94 14 00 00 E8 ?? ?? ?? ??",nullptr)){
            auto* site=found+16;int32_t delta=0;std::memcpy(&delta,site+1,4);
            const uint8_t* target=site+5+delta;
            if(host->generation>1||(target[0]==0xF3&&target[1]==0x0F&&target[2]==0x10&&target[3]==0x05))
                g_hideThresholdNext=reinterpret_cast<ThresholdFn>(host->redirectCall(site,reinterpret_cast<void*>(HideThresholdHook)));
        }
    }
    host->log(g_hideThresholdNext?"Town Portal: snapshots hide the player's character":"Town Portal: player hiding unavailable; snapshots may show the character");
    if(host->redirectCall){
        if(auto* found=host->find(snapshot::hide::equipmentCall,nullptr)){
            auto* site=found+snapshot::hide::callOffset;
            int32_t delta=0;std::memcpy(&delta,site+1,4);
            const auto* target=host->find(snapshot::hide::equipmentTarget,nullptr);
            if(site==module+snapshot::hide::callRva&&site[0]==0xe8&&
               target==module+snapshot::hide::targetRva&&
               (host->generation>1||site+5+delta==target))
                g_equipmentHideNext=reinterpret_cast<EquipmentHideFn>(host->redirectCall(site,reinterpret_cast<void*>(EquipmentHideHook)));
        }
    }
    host->log(g_equipmentHideNext?"Town Portal: snapshots also hide the local player's carried equipment":"Town Portal: equipment hiding unavailable; new snapshots disabled");
    return g_next!=nullptr;
}
bool Json(char* out,uint32_t capacity) {
    if (!g_next||!out||capacity<8192) return false;
    Capture capture{};
    TargetState targetState{};
    WorldTemplate world{};
    NativeThreads threads{};
    TerrainProgress terrainProgress{};uint32_t isolationFaults{};uint64_t isolationTick{};
    {
        std::lock_guard lock(g_lock);
        capture=g_capture;
        if(g_target) targetState=*g_target;
        if(g_world) world=*g_world;
        if(g_nativeThreads) threads=*g_nativeThreads;
        terrainProgress=g_terrainProgress;isolationFaults=g_isolationFaults;isolationTick=g_isolationTick;
    }
    char input[0x841]{},target[0x51]{},scene[0x1f1]{};
    Hex(capture.input.data(),capture.input.size(),input);
    Hex(capture.target.data(),capture.target.size(),target);
    Hex(capture.sceneData.data(),capture.sceneData.size(),scene);
    std::snprintf(out,capacity,
        "{\"calls\":%llu,\"tick\":%llu,\"previewFrames\":%llu,\"previewActive\":%s,\"thread\":%u,\"width\":%u,\"height\":%u,\"scene\":\"0x%llx\",\"context\":\"0x%llx\",\"inputHex\":\"%s\",\"targetHex\":\"%s\",\"sceneHex\":\"%s\",\"offscreen\":{\"ready\":%s,\"color\":\"0x%llx\",\"depth\":\"0x%llx\",\"view\":\"0x%llx\",\"width\":%u,\"height\":%u,\"ticket\":%u,\"error\":%u,\"allocations\":%u,\"releases\":%u,\"completed\":%u}}",
        static_cast<unsigned long long>(g_calls.load()),static_cast<unsigned long long>(capture.tick),
        static_cast<unsigned long long>(g_previewFrames.load()),GetTickCount64()<g_previewUntil.load()?"true":"false",
        capture.thread,capture.width,capture.height,static_cast<unsigned long long>(capture.scene),
        static_cast<unsigned long long>(capture.context),input,target,scene,
        targetState.ticket&&!targetState.error?"true":"false",
        static_cast<unsigned long long>(targetState.color.id),static_cast<unsigned long long>(targetState.depth.id),
        reinterpret_cast<unsigned long long>(targetState.view),targetState.width,targetState.height,targetState.ticket,
        targetState.error,targetState.allocations,targetState.releases,targetState.completed);
    char depth0[200]{},depth1[200]{},intermediate[200]{};
    AttachmentJson(capture.depth[0],depth0,sizeof(depth0));
    AttachmentJson(capture.depth[1],depth1,sizeof(depth1));
    AttachmentJson(capture.intermediate,intermediate,sizeof(intermediate));
    char output[200]{};AttachmentJson(InspectImage(targetState.color),output,sizeof(output));
    size_t probeOffset=std::strlen(out);
    const auto now=GetTickCount64();
    std::snprintf(out+probeOffset-1,capacity-probeOffset+1,
        ",\"probe\":{\"remainingMs\":%llu,\"terrainReady\":%s,\"output\":%s}}",
        static_cast<unsigned long long>(g_displayProbeUntil>now?g_displayProbeUntil-now:0),
        (g_ownedRenderFlags&0x80)?"true":"false",output);
    size_t nativeOffset=std::strlen(out);
    if(g_composite){
        std::lock_guard lock(g_lock);
        const auto& c=*g_composite;
        std::snprintf(out+nativeOffset-1,capacity-nativeOffset+1,
            ",\"composite\":{\"enabled\":%s,\"stage\":%u,\"error\":%u,\"frames\":%llu,\"tick\":%llu,\"image\":%u,\"pipeline\":\"0x%llx\",\"inFrameFrames\":%llu,\"inFrameTick\":%llu,\"inFrameSkip\":%u,\"lightPipeline\":%s,\"requestedSlots\":%u,\"renderedCount\":%u,\"visualVerified\":false}}",
            c.enabled?"true":"false",c.stage,c.error,c.frames,c.tick,c.display.image,reinterpret_cast<uint64_t>(c.pipeline),
            c.inFrameFrames,c.inFrameTick,c.inFrameSkip,c.lightPipeline?"true":"false",g_compositeSlotCount,c.renderedCount);
        nativeOffset=std::strlen(out);
    }
    std::snprintf(out+nativeOffset-1,capacity-nativeOffset+1,
        ",\"native\":{\"metadataOnly\":false,\"experimentalRenderer\":true,\"liveDestinationRendered\":false,\"depthCompositingActive\":false,\"modelScene\":\"0x%llx\",\"cameraState\":\"0x%llx\",\"sceneCapacity\":%llu,\"sceneCount\":%llu,\"renderWidth\":%u,\"renderHeight\":%u,\"depth\":[%s,%s],\"intermediate\":%s}}",
        static_cast<unsigned long long>(capture.modelScene),static_cast<unsigned long long>(capture.cameraState),
        static_cast<unsigned long long>(capture.sceneCapacity),static_cast<unsigned long long>(capture.sceneCount),
        capture.renderWidth,capture.renderHeight,depth0,depth1,intermediate);
    size_t ownedOffset=std::strlen(out);
    std::snprintf(out+ownedOffset-1,capacity-ownedOffset+1,
        ",\"owned\":{\"reservedSlots\":%u,\"templateCaptured\":%s,\"worldBindingCaptured\":%s,\"worldBound\":%s,\"creations\":%u,\"destructions\":%u,\"renderCalls\":%llu,\"renderTick\":%llu,\"renderFlags\":%u}}",
        world.reservedSlots,world.scene?"true":"false",world.worldBound?"true":"false",
        world.ownedBound?"true":"false",world.creations,world.destructions,
        static_cast<unsigned long long>(g_ownedRenderCalls),static_cast<unsigned long long>(g_ownedRenderTick),g_ownedRenderFlags);
    if(g_faultIp) {
        size_t n=std::strlen(out);
        std::snprintf(out+n-1,capacity-n+1,",\"fault\":{\"ip\":\"0x%llx\",\"address\":\"0x%llx\",\"cx\":\"0x%llx\",\"dx\":\"0x%llx\"}}",
            static_cast<unsigned long long>(g_faultIp),static_cast<unsigned long long>(g_faultAddress),
            static_cast<unsigned long long>(g_faultCx),static_cast<unsigned long long>(g_faultDx));
    }
    size_t threadsOffset=std::strlen(out);
    std::snprintf(out+threadsOffset-1,capacity-threadsOffset+1,
        ",\"lifecycleThreads\":{\"create\":%u,\"bind\":%u,\"update\":%u,\"render\":%u,\"destroy\":%u},\"bindingEnabled\":true,\"renderEnabled\":true}",
        threads.create,threads.bind,threads.update,threads.render,threads.destroy);
    size_t displayOffset=std::strlen(out);
    std::snprintf(out+displayOffset-1,capacity-displayOffset+1,",\"display\":{\"registry\":\"0x%llx\",\"image\":%u,\"mappedFrames\":%llu,\"portal\":%llu}}",reinterpret_cast<unsigned long long>(g_display?g_display->registry:nullptr),g_display?g_display->image:0,static_cast<unsigned long long>(g_mappedFrames),static_cast<unsigned long long>(g_renderedPortal));
    size_t clipOffset=std::strlen(out);
    std::snprintf(out+clipOffset-1,capacity-clipOffset+1,",\"exitClip\":{\"applied\":%llu,\"rejected\":%llu,\"visualVerified\":false}}",
        static_cast<unsigned long long>(g_exitClipFrames.load()),static_cast<unsigned long long>(g_exitClipRejected.load()));
    size_t mirrorOffset=std::strlen(out);
    std::snprintf(out+mirrorOffset-1,capacity-mirrorOffset+1,",\"sharedWorld\":%s,\"instances\":{\"owned\":%u,\"eligible\":%u,\"created\":%u,\"removed\":%u,\"updated\":%u,\"unchanged\":%u,\"limit\":256,\"radius\":160,\"selection\":\"nearest\",\"animatedActors\":false}}",
        "false",
        g_mirrors?g_mirrors->count:0,g_mirrors?g_mirrors->eligible:0,g_mirrors?g_mirrors->created:0,g_mirrors?g_mirrors->removed:0,
        g_mirrors?g_mirrors->updated:0,g_mirrors?g_mirrors->unchanged:0);
    g_requested=true;
    size_t terrainOffset=std::strlen(out);
    std::snprintf(out+terrainOffset-1,capacity-terrainOffset+1,
        ",\"isolation\":{\"checked\":%s,\"faults\":%u,\"tick\":%llu},\"terrainRequests\":{\"refreshes\":%llu,\"tick\":%llu,\"calls\":%u,\"blockTouches\":%u,\"limit\":256,\"radius\":160,\"requestedMasks\":[%u,%u],\"readyMasks\":[%u,%u],\"rejected\":%u,\"snapshotSkipped\":%u,\"refreshUs\":%.2f,\"maxRefreshUs\":%.2f,\"focus\":[%.3f,%.3f,%.3f],\"roots\":%u,\"pendingUploads\":%u,\"pendingCompletions\":%u,\"workerBusy\":%u,\"nodesReady\":%u,\"meshReady\":%u}}",
        isolationTick?"true":"false",isolationFaults,isolationTick,
        terrainProgress.refreshes,terrainProgress.tick,terrainProgress.requests,terrainProgress.blockTouches,
        terrainProgress.requested[0],terrainProgress.requested[1],terrainProgress.ready[0],terrainProgress.ready[1],
        terrainProgress.rejected,terrainProgress.skipped,terrainProgress.refreshUs,terrainProgress.maxRefreshUs,
        terrainProgress.focus.x,terrainProgress.focus.y,terrainProgress.focus.z,terrainProgress.roots,
        terrainProgress.pendingUploads,terrainProgress.pendingCompletions,terrainProgress.workerBusy,terrainProgress.nodesReady,terrainProgress.meshReady);
    SharedGpuStatus(out,capacity);
    return capture.tick!=0;
}
bool TargetRequest(uint32_t action) {
    if(!g_target||action<7||action>12) return false;
    // Development-only operations: admission and execution happen on the
    // native update thread before BeginFrame; success is not visual validation.

    if(action>=9) {
        std::lock_guard lock(g_lock);
        if(!g_world||g_world->reservedSlots<4||!g_world->scene) return false;
        if(action>=10&&(!g_target->view||!g_world->worldBound||g_target->error)) return false;
        if(action>=11&&!g_world->ownedBound) return false;
    }
    uint32_t empty=0;
    return g_targetRequest.compare_exchange_strong(empty,action);
}
static uint32_t RegisterProbeImage(void* registry,uint64_t now,uint32_t source=0);
bool ProbeActive(uint64_t now) {
    std::lock_guard lock(g_lock);
    return now<g_displayProbeUntil;
}
void GameplayHud(bool playing,uint64_t now) {g_gameplayHudTick=playing?now:0;}
uint32_t LiveImage(const PortalInstance& portal,bool fieldSide,void* registry,uint64_t now) {
    // Rendering probes are opt-in through MCP. The output has not passed
    // visual validation, so normal portals must keep the reliable fallback.
    if(!g_target||!g_world||!g_display||!registry)return 0;
    std::lock_guard lock(g_lock);
    if(now>=g_displayProbeUntil)return 0;
    if(!g_target->view||!g_world->ownedBound||g_target->error)return 0;
    if(g_liveSelectTick!=now){g_livePortal=portal;g_liveField=fieldSide;g_liveSelectTick=now;g_liveUntil=now+250;}
    if(portal.portalId!=g_renderedPortal||fieldSide!=g_renderedField||!g_target->color.context||
       (g_ownedRenderFlags&0x7ff)!=0x7ff)return 0;
    return RegisterProbeImage(registry,now);
}
uint32_t ProbeImage(void* registry,uint64_t now,uint32_t source) {
    std::lock_guard lock(g_lock);
    return RegisterProbeImage(registry,now,source);
}
uint32_t CompositeImage(const PortalInstance& portal,bool field,uint32_t tint,void* registry,uint64_t now) {
    std::lock_guard lock(g_lock);
    if(!g_composite||!g_target||g_target->error||!g_world||!g_world->scene)return 0;
    // Ask the render thread for this endpoint (nearest first, two at most).
    if(g_compositeSlotTick!=now){g_compositeSlotTick=now;g_compositeSlotCount=0;}
    bool listed=false;
    for(uint32_t i=0;i<g_compositeSlotCount;++i)
        listed|=g_compositeSlots[i].portal.portalId==portal.portalId&&g_compositeSlots[i].field==field;
    if(!listed&&g_compositeSlotCount<kCompositeSlots)g_compositeSlots[g_compositeSlotCount++]={portal,field,tint};
    // The first visible portal allocates the target (render thread, before BeginFrame).
    if(!g_target->ticket&&CompositeAllowed()){
        uint32_t empty=0;g_targetRequest.compare_exchange_strong(empty,7);
        return 0;
    }
    auto& c=*g_composite;
    // The UI timestamp may precede the render thread finishing its current
    // frame while this call waits for g_lock. That is fresh, not an unsigned
    // age wraparound: falling back here draws tiles over the same portal.
    if(!c.enabled||c.error||!c.frames||(now>c.tick&&now-c.tick>250)||!registry)return 0;
    bool rendered=false;
    for(uint32_t i=0;i<c.renderedCount;++i)rendered|=c.renderedPortal[i]==portal.portalId&&c.renderedField[i]==field;
    if(!rendered)return 0;
    if(c.inFrameTick==c.tick)return kDrawnInFrame;
    if(!c.display.image){
        c.display.image=g_registerImage(registry,&c.image,0,(uint64_t(1440)<<32)|2560,1);
        c.display.registry=registry;
    }
    if(c.display.registry==registry&&c.display.image&&At<uint32_t>(registry,0x35304))
        g_bindImage(At<void*>(registry,0x10),At<uint32_t>(registry,0x35300)+(c.display.image&0xfffff),&c.image);
    return c.display.registry==registry?c.display.image:0;
}
static uint32_t RegisterProbeImage(void* registry,uint64_t now,uint32_t sourceIndex) {
    if(now>=g_displayProbeUntil||!g_target||!g_target->view||!g_display||!registry||
       g_target->error||(g_ownedRenderFlags&0x7ff)!=0x7ff)return 0;
    // Intermediate attachments can be recycled by later passes. Sample only
    // the persistent, separately allocated destination output.
    auto source=InspectImage(g_target->color);
    DisplayState* display=g_display;
    if(sourceIndex>0&&sourceIndex<=2){
        auto* view=sourceIndex==1?g_target->view:reinterpret_cast<void*>(g_world->scene);
        auto* resolution=At<void*>(view,0xe8);
        if(!resolution)return 0;
        source=InspectImage(At<ImageView>(resolution,8));
        display=&g_auxDisplays[sourceIndex-1];
    }
    if(!source.valid)return 0;
    if(!display->image){
        display->image=g_registerImage(registry,&source.image,0,(uint64_t(source.height)<<32)|source.width,1);
        display->registry=registry;
    }
    if(display->registry==registry&&display->image&&At<uint32_t>(registry,0x35304))
        g_bindImage(At<void*>(registry,0x10),At<uint32_t>(registry,0x35300)+(display->image&0xfffff),&source.image);
    return display->registry==registry?display->image:0;
}
bool RequestSnapshot(const PortalEndpoint& end,float distance,uint64_t notBefore,bool shroud) {
    if(!g_composite||!g_target||!g_upscaleNext||!HidesPlayer())return false;
    std::lock_guard lock(g_lock);
    if(!g_composite->capturePipeline||!g_composite->snapshotTicket||g_composite->error||
       g_snapshotRequest.active||g_snapshotCutBack||g_snapshotRendering)return false;
    g_snapshotRequest={};
    g_snapshotRequest.end=end;g_snapshotRequest.distance=distance;g_snapshotRequest.shroud=shroud;
    g_snapshotRequest.notBefore=notBefore;g_snapshotRequest.deadline=notBefore+snapshot::kCaptureLimitMs;
    g_snapshotRequest.active=true;
    g_bodyHideSeen=false;g_equipmentHideSeen=false;
    g_hidePlayer=true;
    return true;
}
void MarkHomeSnapshot(const homecache::Key& key) {
    std::lock_guard lock(g_lock);
    if(g_snapshotRequest.active)g_snapshotRequest.home=key;
}
int UploadSnapshot(std::shared_ptr<sharing::Image> image,const char* what) {
    std::lock_guard lock(g_lock);
    return CachedHomeStep(std::move(image),what);
}
float FrameExposure() {return g_frameExposure.load();}
bool MissingSnapshot(uint64_t& portalId,PortalEndpoint& destination,uint64_t after) {
    std::lock_guard lock(g_lock);
    if(!g_missing.portalId||g_missing.tick<=after||GetTickCount64()-g_missing.tick>500)return false;
    portalId=g_missing.portalId;destination=g_missing.destination;
    return true;
}
void SnapshotAreaReady(bool ready,uint64_t now) {
    std::lock_guard lock(g_lock);
    g_snapshotAreaReady=ready;g_snapshotAreaTick=now;
}
void CancelSnapshot() {
    std::lock_guard lock(g_lock);
    g_snapshotRequest.active=false;
    g_snapshotCaptureNow=false;
    g_hidePlayer=false;
}
void SetCast(uint64_t portalId,float x,float y,float z,int effect) {
    std::lock_guard lock(g_lock);
    g_cast={portalId,{x,y,z},effect};
}
bool HidesPlayer() { return g_hideThresholdNext&&g_equipmentHideNext; }
bool SnapshotsReady() {
    if(!g_composite||!g_target||!g_upscaleNext||!HidesPlayer()||!CompositeAllowed())return false;
    std::lock_guard lock(g_lock);
    if(g_composite->snapshotTicket&&g_composite->capturePipeline)return true;
    if(!g_target->ticket&&g_world&&g_world->scene){uint32_t empty=0;g_targetRequest.compare_exchange_strong(empty,7);}
    return false;
}
bool SnapshotBusy() {
    std::lock_guard lock(g_lock);
    return g_snapshotRequest.active||g_snapshotCutBack||g_snapshotRendering;
}
bool Preview(const PortalInstance& portal,bool fieldSide,uint32_t durationMs) {
    if (!g_next||!portal.active) return false;
    std::lock_guard lock(g_lock);
    g_previewPortal=portal;
    g_previewField=fieldSide;
    g_requested=true;
    g_previewUntil=GetTickCount64()+std::min(durationMs,3000u);
    return true;
}
}
