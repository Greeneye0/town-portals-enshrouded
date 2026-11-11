#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "portal_build.h"
#include "portal_natives.h"
#include "portal_quality.h"
#include "portal_scene_probe.h"
#include "portal_snapshot_share.h"
#include "portal_snapshot_native.h"
#include "portal_snapshot_timing.h"
#include "portal_snapshot_hide.h"
#include "portal_default_view.h"
#include "portal_easter.h"
#include "portal_water.h"
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
#include <dxgi.h>
#include <shlobj.h>
#pragma comment(lib,"dxgi.lib")
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
// nearDistance: when above 0, the view's near plane is moved out to that distance instead of
// the oblique clip at plane (see ProjectionMatrixHook).
struct ClipScope {bool active{};math::Plane plane{};float nearDistance{};};
thread_local ClipScope g_clipScope{};
std::atomic<uint64_t> g_exitClipFrames{},g_exitClipRejected{};
bool g_exitClipEnabled=true,g_forceCameraCut{};
struct NativeSpan {const void* data;uint64_t count;};
using InstanceCreateFn=uint32_t (*)(void*,const void*,const void*);
using InstanceRemoveFn=void (*)(void*,const NativeSpan*);
using InstanceTransformFn=void (*)(void*,const NativeSpan*,const NativeSpan*,uint8_t);
InstanceCreateFn g_instanceCreate{};InstanceRemoveFn g_instanceRemove{};
InstanceTransformFn g_instanceTransform{};
// memory: the remembered object it was made from (slot + 1), or 0 for one mirrored from the world.
struct MirrorRecord {uint32_t source{},owned{};uint64_t guid[2]{};uint8_t transform[56]{};uint32_t memory{};};
// Remembered objects: the static objects (crates, furniture, ...) the game has shown around the
// player this session, kept by place. The game only holds the objects near the player, so the
// second camera looking at a far place has none of its own; it is given the remembered ones
// (SyncOwnedInstances). An open-addressed table in a block the loader keeps over reloads.
// used: 0 empty; 1 seen; 2 seen again in the same place five seconds or more later (only these
// are shown: the tools a character carries are static objects too, and every spot they were
// seen at came out as a tool hanging in the air, 2026-11-09); 3 gone (the slot stays taken so
// later ones in its chain are still found). first, last: seconds (GetTickCount64 / 1000).
struct RememberedObject {uint64_t guid[2];uint8_t transform[56];float x,y,z;uint32_t used,first,last;};
constexpr uint32_t kRememberSlots=16384;
struct ObjectMemory {uint32_t count,pad;RememberedObject slots[kRememberSlots];};
ObjectMemory* g_objectMemory{};
math::Vec3 g_playerCamera{};bool g_havePlayerCamera{};
struct MirrorState {MirrorRecord records[256]{};uint64_t tick{};uint32_t count{},eligible{},created{},removed{},updated{},unchanged{};};
MirrorState* g_mirrors{};
// Effects (fire, smoke, sparks) twinned into the second camera's scene (SyncOwnedEffects).
constexpr uint32_t kEffects=512; // 64 was used up by the fireflies round a base before the altar flame got its turn
struct EffectRecord {uint32_t source,owned;};
struct EffectState {EffectRecord records[kEffects];uint32_t count,created,removed,refused;};
EffectState* g_effects{};
// Animated things mirrored into the second camera's scene (SyncOwnedActors).
constexpr uint32_t kActorParts=24,kActors=96; // 40 until 2.0.1: a base with many residents filled it and the player was not twinned (user, 2026-11-10)
struct ActorPart {uint32_t source,owned;uint8_t transform[56];};
struct ActorRecord {uint32_t sourceGroup,ownedGroup,parts,pad;uint64_t guid[2];ActorPart part[kActorParts];};
thread_local bool g_inPlayerRender{}; // set while the player's own view is rendered (RenderHook)
bool g_vfxPassAll{}; // all three effect pass calls are taken (far effects can be drawn without fog)
struct ActorState {ActorRecord records[kActors];uint32_t count,created,removed,posed;};
ActorState* g_actors{};
math::Vec3 g_modelFocus{};bool g_haveModelFocus{};
std::atomic<bool> g_requested{true};
std::atomic<uint64_t> g_calls{};
// Recursive: the second camera's render runs under this lock and calls back into the hooks
// that take it (UpscaleHook). With a plain mutex that threw inside the native frame recording,
// left its timestamp queries unfinished and froze the game in vkGetQueryPoolResults (2026-11-09).
std::recursive_mutex g_lock;
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
// The second nearest end on screen (LiveImage) and the end this frame is rendered for.
PortalInstance g_livePortal2{},g_nowPortal{};
bool g_liveField2{},g_nowField{};
uint64_t g_liveSecondTick{};
bool g_liveAlternating{},g_liveNoStore{};
uint64_t g_liveUntil{},g_liveSelectTick{},g_renderedPortal{},g_mappedFrames{};
// FarShot: once an end has been live and settled for a while, the second camera holds the
// snapshot camera's pose there for a few frames (the departure snapshot's: 0.4 m out from the
// far end, looking out through it, 110 degrees wide) and the last of them is stored as that
// end's parallax picture, refreshed every couple of minutes it is live. Ends that are not live
// then show that picture plainly instead of a held live frame, which, shot from the viewer's eye
// through a narrow aimed lens, came apart in shreds seen from anywhere else (user's two-player
// screenshot, 2026-11-10). Render thread.
struct FarShot {
    bool active{},captureNow{};uint64_t portalId{};bool field{};PortalInstance portal{};uint64_t startedAt{};
    PortalEndpoint end{};uint32_t frames{};uint8_t pose[0x28]{};
};
FarShot g_farShot;
struct FarShotDone {uint64_t portalId{};bool field{};float x{},z{};uint64_t tick{};};
FarShotDone g_farShotsDone[16];
constexpr float kFarShotDistance=0.4f;     // as a departure snapshot through a field end
constexpr uint32_t kFarShotFrames=4;       // the frame a render gives is for the call before
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
// The game's graphics system has one critical section (+0x710) around its device calls
// (FUN_140fcab30, FUN_140fd3aa0 and others). A fault caught here inside such a call skips the
// game's own release: the 2026-11-09 fault (exe+0xfca9a2, in FUN_140fcab30) left it held by
// the main thread, the texture transfer thread waited on it from then on, and at quit the
// main thread waited for that thread's work for ever (dump of 2026-11-10). So each guarded
// function notes how deep this thread holds it on the way in, and a caught fault lets go of
// what was taken since.
thread_local long t_graphicsLockDepth{};
thread_local bool t_graphicsLockNoted{};
CRITICAL_SECTION* GraphicsLock() {
    __try {
        if(!g_target||!g_target->color.context)return nullptr;
        auto* graphics=*reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(g_target->color.context)+8);
        return graphics?reinterpret_cast<CRITICAL_SECTION*>(graphics+0x710):nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return nullptr;}
}
long GraphicsLockDepth(CRITICAL_SECTION* lock) {
    __try {
        return lock->OwningThread==reinterpret_cast<HANDLE>(uintptr_t(GetCurrentThreadId()))?lock->RecursionCount:0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
void NoteGraphicsLock() {
    auto* lock=GraphicsLock();
    t_graphicsLockNoted=lock!=nullptr;
    t_graphicsLockDepth=lock?GraphicsLockDepth(lock):0;
}
void ReleaseGraphicsLockAfterFault() {
    auto* lock=t_graphicsLockNoted?GraphicsLock():nullptr;
    if(!lock)return;
    int released=0;
    for(long extra=GraphicsLockDepth(lock)-t_graphicsLockDepth;extra>0&&released<8;--extra,++released)LeaveCriticalSection(lock);
    if(released){
        char line[160]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view: the fault left the game's graphics lock held %d deep; released",released);
        greeneye::Host()->log(line);
    }
}
// The same for the game's render memory: its allocator (render root +0, "Game_Render") takes a
// spin lock (+0xf8: 0 free, 0x80000000 held) round every request. A fault caught inside a
// request (a TLSF assert, 2026-11-10) left it held, and the main thread then spun on it for
// good: the game stopped answering. When a fault's stack runs through the allocator's own
// code, the lock is let go.
void ReleaseRenderMemoryLockAfterFault(bool inAllocator) {
    if(!inAllocator||!g_world||!g_world->root)return;
    __try {
        auto* allocator=*reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(g_world->root));
        if(!allocator)return;
        auto* lock=reinterpret_cast<volatile LONG*>(allocator+0xf8);
        if(InterlockedCompareExchange(lock,0,LONG(0x80000000))==LONG(0x80000000))
            greeneye::Host()->log("Town Portal: live view: the fault left the game's render memory locked; released");
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
// Is this address in the render memory allocator's code (its allocate call, vtable slot 1, and
// the functions round it)?
bool InRenderAllocator(uint64_t rip) {
    if(!g_world||!g_world->root)return false;
    __try {
        auto* allocator=*reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(g_world->root));
        if(!allocator)return false;
        const uint64_t allocate=reinterpret_cast<uint64_t>((*reinterpret_cast<void***>(allocator))[1]);
        return rip+0x1000>=allocate&&rip<allocate+0x1000;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
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
    bool inAllocator=false;
    for(int i=0;i<24&&cursor.Rip;i++) {
        inAllocator|=InRenderAllocator(cursor.Rip);
        length+=std::snprintf(report+length,sizeof(report)-length,"%s\"0x%llx\"",i?",":"",cursor.Rip-base);
        DWORD64 imageBase{},establisher{};void* handler{};
        auto entry=RtlLookupFunctionEntry(cursor.Rip,&imageBase,nullptr);
        if(!entry) {cursor.Rip=*reinterpret_cast<DWORD64*>(cursor.Rsp);cursor.Rsp+=8;}
        else RtlVirtualUnwind(UNW_FLAG_NHANDLER,imageBase,cursor.Rip,entry,&cursor,&handler,&establisher,nullptr);
    }
    length+=std::snprintf(report+length,sizeof(report)-length,"]}");
    HANDLE file=CreateFileW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\native-render-fault.json",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
    if(file!=INVALID_HANDLE_VALUE){DWORD written{};WriteFile(file,report,length,&written,nullptr);CloseHandle(file);}
    ReleaseGraphicsLockAfterFault();
    ReleaseRenderMemoryLockAfterFault(inAllocator);
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
// The live view: the second camera stands where the viewer's eye is, carried through the portal
// (MapCameraEnds), and each of its frames is stored like a snapshot: its HDR scene colour and
// depth, before anti-aliasing, drawn into the scratch target with the camera it was taken from.
// The portal then traces it from the viewer's eye as it is now (portal_composite.frag, snap.w 2),
// so the view stays steady between the second camera's frames. Under g_lock.
uint64_t g_liveWantTick{}; // a portal on screen last asked for the live view (under g_lock)
struct LiveView {
    Snapshot snapshot{};
    uint64_t portalId{};bool field{};
    // Armed by RenderOwnedOnce for the frame it is about to record.
    bool armed{};uint64_t armedPortal{};bool armedField{};PortalEndpoint destination{};math::Vec3 camera{},cameraShown{};
    uint64_t captures{},skipped{};
    // Since when this portal and side have been asked for, and whether the far end has loaded
    // for the second camera (every readiness flag set, or four seconds of frames).
    uint64_t since{};bool ready{};
    // This end's own camera matrices as the view state last held them (see LiveCapture).
    float view[16]{},projection[16]{};bool haveMatrices{};
    uint64_t shownSince{};   // when this end's live picture first showed (it fades in from dark)
};
LiveView g_liveView,g_liveView2;
LiveView* g_liveNow=&g_liveView; // the one this frame's render is for
const Snapshot* LiveSnapshot(const PortalInstance& p,bool field,uint64_t now) {
    // The last live picture of an end stays up when the second camera is busy elsewhere or has
    // not caught up (the default picture showed in such moments; user, 2026-11-10): for ten
    // minutes, drawn as a still picture once it is older than 0.4 s (IsLiveSnapshot).
    // tools/live.nokeep (dev) shows it only while fresh, as before.
    // (2026-11-10, later: not wanted after all. A picture that is no longer live is not shown:
    // the swirl, then the live view fades in again. tools/live.keep, dev, holds it as above.)
    // (Later again: held after all, shown wavy as a picture that is waiting. tools/live.nokeep.)
    // (2.0.1: not held. A live frame is shot from the viewer's eye through a narrow aimed lens,
    // and held as a still it showed in shreds from anywhere else; the end's parallax snapshot,
    // taken from the live camera itself (FarShot), shows instead. tools/live.keep, dev, holds it.)
    static uint64_t checked=0;static bool nokeep=true;
    if(now-checked>1000){checked=now;nokeep=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.keep")==INVALID_FILE_ATTRIBUTES;}
    for(const LiveView* l:{&g_liveView,&g_liveView2})
        if(l->snapshot.valid&&l->ready&&l->portalId==p.portalId&&l->field==field&&now-std::min(now,l->snapshot.tick)<=(nokeep?400u:600000u))return &l->snapshot;
    return nullptr;
}
// With the live view on, a portal shows its swirl until the far side is live and then the
// picture comes up from dark over 0.8 s. The old pictures (a default picture of the region, an
// earlier capture) are not put up in between: going from one of those to the live view was a
// jolt (user, 2026-11-10). tools/live.stills (dev) shows them as before.
bool LiveAuto(uint64_t now);
extern std::atomic<bool> g_liveUnavailable; // portal_scene_probe.cpp LiveRoom
std::atomic<bool> g_liveUnavailable{false};
int LiveViewSetting();
bool LiveOnly(uint64_t now) {
    static uint64_t checked=0;static bool stills=false;
    if(now-checked>1000){checked=now;stills=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.stills")!=INVALID_FILE_ATTRIBUTES;}
    // Whenever the live view is on at all (not only once a portal has asked for it: its first
    // frames showed the default picture plain, user 2026-11-10).
    return !stills&&LiveAuto(now)&&g_target&&!g_target->error;
}
bool LiveWaiting(const PortalInstance& p,bool field,uint64_t now) {
    if(!LiveOnly(now)||!g_livePortal.active||g_livePortal.portalId!=p.portalId||g_liveField!=field)return false;
    if(now>g_liveUntil+1000)return false; // not asked for just now
    return !(g_liveView.ready&&g_liveView.portalId==p.portalId&&g_liveView.field==field);
}
float LiveFade(const Snapshot* s,uint64_t now) {
    LiveView* l=s==&g_liveView.snapshot?&g_liveView:s==&g_liveView2.snapshot?&g_liveView2:nullptr;
    if(!l)return 1.f;
    // Not drawn for a while (the second camera was elsewhere): it fades in afresh.
    static uint64_t drawn[2]{};
    uint64_t& last=drawn[l==&g_liveView2];
    if(!l->shownSince||now-std::min(now,last)>500)l->shownSince=now;
    last=now;
    const float t=std::clamp(float(now-std::min(now,l->shownSince))/800.f,0.f,1.f);
    return t*t*(3.f-2.f*t);
}
const Snapshot* InnerPicture() {
    return g_liveNow&&g_liveNow->snapshot.valid?&g_liveNow->snapshot:nullptr;
}
bool IsLiveSnapshot(const Snapshot* s) {
    if(s!=&g_liveView.snapshot&&s!=&g_liveView2.snapshot)return false;
    const uint64_t now=GetTickCount64();
    return now-std::min(now,s->tick)<=400;
}
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
    // Usage 0x07: copied from (readback), copied to (capture, upload), sampled; no render target
    // (see SnapshotScratch). The capture draws into the scratch image, usage 0x4f as before.
    for(auto& snapshot:c.snapshots){
        ImageDesc(desc,kSnapshotWidth,kSnapshotHeight,0x61,0x07);
        g_image(plan,&snapshot.image,desc);
    }
    auto& scratch=*g_snapshotScratch;
    scratch={};
    ImageDesc(desc,kSnapshotWidth,kSnapshotHeight,0x61,0x4f);
    g_image(plan,&scratch.image,desc);
    g_commit(scope,result,&c.snapshotTicket,At<void*>(root,0x160));
    c.snapshotError=result[0];
    g_end(scope);
    bool ready=!c.snapshotError&&c.snapshotTicket&&scratch.image.context;
    for(const auto& snapshot:c.snapshots)ready=ready&&snapshot.image.context;
    if(!ready){
        if(c.snapshotTicket)g_release(At<void*>(root,0x160),&c.snapshotTicket);
        for(auto& snapshot:c.snapshots)snapshot={};
        c.snapshotTicket=0;
        scratch={};
    }
    char line[120]{};
    std::snprintf(line,sizeof(line),ready?"Town Portal: portal snapshots ready (%ux%u)":"Town Portal: portal snapshots unavailable (error %u)",
                  ready?kSnapshotWidth:c.snapshotError,kSnapshotHeight);
    greeneye::Host()->log(line);
}
void AllocateLiveSecond(uint8_t* root) {
    auto& c=*g_composite;
    if(!g_liveSecond||g_liveSecond->ticket||g_liveSecond->error||!c.enabled||!c.snapshotTicket)return;
    alignas(16) uint8_t scope[0x180]{},plan[0x20]{},desc[0x30]{},result[16]{};
    g_scope(scope,At<void*>(root,0x128),0);
    g_plan(plan,scope,At<uint32_t>(root,0x270),0,1);
    ImageDesc(desc,kSnapshotWidth,kSnapshotHeight,0x61,0x4f);
    g_image(plan,&g_liveSecond->image,desc);
    g_commit(scope,result,&g_liveSecond->ticket,At<void*>(root,0x160));
    g_liveSecond->error=result[0];
    g_end(scope);
    if(g_liveSecond->error||!g_liveSecond->ticket||!g_liveSecond->image.context){
        if(g_liveSecond->ticket)g_release(At<void*>(root,0x160),&g_liveSecond->ticket);
        const uint32_t error=g_liveSecond->error?g_liveSecond->error:1;
        *g_liveSecond={};g_liveSecond->error=error;
    }
    greeneye::Host()->log(g_liveSecond->ticket?"Town Portal: live view: a second end can be shown live (the two take turns)":"Town Portal: live view: no image for a second live end");
}
// The second camera's frame: tall, like the portal's opening (2.4 m by 3.3 m), which the live
// view's camera is aimed at and zoomed to fill (AimAtExit).
// As wide as the game renders a portal that fills the middle of the screen, and as tall as the
// game renders at all with an upscaler on a 1440-line screen: close up the visible part of a
// portal is wider than tall, and the earlier 720 x 1008 frame was stretched over it (2026-11-09).
constexpr uint32_t kOwnedWidth=1360,kOwnedHeight=768; // 1920 x 1088 cost too much: 28 frames a second against 37 (2026-11-09)
// The size the second camera really renders at follows the opening on the player's screen.
// kOwnedWidth x kOwnedHeight is the most it gets. A whole portal a few steps away is tall and
// narrow on screen (about 420 x 830 of the game's own render pixels on a 5120 x 1440 screen),
// and the fixed 1360 x 768 frame, zoomed to hold its height, spent over two thirds of its
// pixels beside the opening; the frame cost about 7.6 ms a megapixel (2026-11-09).
// The game has its own way to change a view's render size from frame to frame, its dynamic
// resolution: with the DynamicResolution feature (public bit 29, the view's own bit 26) the
// render function calls FUN_140116710(view state, scale) at the start of each frame, which
// sets the frame's size (state +0x9c, one per frame parity at +0x850) to scale times the most
// (+0xac) and resizes the view's images. The second camera is created with that feature and
// the call is hooked (RenderScaleHook): for this view it gets the size wanted, as the most,
// at scale 1. The picture's shape (the projection's aspect) the game takes from the most
// (+0xac), so the projection descriptor is given the real shape (ProjectionDescHook).
// Calling it from outside the render function on a view made without the feature faulted in
// the image resize (exe+0xfca9a2, 2026-11-09): such a view has one image where a view with
// the feature has one per parity.
// tools\owned.fixed (dev) keeps the full frame; tools\owned.size (dev) holds "width height".
using RenderScaleFn=void(*)(uint8_t* state,float scale);
RenderScaleFn g_renderScale{};
struct OwnedSize {
    uint32_t width{kOwnedWidth},height{kOwnedHeight};   // this frame's
    uint32_t wantWidth{kOwnedWidth},wantHeight{kOwnedHeight};
    uint64_t smallerSince{},changes{};
    float mainDensity{};                                  // the player's view: render pixels per unit of tangent
    bool fixed{},able{};uint32_t forcedWidth{},forcedHeight{};
};
OwnedSize g_ownedSize;
thread_local float g_ownedAspectNow{};                    // set while the second camera renders
float OwnedAspect() {return float(g_ownedSize.width)/float(g_ownedSize.height);}
// The size for an opening that reaches (in the second camera's own frame, as tangents from its
// centre, margin included) reachX to each side and reachY up and down: as many pixels as the
// player's view has over the same angle, in steps of 128, grown at once and shrunk only after
// a second and a half of a clearly smaller need (each change makes the game resize its images).
void ChooseOwnedSize(float reachX,float reachY,float cosine) {
    auto& o=g_ownedSize;
    if(o.fixed||!o.able||!(o.mainDensity>1.f)){o.wantWidth=kOwnedWidth;o.wantHeight=kOwnedHeight;return;}
    if(o.forcedWidth){o.wantWidth=o.forcedWidth;o.wantHeight=o.forcedHeight;return;}
    const float c=std::clamp(cosine,0.45f,1.f);
    // Twice the player's own pixel density: at about one to one the far picture showed as
    // blocks (the portal's shader takes single texels by depth, and the game's upscaler has
    // no sub-pixel movement of the far frame to rebuild from, 2026-11-10). The size costs
    // little (512x768 against 1360x768 was two frames a second).
    const float density=o.mainDensity/(c*c)*2.0f;         // off the screen's centre a pixel covers less angle
    const auto step=[](float pixels,uint32_t least,uint32_t most){
        if(!(pixels>0)||!std::isfinite(pixels))return most;
        return std::clamp<uint32_t>((uint32_t(pixels)+127u)/128u*128u,least,most);
    };
    const uint32_t width=step(2.f*reachX*density,256u,kOwnedWidth),height=step(2.f*reachY*density,256u,kOwnedHeight);
    const uint64_t now=GetTickCount64();
    if(width>o.wantWidth||height>o.wantHeight){
        o.wantWidth=std::max(width,o.wantWidth);o.wantHeight=std::max(height,o.wantHeight);o.smallerSince=0;
    } else if(uint64_t(width)*height*10u<uint64_t(o.wantWidth)*o.wantHeight*8u){
        if(!o.smallerSince)o.smallerSince=now;
        else if(now-o.smallerSince>1500){o.wantWidth=width;o.wantHeight=height;o.smallerSince=0;}
    } else o.smallerSince=0;
}
// Before the second camera's frame: the size it is to render at (RenderScaleHook applies it).
void ApplyOwnedSize(bool live) {
    auto& o=g_ownedSize;
    static uint64_t checked=0;
    if(const uint64_t now=GetTickCount64();now-checked>1000){
        checked=now;
        o.fixed=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\owned.fixed")!=INVALID_FILE_ATTRIBUTES;
        o.forcedWidth=o.forcedHeight=0;
        if(FILE* f=_wfopen(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\owned.size",L"r")){
            unsigned w=0,h=0;
            if(std::fscanf(f,"%u %u",&w,&h)==2&&w>=64&&h>=64&&w<=kOwnedWidth&&h<=kOwnedHeight){o.forcedWidth=w&~1u;o.forcedHeight=h&~1u;}
            std::fclose(f);
        }
    }
    uint32_t width=kOwnedWidth,height=kOwnedHeight;
    auto* state=g_target&&g_target->view?At<uint8_t*>(g_target->view,0xa0):nullptr;
    // Only a view made with the feature, of the size this was written for.
    o.able=g_renderScale&&state&&(At<uint32_t>(state,0x10)&0x4000000u)&&At<uint64_t>(state,0xac)==(uint64_t(kOwnedHeight)<<32|kOwnedWidth);
    if(o.able&&live&&!o.fixed){width=o.forcedWidth?o.forcedWidth:o.wantWidth;height=o.forcedHeight?o.forcedHeight:o.wantHeight;}
    if(width!=o.width||height!=o.height){
        ++o.changes;
        if(o.changes<=20||o.changes%100==0){
            char line[140]{};
            std::snprintf(line,sizeof(line),"Town Portal: live view: second camera frame %ux%u (was %ux%u), change %llu",width,height,o.width,o.height,o.changes);
            greeneye::Host()->log(line);
        }
    }
    o.width=width;o.height=height;
}
// The game's dynamic resolution call (see above). For the second camera's frame (this thread
// is in its render: g_ownedAspectNow) the size chosen stands in for the most, at scale 1.
// Effects that carry something from one frame to the next (the altar's flame: its pieces stood
// still and flat in the far view, 2026-11-10) keep it in ONE table for all effect systems
// (effect manager +0x308 / +0x318, the half chosen by the frame number). Every view's effect
// tick (FUN_1401a5240) first wipes this frame's half (the fill at exe+0x1a559c) and then
// writes its own entries. The second camera is rendered before the player's view, so the
// player's tick wiped the far view's entries every frame. In a frame in which the second
// camera was rendered, the player's tick leaves the wipe out: both views' entries share the
// half (their keys are per effect instance). tools/vfx.wipe (dev) wipes as the game does.
using TableFillFn=void(*)(void* scope,void* buffer,uint64_t offset,uint64_t size,uint32_t value);
TableFillFn g_tableFill{};
// Set by the second camera's render, taken by the player's render that follows it on the same
// thread. (The mod's frame counter advances on the update thread and could step between the
// two renders, so comparing it let the wipe through in most frames: the far view's flame
// stayed frozen, 2026-11-10.)
thread_local bool g_ownedEffectsTicked{};
std::atomic<uint64_t> g_tableWipesSkipped{},g_tableWipes{};
// A table of its own for the far view. What an effect's pieces carry from one frame to the
// next is kept by the effect MANAGER, not by the scene's effect system: two tables (manager
// +0x2e8 / +0x2f8, 0x100000 bytes of 8-byte entries, empty = 0xffffffff) and two payload arrays
// (+0x308 / +0x318, 0x20 bytes an entry), one of each read and the other written by frame
// parity (FUN_1401a5240, the per-view effect tick; its only call is at exe+0x133bb4: out,
// the view's effect part = effect system +0x80, the command scope, the view's parameters).
// Two views ticking in one frame share them, and the far view's altar flame was never drawn
// though its effect ran (2026-11-10). For the far view's tick the four handles (16 bytes
// each: graph pointer, id, extra) are exchanged for a private set and put back after it.
// The private buffers are made as the manager makes its own direct ones (exe+0x198d58:
// FUN_140fd34c0(manager +0x118, {size, usage bits, 0}) then FUN_141029880(out, manager +0x120,
// raw, name)); usage bits 0x12 as the game's tables. Both private tables start empty.
// OFF unless tools/fx.private.on exists: the first far tick with the private set froze the
// game for good (the graphics driver never came back from one call, 2026-11-10), as the
// compute step loops for ever on tables it cannot read as it expects. Not to be switched on
// with a player in the game until the buffers are known to be made as the manager's own.
struct FxPrivate {void* manager;uint8_t handle[4][16];void* raw[4];uint32_t made,filled,error;uint64_t ticks;};
FxPrivate* g_fxPrivate{};
bool g_fxPrivateOn{};
using EffectTickFn=void*(*)(void* out,uint8_t* viewPart,void* scope,void* viewParameters);
EffectTickFn g_effectTick{};
uint32_t g_fxPrivateMode{};
void FxPrivateMake(uint8_t* manager) {
    auto& f=*g_fxPrivate;
    if(f.manager==manager&&(f.made||f.error))return;
    f={};f.manager=manager;
    static const size_t at[4]={0x2e8,0x2f8,0x308,0x318};
    void* device=At<void*>(manager,0x118);void* graph=At<void*>(manager,0x120);
    if(!device||!graph){f.error=1;return;}
    for(int i=0;i<4;++i){
        const uint64_t size=Native<uint64_t(*)(const void*)>(0x1030740)(manager+at[i]);
        if(!size||size>(256ull<<20)){f.error=2;break;}
        alignas(8) uint8_t description[0x20]{};
        At<uint64_t>(description,0)=size;At<uint32_t>(description,8)=0x12u;
        f.raw[i]=Native<void*(*)(void*,const void*)>(0xfd34c0)(device,description);
        if(!f.raw[i]){f.error=3;break;}
        alignas(8) uint8_t out[0x20]{};
        auto* made=Native<uint8_t*(*)(void*,void*,void*,void*)>(0x1029880)(out,graph,f.raw[i],*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))+0x1517588));
        if(!made){f.error=4;break;}
        std::memcpy(f.handle[i],made,16);
        if(Native<uint64_t(*)(const void*)>(0x1030740)(f.handle[i])!=size){f.error=5;break;}
    }
    if(!f.error)f.made=1;
    char line[160]{};
    std::snprintf(line,sizeof(line),f.made?"Town Portal: live view: the far view's effects keep their frame-to-frame tables apart from the player's":"Town Portal: live view: no private effect tables (error %u)",f.error);
    greeneye::Host()->log(line);
}
void* EffectTickPrivate(void* out,uint8_t* viewPart,void* scope,void* viewParameters,uint8_t* manager) {
    auto& f=*g_fxPrivate;
    static const size_t at[4]={0x2e8,0x2f8,0x308,0x318};
    uint8_t saved[4][16];
    // Dev: tools/fx.private.on holds a number: 1 the tables, 2 the payload arrays, 4 both
    // private tables emptied before every tick, 8 the payload arrays zeroed before every tick.
    const uint32_t mode=g_fxPrivateMode;
    for(int i=0;i<4;++i){std::memcpy(saved[i],manager+at[i],16);if(mode&(i<2?1u:2u))std::memcpy(manager+at[i],f.handle[i],16);}
    void* result=nullptr;
    __try {
        if(!f.filled||(mode&4)){
            for(int i=0;i<2;++i)g_tableFill(scope,f.handle[i],0,Native<uint64_t(*)(const void*)>(0x1030740)(f.handle[i]),0xffffffffu);
        }
        if(!f.filled||(mode&8)){
            for(int i=2;i<4;++i)g_tableFill(scope,f.handle[i],0,Native<uint64_t(*)(const void*)>(0x1030740)(f.handle[i]),0u);
        }
        if(!f.filled){f.filled=1;greeneye::Host()->log("Town Portal: fx.private: first far tick with the private set recorded");}
        result=g_effectTick(out,viewPart,scope,viewParameters);
        ++f.ticks;
    } __finally {
        for(int i=0;i<4;++i)std::memcpy(manager+at[i],saved[i],16);
    }
    return result;
}
void* EffectTickHook(void* out,uint8_t* viewPart,void* scope,void* viewParameters) {
    greeneye::Lease lease;
    if(lease&&viewParameters){   // Dev: tools/fx.params -> both views' tick parameters, once each.
        static uint64_t checked=0;static int want=0;
        const uint64_t now=GetTickCount64();
        if(!want&&now-checked>1000){checked=now;if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.params")!=INVALID_FILE_ATTRIBUTES){DeleteFileW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.params");want=3;}}
        const int bit=g_ownedAspectNow>0?1:2;
        if((want&bit)&&!IsBadReadPtr(viewParameters,0x7c0)){
            want&=~bit;
            if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/fx-params.txt",bit==1?"w":"a")){
                std::fprintf(f,"%s part %p params %p:",bit==1?"far":"player",static_cast<void*>(viewPart),viewParameters);
                for(size_t o=0;o<0x7c0;o+=4)std::fprintf(f," %08x",At<uint32_t>(viewParameters,o));
                std::fprintf(f,"%c",10);std::fclose(f);
            }
        }
    }
    if(lease&&g_ownedAspectNow<=0){
        // Dev: tools/fx.noplayer leaves the player's own effect tick out (do the far view's
        // pieces come right when nothing else writes the shared lists in the frame?).
        static uint64_t checked=0;static bool none=false;
        if(const uint64_t now=GetTickCount64();now-checked>500){checked=now;none=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.noplayer")!=INVALID_FILE_ATTRIBUTES;}
        if(none&&out){std::memset(out,0,16);return out;}
    }
    if(lease&&viewPart){   // Dev: how often each view's effect step finds a time step queued (system +0x838).
        static uint64_t toldAt=0;static uint32_t with[2]{},without[2]{},many[2]{};
        const int who=g_ownedAspectNow>0?0:1;
        auto* system=At<uint8_t*>(viewPart,0);
        if(system&&!IsBadReadPtr(system,0x840)){
            const uint32_t queued=At<uint32_t>(system,0x838);
            if(queued==0)++without[who];else if(queued==1)++with[who];else ++many[who];
            if(const uint64_t now=GetTickCount64();now-toldAt>10000){
                toldAt=now;char line[260]{};
                std::snprintf(line,sizeof(line),"Town Portal: effect steps in 10 s: far with a time step %u, without %u, several %u; player's with %u, without %u, several %u; far runtimes %u, player's this call %u",
                    with[0],without[0],many[0],with[1],without[1],many[1],who==0?At<uint32_t>(viewPart,0x5d8):0u,who==1?At<uint32_t>(viewPart,0x5d8):0u);
                greeneye::Host()->log(line);
                with[0]=with[1]=without[0]=without[1]=many[0]=many[1]=0;
                // The list counters: what the step is given (view parameters +0x138..+0x158) against the
                // view state's own (+0xb50..+0xb7c), with each counter's record (offset, last value).
                auto* st=who==0?(g_target&&g_target->view?At<uint8_t*>(g_target->view,0xa0):nullptr):WorldState();
                auto* manager=At<uint8_t*>(system,8);
                auto* table=manager?At<uint8_t*>(manager,0x158):nullptr;
                auto* records=table?At<uint8_t*>(table,0x48):nullptr;
                if(st&&viewParameters&&records){
                    char l2[700]{};int n=std::snprintf(l2,sizeof(l2),"Town Portal: %s list counters: given",who==0?"far":"player's");
                    for(int o=0x138;o<0x158;o+=4)n+=std::snprintf(l2+n,sizeof(l2)-n," %x",At<uint32_t>(viewParameters,o));
                    n+=std::snprintf(l2+n,sizeof(l2)-n,"; state");
                    for(int o=0xb50;o<0xb7c;o+=4){
                        const uint32_t id=At<uint32_t>(st,o);
                        auto* rec=id?records+size_t(id&0xffff)*0x40:nullptr;
                        if(rec&&!IsBadReadPtr(rec,0x40)&&At<uint32_t>(rec,0x38)==id)n+=std::snprintf(l2+n,sizeof(l2)-n," %x(@%x=%x,%x)",id,At<uint32_t>(rec,0x10),At<uint32_t>(rec,0x14),At<uint32_t>(rec,0x1c));
                        else n+=std::snprintf(l2+n,sizeof(l2)-n," %x(-)",id);
                    }
                    greeneye::Host()->log(l2);
                }
            }
        }
    }
    if(lease&&g_ownedAspectNow>0&&viewParameters){
        // Dev: tools/fx.noplanes gives the far view's effect step planes that cut nothing away
        // (each 0,0,0 with a large distance, either sign by the file's first character).
        static uint64_t checked=0;static int planes=0;
        if(const uint64_t now=GetTickCount64();now-checked>500){
            checked=now;planes=0;
            if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/fx.noplanes","r")){const int c=std::fgetc(f);planes=c=='-'?-1:1;std::fclose(f);}
        }
        if(planes)for(int i=0;i<5;++i){
            float* plane=&At<float>(viewParameters,0x40+i*0x10);
            plane[0]=plane[1]=plane[2]=0;plane[3]=planes*1e9f;
        }
    }
    if(!lease||g_ownedAspectNow<=0)return g_effectTick(out,viewPart,scope,viewParameters);
    g_fxPrivateOn=false;
    if(!g_fxPrivate||!g_tableFill||!viewPart||!g_target||!g_target->view)return g_effectTick(out,viewPart,scope,viewParameters);
    static uint64_t checked=0;static bool off=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=true;g_fxPrivateMode=0;
        if(FILE* file=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/fx.private.on",L"r")){
            unsigned m=0;if(std::fscanf(file,"%u",&m)==1&&m){g_fxPrivateMode=m;off=false;}
            std::fclose(file);
        }
    }
    auto* state=At<uint8_t*>(g_target->view,0xa0);
    auto* system=At<uint8_t*>(viewPart,0);
    auto* manager=system?At<uint8_t*>(system,8):nullptr;
    if(off||!state||At<uint8_t*>(state,0x9a8)!=viewPart||!manager)return g_effectTick(out,viewPart,scope,viewParameters);
    FxPrivateMake(manager);
    if(!g_fxPrivate->made)return g_effectTick(out,viewPart,scope,viewParameters);
    g_fxPrivateOn=(g_fxPrivateMode&1)!=0;
    return EffectTickPrivate(out,viewPart,scope,viewParameters,manager);
}
void TableFillHook(void* scope,void* buffer,uint64_t offset,uint64_t size,uint32_t value) {
    greeneye::Lease lease;
    if(lease&&g_ownedAspectNow<=0&&g_ownedEffectsTicked&&g_fxPrivateOn)g_ownedEffectsTicked=false; // its own tables: the game's wipe stands
    if(lease&&g_ownedAspectNow<=0&&g_ownedEffectsTicked){
        g_ownedEffectsTicked=false;
        static uint64_t checked=0;static bool wipe=false;
        if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;wipe=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/vfx.wipe")!=INVALID_FILE_ATTRIBUTES;}
        if(!wipe){++g_tableWipesSkipped;return;}
    }
    ++g_tableWipes;
    g_tableFill(scope,buffer,offset,size,value);
}
void RenderScaleHook(uint8_t* state,float scale) {
    greeneye::Lease lease;
    if(lease&&g_ownedAspectNow>0&&g_ownedSize.able&&state){
        const uint64_t most=At<uint64_t>(state,0xac);
        if(most==(uint64_t(kOwnedHeight)<<32|kOwnedWidth)){
            At<uint64_t>(state,0xac)=uint64_t(g_ownedSize.height)<<32|g_ownedSize.width;
            // The game's own scale for this view (+0xb8, which its controller had brought down
            // to 0.67 to reach 60 frames a second) stays at 1: the size is chosen here, and
            // with the two at odds the far view's grass came out in blocks (2026-11-10).
            At<float>(state,0xb8)=1.f;At<float>(state,0xbc)=0.f;
            g_renderScale(state,1.f);
            At<uint64_t>(state,0xac)=most;
            return;
        }
    }
    g_renderScale(state,scale);
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
    ImageDesc(desc,kOwnedWidth,kOwnedHeight,0x25,0x4c);
    g_image(plan,&g_target->color,desc);
    // The composite image is 2560x1440 whatever the screen size (at least the
    // screen's height on 1080p and 1440p, so outlines are not upscaled);
    // portal_composite.frag divides gl_FragCoord by the same size.
    if(CompositeAllowed()){
        ImageDesc(desc,2560,1440,0x25,0x4c);
        g_image(plan,&g_composite->image,desc);
    }
    ImageDesc(desc,kOwnedWidth,kOwnedHeight,0x7e,0x14);
    g_image(plan,&g_target->depth,desc);
    g_commit(scope,result,&g_target->ticket,At<void*>(root,0x160));
    g_target->error=result[0];
    g_end(scope);
    g_target->root=reinterpret_cast<uint64_t>(root);
    if(!g_target->error&&g_target->ticket&&g_target->color.context&&g_target->depth.context) {
        uint64_t color=g_dimensions(&g_target->color),depth=g_dimensions(&g_target->depth);
        g_target->width=static_cast<uint32_t>(color);
        g_target->height=static_cast<uint32_t>(color>>32);
        if(color!=depth||g_target->width!=kOwnedWidth||g_target->height!=kOwnedHeight) g_target->error=0x10001;
        ++g_target->allocations;
        CompositeCreate();
        AllocateSnapshots(root);
    }
}
// Dev test: tools\tree.gone takes the second camera's brick tree out of its scene for good
// (after it was made with one), until the file goes or the view is destroyed; kept over
// reloads so the tree is always put back before the game frees the scene.
struct GoneTree {uint8_t* scene;void* tree;};
GoneTree* g_goneTree{};
void RestoreGoneTree() {
    if(g_goneTree&&g_goneTree->scene&&g_goneTree->tree){
        if(g_target->view&&At<uint8_t*>(g_target->view,0x98)==g_goneTree->scene&&!At<void*>(g_goneTree->scene,0xa80))
            At<void*>(g_goneTree->scene,0xa80)=g_goneTree->tree;
        *g_goneTree={};
    }
}
void UpdateGoneTree() {
    if(!g_goneTree||!g_target->view)return;
    static uint64_t checked=0;static bool on=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;on=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\tree.gone")!=INVALID_FILE_ATTRIBUTES;}
    auto* scene=At<uint8_t*>(g_target->view,0x98);
    if(on&&scene&&!g_goneTree->tree&&At<void*>(scene,0xa80)){
        g_goneTree->scene=scene;g_goneTree->tree=At<void*>(scene,0xa80);
        At<void*>(scene,0xa80)=nullptr;
        greeneye::Host()->log("Town Portal: dev: second camera's tree taken out of its scene");
    } else if(!on&&g_goneTree->tree){
        RestoreGoneTree();
        greeneye::Host()->log("Town Portal: dev: second camera's tree put back");
    }
}
void DestroyOwnedView() {
    RestoreGoneTree();
    g_viewDestroy(&g_target->view);
}
void ReleaseTarget() {
    CompositeRelease();
    g_destinationPoseValid=false;
    g_terrainProgress={};g_isolationTick=0;g_isolationFaults=0;
    g_renderOnce=false;g_ownedPrepared=false;g_liveUntil=0;g_displayProbeUntil=0;g_renderedPortal=0;g_ownedRenderTick=0;
    g_ownedRenderFlags=0;g_ownedRenderCalls=0;g_liveView=LiveView{};g_liveView2=LiveView{};g_liveNow=&g_liveView;
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
        if(g_actors)*g_actors={};if(g_effects)*g_effects={};
    }
    if(g_output&&g_output->context)g_freeImage(reinterpret_cast<void*>(g_output->context),g_output);
    if(g_target->ticket&&g_target->root) {
        g_release(At<void*>(reinterpret_cast<void*>(g_target->root),0x160),&g_target->ticket);
        ++g_target->releases;
    }
    g_target->root=0;g_target->color={};g_target->depth={};
    g_target->width=0;g_target->height=0;g_target->error=0;
}
// The second camera's scene needs its own brick tree (the game's structure for terrain past the
// 32 m of full-detail meshes, detailed round its own camera). The game does try to make one
// with the scene, but takes the memory from the renderer's own allocator, which is all but
// full once the player's tree exists: the first 768 KB array already fails ("[brick_tree]
// Failed to create brick tree" in the game's log), and the far side then showed the player's
// tree, coarse and smeared away from the player (2026-11-09). So while the second camera's
// scene is being made, the tree is handed an allocator of its own.
//
// That allocator has to outlive this DLL (the tree frees through it when the scene goes, maybe
// generations of hot reload later), so it is a page of machine code and data that is never
// freed, kept by the loader: an object whose table has the game's allocate (+8: this, size,
// alignment, ...) and free (+0x10: this, pointer, ...) as VirtualAlloc and VirtualFree (whole
// pages, zeroed, aligned to 64 KB, which covers every alignment asked for), and counters.
struct TreeAllocatorKeep {uint8_t* page;};
TreeAllocatorKeep* g_treeAllocator{};
uint8_t* EnsureTreeAllocator() {
    if(!g_treeAllocator)return nullptr;
    if(g_treeAllocator->page)return g_treeAllocator->page;
    auto* page=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!page)return nullptr;
    // +0x00 the object (its table pointer); +0x10 the table (8 entries); +0x80 allocations,
    // +0x88 frees, +0x90 bytes asked for; +0x100 code.
    uint8_t* code=page+0x100;uint8_t* at=code;
    auto put=[&](std::initializer_list<uint8_t> bytes){for(uint8_t b:bytes)*at++=b;};
    auto put64=[&](uint64_t v){std::memcpy(at,&v,8);at+=8;};
    auto rel=[&](uint8_t* target){const int32_t d=int32_t(target-(at+4));std::memcpy(at,&d,4);at+=4;};
    uint8_t* allocate=at;
    put({0x48,0x85,0xD2,0x75,0x03,0x48,0xFF,0xC2});          // test rdx,rdx; jnz +3; inc rdx
    put({0xF0,0x48,0xFF,0x05});rel(page+0x80);                // lock inc qword [allocations]
    put({0xF0,0x48,0x01,0x15});rel(page+0x90);                // lock add [bytes],rdx
    put({0x48,0x83,0xEC,0x28,0x33,0xC9});                     // sub rsp,28h; xor ecx,ecx
    put({0x41,0xB8,0x00,0x30,0x00,0x00});                     // mov r8d,MEM_COMMIT|MEM_RESERVE
    put({0x41,0xB9,0x04,0x00,0x00,0x00});                     // mov r9d,PAGE_READWRITE
    put({0x48,0xB8});put64(reinterpret_cast<uint64_t>(&VirtualAlloc));
    put({0xFF,0xD0,0x48,0x83,0xC4,0x28,0xC3});                // call rax; add rsp,28h; ret
    uint8_t* release=at;
    put({0x48,0x85,0xD2,0x75,0x01,0xC3});                     // test rdx,rdx; jnz +1; ret
    put({0xF0,0x48,0xFF,0x05});rel(page+0x88);                // lock inc qword [frees]
    put({0x48,0x83,0xEC,0x28,0x48,0x8B,0xCA,0x33,0xD2});      // sub rsp,28h; mov rcx,rdx; xor edx,edx
    put({0x41,0xB8,0x00,0x80,0x00,0x00});                     // mov r8d,MEM_RELEASE
    put({0x48,0xB8});put64(reinterpret_cast<uint64_t>(&VirtualFree));
    put({0xFF,0xD0,0x48,0x83,0xC4,0x28,0xC3});                // call rax; add rsp,28h; ret
    uint8_t* nothing=at;
    put({0x33,0xC0,0xC3});                                    // xor eax,eax; ret
    auto** table=reinterpret_cast<uint8_t**>(page+0x10);
    for(int i=0;i<8;++i)table[i]=nothing;
    table[1]=allocate;table[2]=release;
    *reinterpret_cast<uint8_t***>(page)=table;
    DWORD old{};
    if(!VirtualProtect(page,0x1000,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(page,0,MEM_RELEASE);return nullptr;}
    FlushInstructionCache(GetCurrentProcess(),page,0x1000);
    g_treeAllocator->page=page;
    return page;
}
// The game's render memory ("Game_Render", 450 MB, made in FUN_1408435e0) is one fixed pool.
// With the second camera's scene in it the pool is full, and what does not fit spills into
// the game's general memory (136 MB of it on 2026-11-10) - except for requests made through
// FUN_140886ae0, which forbids the spill (flags 4 and 8): those came back empty. The game
// logged "[render_system] out of memory" about 48 times a second (cloth arrays of a model it
// kept retrying, FUN_140117770), cloth pieces were made without their simulation object (the
// fault of 2026-11-09), and the effect system's buffers go through the same call. So when
// such a request fails, it is asked again with the spill allowed (flag 8 alone).
using StrictAllocFn=void*(*)(void** allocator,uint64_t size,uint64_t alignment,uint8_t* flag);
StrictAllocFn g_strictAlloc{};
std::atomic<uint64_t> g_strictRetried{},g_strictBytes{};
void* StrictAllocHook(void** allocator,uint64_t size,uint64_t alignment,uint8_t* flag) {
    void* made=g_strictAlloc(allocator,size,alignment,flag);
    if(made||!size||size>(64ull<<20)||!allocator||!flag)return made;
    greeneye::Lease lease;
    if(!lease)return made;
    auto allocate=reinterpret_cast<void*(*)(void*,uint64_t,uint64_t,uint32_t,uint8_t)>((*reinterpret_cast<void***>(allocator))[1]);
    made=allocate(allocator,size,alignment,8u,*flag);
    if(made){
        g_strictBytes+=size;
        const uint64_t count=++g_strictRetried;
        if(count<=5||count%1000==0){
            char line[160]{};
            std::snprintf(line,sizeof(line),"Town Portal: the game's render memory was full; request %llu (%llu bytes) given general memory, %llu MB so in all",count,size,g_strictBytes.load()>>20);
            greeneye::Host()->log(line);
        }
    }
    return made;
}
// The same for every other request to the render memory. Only the strict call above was
// covered; the rest ask the pool directly (its allocate, vtable slot 1: a thin wrapper,
// self, size, alignment, flags, one more byte) and take "no" for an answer: with the pool full
// (450 of 450 MB with the second scene, 2026-11-10) the altar's flame was simply not there in
// the far view, though its effect ran. Flag 8 lets a request spill into general memory.
// Hooked once the pool is known (render root +0), for that pool only, and not past 1 GB of
// spill (pool +0xb8). tools/pool.off (dev) leaves the retry out.
// The hook is on the function under that wrapper (FUN_1408bc680: self, where the answer
// goes (three words, the memory first), size, alignment, flags, one more byte), which has a
// signature of its own; hooks can only be made at start, when the pool is not yet known.
struct PoolAnswer {void* memory;uint64_t a,b;};
using PoolAllocFn=PoolAnswer*(*)(void* self,PoolAnswer* answer,uint64_t size,uint64_t alignment,uint8_t flags,uint8_t more);
PoolAllocFn g_poolAlloc{};
bool g_poolOff{};
std::atomic<uint64_t> g_poolRetried{},g_poolBytes{};
PoolAnswer* PoolAllocHook(void* self,PoolAnswer* answer,uint64_t size,uint64_t alignment,uint8_t flags,uint8_t more) {
    PoolAnswer* made=g_poolAlloc(self,answer,size,alignment,flags,more);
    if(!answer||answer->memory||!size||(flags&8)||size>(64ull<<20))return made;
    greeneye::Lease lease;
    if(!lease||g_poolOff||!g_world||!g_world->root)return made;
    if(self!=*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(g_world->root)))return made;
    if(At<uint64_t>(self,0xb8)>(1024ull<<20))return made;
    made=g_poolAlloc(self,answer,size,alignment,uint8_t(flags|8),more);
    if(answer->memory){
        g_poolBytes+=size;
        const uint64_t count=++g_poolRetried;
        if(count<=5||count%5000==0){
            char line[180]{};
            std::snprintf(line,sizeof(line),"Town Portal: render memory full: request %llu (%llu bytes, flags %u) given general memory, %llu MB so in all",count,size,unsigned(flags),g_poolBytes.load()>>20);
            greeneye::Host()->log(line);
        }
    }
    return made;
}
void EnsurePoolHook() {
    static uint64_t checked=0;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;g_poolOff=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/pool.off")!=INVALID_FILE_ATTRIBUTES;}
}
// The game's volumetric fog keeps ONE pool of density tiles for all scenes: 24 x 24 x 16 =
// 9216 (constants in the renderer's create, FUN_140101ea0; the atlas is made by
// FUN_1401b9790(atlas, description) with the three counts at description +0x20/+0x24/+0x28).
// The player's scene uses most of it, and the second scene's fog then ran out ("[volumetric_fog3]
// out of density field tiles", 4884 times at each creation). Made 32 x 32 x 16 = 16384 here, at
// the game's start (32 an axis is the most a tile's number can hold). Takes effect only in a
// game started with the mod loaded. tools/fogtiles.off (dev) leaves the game's size.
using FogAtlasFn=void*(*)(void* atlas,uint8_t* description);
FogAtlasFn g_fogAtlas{};
bool g_fogTilesRaised{};
void* FogAtlasHook(void* atlas,uint8_t* description) {
    if(description&&At<uint32_t>(description,0x20)==24&&At<uint32_t>(description,0x24)==24&&At<uint32_t>(description,0x28)==16
       &&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fogtiles.off")==INVALID_FILE_ATTRIBUTES){
        At<uint32_t>(description,0x20)=32;At<uint32_t>(description,0x24)=32;
        g_fogTilesRaised=true;
        greeneye::Host()->log("Town Portal: volumetric fog tile pool raised from 9216 to 16384 (room for the far view's fog)");
    }
    return g_fogAtlas(atlas,description);
}
// The game's task queues: ONE pool for every scene, the server inside a hosted game and more,
// sized by the task system's description (+0x20, the hard-coded 32 the app's start-up passes
// to FUN_1408ea720(allocator, description), which allocates the pool with that many slots;
// nothing else assumes 32: static RE 2026-11-10, C:/temp/enshrouded-re/queue-q*-out.txt). On a
// world the player hosts only 3 were free, the second camera needs 5 (scene, water, world
// attach), and squeezing it in crashed the graphics card. Made 48 here at the game's start
// (about 8 KB more); LiveRoom still asks for 5 free. Takes effect only in a game started with
// the mod loaded. tools/queues.off (dev) leaves the game's size.
using TaskSystemCreateFn=void*(*)(void* allocator,uint8_t* description);
TaskSystemCreateFn g_taskSystemCreate{};
void* TaskSystemCreateHook(void* allocator,uint8_t* description) {
    if(description&&At<uint32_t>(description,0x20)>=16&&At<uint32_t>(description,0x20)<48
       &&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/queues.off")==INVALID_FILE_ATTRIBUTES){
        char line[140]{};
        std::snprintf(line,sizeof(line),"Town Portal: the game's task queues raised from %u to 48 (room for the live view on a hosted world)",At<uint32_t>(description,0x20));
        At<uint32_t>(description,0x20)=48;
        greeneye::Host()->log(line);
    }
    return g_taskSystemCreate(allocator,description);
}
using BrickTreeCreateFn=void*(*)(void* allocator,void* parameters);
BrickTreeCreateFn g_brickTreeCreate{};
bool g_creatingOwned{};
void* BrickTreeCreateHook(void* allocator,void* parameters) {
    greeneye::Lease lease;
    // Dev: tools\tree.shared leaves the second camera without a tree of its own (as before
    // 2026-11-09 18:20), to compare.
    if(lease&&g_creatingOwned&&GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\tree.shared")==INVALID_FILE_ATTRIBUTES){
        if(auto* own=EnsureTreeAllocator()){
            void* tree=g_brickTreeCreate(own,parameters);
            char line[200]{};
            std::snprintf(line,sizeof(line),"Town Portal: second camera's own brick tree %s (%llu allocations and %llu frees so far, %llu MB asked for in all)",
                          tree?"made":"NOT made",At<uint64_t>(own,0x80),At<uint64_t>(own,0x88),At<uint64_t>(own,0x90)>>20);
            greeneye::Host()->log(line);
            return tree;
        }
    }
    return g_brickTreeCreate(allocator,parameters);
}
struct CreatingOwned {CreatingOwned(){g_creatingOwned=true;}~CreatingOwned(){g_creatingOwned=false;}};
#ifdef TOWN_PORTAL_DEV
// Dev: the second camera's scene gets no brick tree ("[brick_tree] Failed to create brick tree"
// in the game's log at every creation, 2026-11-09). The game destroys the half-made tree
// (FUN_1400526f0) on the way out: which of its parts were made by then says which step failed.
using BrickTreeDestroyFn=void(*)(uint8_t*);
BrickTreeDestroyFn g_brickTreeDestroy{};
void DescribeGivenUpTree(uint8_t* tree) {
    {
        __try {
            char line[400]{};
            std::snprintf(line,sizeof(line),"Town Portal: brick tree given up at creation: task queue %p, pools %p %p, callbacks %p %p, gpu %llx %llx %llx, sizes %u %u %u = %u bricks, %u nodes, arrays %p %p %p %p, +0x2076 %llx",
                          At<void*>(tree,0x38),At<void*>(tree,0x2080*8),At<void*>(tree,0x2088*8),At<void*>(tree,0x2a*8),At<void*>(tree,0x20ad*8),
                          At<uint64_t>(tree,0x2062*8),At<uint64_t>(tree,0x2066*8),At<uint64_t>(tree,0x206a*8),
                          At<uint32_t>(tree,0x104),At<uint32_t>(tree,0x108),At<uint32_t>(tree,0x10c),At<uint32_t>(tree,0x110),At<uint32_t>(tree,0x114),
                          At<void*>(tree,0xf*8),At<void*>(tree,0xc*8),At<void*>(tree,0x1c*8),At<void*>(tree,0x11*8),At<uint64_t>(tree,0x2076*8));
            greeneye::Host()->log(line);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
}
void BrickTreeDestroyHook(uint8_t* tree) {
    greeneye::Lease lease;
    if(lease&&g_creatingOwned&&tree)DescribeGivenUpTree(tree);
    g_brickTreeDestroy(tree);
}
#endif
void CreateOwnedViewBody(void* scene);
extern uint8_t* g_treeSeen;
void CreateOwnedView(void* scene) {
    CreatingOwned creating;
    g_treeSeen=nullptr; // a new tree may get the old one's address
    CreateOwnedViewBody(scene);
}
void CreateOwnedViewBody(void* scene) {
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
    // (2026-11-10: the water is kept after all. Without it a lake seen through a portal was
    // dry land; with the scene's water tree and the view's Water feature it has waves and
    // reflections, and the queues were there. tools/water.off, dev, leaves it out as before.)
    // [Portals] LiveView=Full|Balanced|Fast in town-portal.ini (read when the second camera is
    // made): what the view through a portal is allowed to cost. Full: everything. Balanced:
    // without the far view's water (a lake seen through a portal is dry; about 4 frames a
    // second back). Fast: also without its bounced light (shade seen through a portal is very
    // dark; about 7 more). Measured 2026-11-10: 31 to 34 frames a second at Full against 60
    // with no portal on screen.
    int liveQuality=std::max(0,LiveViewSetting());
    {
        char line[100]{};std::snprintf(line,sizeof(line),"Town Portal: live view quality: %s",liveQuality==2?"Fast":liveQuality==1?"Balanced":"Full");
        greeneye::Host()->log(line);
    }
    bool farWater=liveQuality==0&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/water.off")==INVALID_FILE_ATTRIBUTES;
    At<uint32_t>(description,4)=(At<uint32_t>(description,4)|0x100)&~0x80u;
    if(farWater)At<uint32_t>(description,4)|=0x80u;
    // Dev test: tools\water.tree keeps the water brick tree (create flag 0x80).
    if(GetFileAttributesW(L"C:\Mods\Enshrouded\TownPortal\tools\water.tree")!=INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,4)|=0x80u;
    // Dev: tools/create.clear holds a hex mask of scene create flags to leave out (bit 0
    // MaterialBaking, 1 DetailScattering, 2 Weather, 3 Cloth, 4 ChainSystem, 5 Fog3, 6 BrickTree,
    // 7 WaterBrickTree, 8 DynamicDisplaySize).
    if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/create.set","r")){
        unsigned set=0;if(std::fscanf(f,"%x",&set)==1)At<uint32_t>(description,4)|=set;
        std::fclose(f);
    }
    if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/create.clear","r")){
        unsigned clear=0;if(std::fscanf(f,"%x",&clear)==1)At<uint32_t>(description,4)&=~clear;
        std::fclose(f);
    }
    // Bounded world configuration: culling, shadows, VFX, fog, grass, SSAO,
    // heightmap, contact shadows, TAA and bricks, with GI dependencies.
    // No shared SSR, DLSS or UI frosted glass. f4140 maps public bit16
    // to internal feature17; clearing public bit17 would NOT exclude SSR.
    // Full-detail terrain for the second camera needs two things (found 2026-11-09):
    // - Bit 4 (0x10), with bit 25, turns on the view's internal feature 16, and only with that
    //   does the game mesh the terrain for the view at full detail (FUN_14012c3b0 passes the
    //   terrain mesher level 6 instead of 0). Without it the far walls and ground are the brick
    //   renderer's distance look, colours smeared down the walls.
    // - Occlusion culling (bit 0) must stay ON: the terrain pieces (model scene +0x2b0) reach
    //   the colour pass only through the culling path. With it off they were in the depth
    //   pass alone and drew as pitch black silhouettes.
    // - Bit 6 (DetailScattering in the game's log) is the grass and small ground plants; without
    //   it the far ground was bare.
    // The game's log names the set at each creation ("creating render scene ... initial features").
    constexpr uint32_t features=0x0a3027ff; // GI dependencies; Water disabled with WaterBrickTree.
    static_assert(!(features & 0x10000));
    const uint32_t worldFeatures=At<uint32_t>(description,0x40);
    At<uint32_t>(description,0x40)=features|(farWater?0x00800000u:0u);
    if(liveQuality==2)At<uint32_t>(description,0x40)&=~0x00200000u; // GpuBvh: the bounced light
    // Without the view's own anti-aliasing (Taa, public bit 20): with it the far picture was
    // smoothed twice, by this and then by the player's upscaler, and grass blades ran together
    // while stonework looked painted; without it blades and stones are crisp (2026-11-10; the
    // frame is at twice the player's pixel density, which the player's upscaler smooths).
    // tools/taa.on (dev) keeps it. (Bit 21 is GpuBvh: without it shade has no bounced light.)
    if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/taa.on")==INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)&=~0x00100000u;
    // Without the view's fog (Fog, public bit 3): with it no flame, spark, smoke or spell effect
    // showed in the far view. Their pieces were simulated and handed over for drawing (the far
    // view's list counters ran), but they are drawn through the view's fog volume, which for this
    // view comes out wrong (the game logs "[volumetric_fog3] out of density field tiles" each
    // time the second scene is made) and took them away entirely. Without the feature the altar's
    // flame and a held wand's flame show as they should (2026-11-10). The far view then has no
    // distance haze of its own. tools/fog.on (dev) keeps the feature.
    // (Since the effects are drawn without fog, VfxPassCall, the view keeps its fog: with it come
    // the sky and the haze. tools/fog.off, dev, leaves it out as before; without the hooks too.)
    if(!g_vfxPassAll||GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fog.off")!=INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)&=~0x00000008u;
    // NOT the player's own feature set: with it the view runs the player's upscaler (DLSS), which
    // gave a sharp far side, but the game has one DLSS history for all views, and the second
    // camera's frames ghosted over the player's whole screen (2026-11-09). tools\features.player
    // (dev) still tries it, without Water (bit 23).
    if(GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\features.player")!=INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)=worldFeatures&~0x00800000u;
    // Dev: tools\features.set holds a hex mask of feature bits to add to the second camera
    // (read once at creation), to find what the player's own view has that this one lacks
    // (grass, 2026-11-09). The log names the player's view's own mask.
    {
        unsigned add=0;
        if(FILE* f=_wfopen(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\features.set",L"r")){
            if(std::fscanf(f,"%x",&add)!=1)add=0;
            std::fclose(f);
        }
        At<uint32_t>(description,0x40)|=add;
        char line[160]{};
        std::snprintf(line,sizeof(line),"Town Portal: second camera features 0x%08x (added 0x%x); the player's view has 0x%08x, flags 0x%08x",
                      At<uint32_t>(description,0x40),add,worldFeatures,At<uint32_t>(g_world->creation,4));
        greeneye::Host()->log(line);
    }
    // Dev: tools\occlusion.disabled leaves occlusion culling out (the terrain then draws black,
    // see above). It was left out for a while because blocks of the far place went missing
    // from frame to frame when the view was re-aimed at 30 frames a second (2026-11-09).
    if(GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\occlusion.disabled")!=INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)&=~1u;
    // Dev: tools\features.clear holds a hex mask of feature bits to leave out of the second
    // camera (read once at creation), to find which one draws something wrongly.
    if(FILE* f=_wfopen(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\features.clear",L"r")){
        unsigned clear=0;
        if(std::fscanf(f,"%x",&clear)==1){
            At<uint32_t>(description,0x40)&=~clear;
            char line[96]{};
            std::snprintf(line,sizeof(line),"Town Portal: second camera features 0x%08x (cleared 0x%x)",At<uint32_t>(description,0x40),clear);
            greeneye::Host()->log(line);
        }
        std::fclose(f);
    }
    // DynamicResolution (bit 29): the second camera's frame size follows the opening (RenderScaleHook).
    if(g_renderScale&&GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\owned.fixed")==INVALID_FILE_ATTRIBUTES)
        At<uint32_t>(description,0x40)|=0x20000000u;
    At<uint32_t>(description,0x4c)=kOwnedWidth;At<uint32_t>(description,0x50)=kOwnedHeight;
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
    uint32_t saved=At<uint32_t>(description,0x64);
    // Alter the startup descriptor before any pools or scene-indexed systems
    // exist. Never resize or replace a live scene pool.
    At<uint32_t>(description,0x64)=std::max(saved,4u);
    __try {return g_rootCreate(result,allocator,description);}
    __finally {At<uint32_t>(description,0x64)=saved;}
}
void* RootCreateHook(void* result,void* allocator,void* description) {
    greeneye::Lease lease;
    if(!lease) return g_rootCreate(result,allocator,description);
    uint32_t saved=At<uint32_t>(description,0x64);
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
        g_liveUnavailable=false; // a new world: room for the live view is judged again
        std::memcpy(g_world->creation,description,sizeof(g_world->creation));
    }
    return returned;
}
void WorldBindHook(void* models,const void* description) {
    greeneye::Lease lease;
    g_worldBind(models,description);
    if(lease) {
        std::lock_guard lock(g_lock);
        {
            static uint32_t told=0;
            if(told<20){
                ++told;char line[160]{};
                std::snprintf(line,sizeof(line),"Town Portal: the game attaches a scene to the world: %p (the player's view %p)%s",
                              reinterpret_cast<void*>(At<uint64_t>(models,0)),reinterpret_cast<void*>(g_world->scene),At<uint64_t>(models,0)==g_world->scene?"":", not the player's");
                greeneye::Host()->log(line);
            }
        }
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
    if(capacity<used||capacity-used<needed) {
        static uint64_t told=0;
        if(const uint64_t now=GetTickCount64();now-told>10000){
            told=now;char line[160]{};
            std::snprintf(line,sizeof(line),"Town Portal: live view waits for the game's task queues (%u of %u in use, %u needed)",used,capacity,needed);
            greeneye::Host()->log(line);
        }
        return false;
    }
    return true;
}
void ProcessTargetRequest(void* scene,uint32_t action) {
    NoteGraphicsLock();
    // Only the native rendering thread touches allocation plans or view state.
    __try {
        if(action==8) ReleaseTarget();
        else if(action==7||action==9) {
            AllocateTarget(scene);
            if(g_target->ticket&&!g_target->error)AllocateLiveSecond(At<uint8_t*>(scene,0));
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
                g_displayProbeUntil=GetTickCount64()+600000;g_probePoseValid=false; // ten minutes
                // Development-only A/B switch, read once per explicit MCP probe.
                const bool clip=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\exit-clip.disabled")==INVALID_FILE_ATTRIBUTES;
                g_forceCameraCut=g_forceCameraCut||clip!=g_exitClipEnabled;g_exitClipEnabled=clip;
            }
        }
    } __except(TargetFault(GetExceptionInformation())) {}
}
// The live view by itself (greeneye\town-portal\live-view.enabled): while a portal on screen asks
// for it (LiveImage sets g_liveWantTick), the second camera is made, then the world attached to
// it; once no portal has asked for 20 s it is destroyed again (the compositor's own images
// stay). One step per call, a quarter second apart, on the render thread before the frame.
// [Portals] LiveView in town-portal.ini: Full (the default), Balanced, Fast, or Off (the old
// pictures with parallax, no live view). Read again at most once a second; the quality is taken
// when the second camera is made (CreateOwnedView), Off takes effect at once.
inline constexpr const char* kLiveViewKey2_0_0 =
    "; LiveView: what you see through a portal. Full shows the other side live; Balanced leaves\n"
    "; water out of that view and Fast also its bounced light (more frames a second, darker shade);\n"
    "; Off shows still pictures of the other side as in earlier versions.\n"
    "LiveView=Full\n";
inline constexpr const char* kLiveViewKey =
    "; LiveView: what you see through a portal. Auto: live (Full) only with the game's Ultra Quality\n"
    "; preset on an NVIDIA graphics card with 8 GB or more, otherwise still pictures with depth (Off).\n"
    "; Full shows the other side live; Balanced leaves water out of that view and Fast also its\n"
    "; bounced light (more frames a second, darker shade); Off shows still pictures, as in 1.x.\n"
    "LiveView=Auto\n";
// 2.0.0 wrote LiveView=Full into every file. Its block, untouched, becomes Auto; a value the
// player set by hand stays (2.0.1: the live view hung an AMD card and cost others too much).
void MigrateLiveViewKey() {
    const std::wstring path=rules::Path();
    FILE* f=_wfopen(path.c_str(),L"rb");
    if(!f)return;
    std::string text;char buf[4096];
    for(size_t n;(n=std::fread(buf,1,sizeof(buf),f))>0;)text.append(buf,n);
    std::fclose(f);
    std::string old=kLiveViewKey2_0_0;
    size_t at=text.find(old);
    if(at==std::string::npos){ // the same with CRLF line ends
        std::string crlf;for(char ch:old){if(ch=='\n')crlf+='\r';crlf+=ch;}
        at=text.find(crlf);if(at!=std::string::npos)old=crlf;
    }
    if(at==std::string::npos)return;
    text.replace(at,old.size(),kLiveViewKey);
    if(FILE* out=_wfopen(path.c_str(),L"wb")){std::fwrite(text.data(),1,text.size(),out);std::fclose(out);}
    greeneye::Host()->log("Town Portal: LiveView=Full from 2.0.0 changed to LiveView=Auto in town-portal.ini");
}
// LiveView=Auto: 0 Full, -1 Off. The live view was made and tested on an NVIDIA card with the
// game's Ultra Quality preset; on an AMD RX 7900 XTX (FSR3) its first frame hung the graphics
// card, and others lost too many frames a second (Nexus, 2026-11-10). Auto shows it live only
// with the Ultra preset (user's rule, 2026-11-10) on an NVIDIA card with 8 GB or more; anything
// else gets the still pictures with depth.
int LiveViewAuto() {
    static int choice=-2;
    if(choice!=-2)return choice;
    uint32_t vendor=0;uint64_t memory=0;
    IDXGIFactory1* factory=nullptr;
    if(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))){
        IDXGIAdapter1* adapter=nullptr;
        for(UINT i=0;i<8&&factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i){
            DXGI_ADAPTER_DESC1 desc{};
            if(SUCCEEDED(adapter->GetDesc1(&desc))&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&desc.DedicatedVideoMemory>memory){
                memory=desc.DedicatedVideoMemory;vendor=desc.VendorId;
            }
            adapter->Release();
        }
        factory->Release();
    }
    std::string preset;
    {
        PWSTR games=nullptr;std::string text;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames,0,nullptr,&games))){
            const std::wstring path=std::wstring(games)+L"\\keen games\\Enshrouded\\local_settings.json";
            if(FILE* f=_wfopen(path.c_str(),L"rb")){text.resize(65536);text.resize(std::fread(text.data(),1,text.size(),f));std::fclose(f);}
            CoTaskMemFree(games);
        }
        const size_t at=text.find("\"preset\"");
        if(at!=std::string::npos){
            const size_t open=text.find('"',text.find(':',at+8));
            const size_t close=open==std::string::npos?open:text.find('"',open+1);
            if(close!=std::string::npos)preset=text.substr(open+1,close-open-1);
        }
    }
    const uint64_t gb=memory>>30;
    const char* why="the game's Ultra Quality preset";
    choice=0;
    if(preset!="Ultra"){choice=-1;why="the game's preset is not Ultra Quality";}
    else if(vendor!=0x10de){choice=-1;why=vendor==0x1002?"an AMD graphics card":vendor==0x8086?"an Intel graphics card":"the graphics card is not NVIDIA";}
    else if(gb<8){choice=-1;why="under 8 GB of video memory";}
    char line[220]{};
    std::snprintf(line,sizeof(line),"Town Portal: LiveView=Auto chose %s: %s (preset %s, graphics card vendor %04x, %llu GB)",
                  choice<0?"Off (still pictures)":"Full (live)",why,preset.empty()?"unknown":preset.c_str(),vendor,static_cast<unsigned long long>(gb));
    greeneye::Host()->log(line);
    return choice;
}
int LiveViewSetting() {
    static uint64_t checked=0;static int value=0;static bool added=false;
    const uint64_t now=GetTickCount64();
    if(!added){added=true;MigrateLiveViewKey();rules::AddPortalsKey(L"LiveView",kLiveViewKey);}
    if(now-checked>1000){
        checked=now;
        wchar_t text[16]{};
        GetPrivateProfileStringW(L"Portals",L"LiveView",L"Auto",text,16,rules::Path().c_str());
        value=!_wcsicmp(text,L"Off")?-1:!_wcsicmp(text,L"Balanced")?1:!_wcsicmp(text,L"Fast")?2:!_wcsicmp(text,L"Full")?0:LiveViewAuto();
    }
    return value;
}
// The game has 32 task queues for all its scenes; the second camera's scene takes 2, its water
// 2 more, and attaching it to the world 1. On a world the player hosts, the server inside the
// game holds more of them: only 3 were free, and a second camera made in that squeeze (without
// its water) crashed the graphics card within seconds, twice ("Vulkan device lost" in the world
// distance field pass, 2026-11-10). So the live view is only started with room for all five;
// otherwise the portals show their pictures as with LiveView=Off, until the next world.
bool LiveAuto(uint64_t now) {
    (void)now;
    return LiveViewSetting()>=0&&!g_liveUnavailable.load();
}
bool LiveRoom(void* scene) {
    auto* scheduler=At<void*>(At<void*>(scene,0),0x148);
    const uint32_t used=At<uint32_t>(scheduler,0xe0),capacity=At<uint32_t>(scheduler,0xf0);
    const uint32_t free=capacity<used?0u:capacity-used;
    if(free>=5)return true;
    if(!g_liveUnavailable.exchange(true)){
        char line[200]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view off for this world: %u of %u of the game's task queues free, 5 needed (a hosted world's server holds the rest); portals show pictures",free,capacity);
        greeneye::Host()->log(line);
    }
    return false;
}
void ReleaseOwnedView() {
    if(!g_target->view||!g_world||g_world->ownedView!=reinterpret_cast<uint64_t>(g_target->view))return;
    g_destinationPoseValid=false;g_terrainProgress={};g_isolationTick=0;g_isolationFaults=0;
    g_renderOnce=false;g_ownedPrepared=false;g_liveUntil=0;g_renderedPortal=0;g_ownedRenderTick=0;
    g_ownedRenderFlags=0;g_ownedRenderCalls=0;g_liveView=LiveView{};g_liveView2=LiveView{};g_liveNow=&g_liveView;
    DestroyOwnedView();
    g_world->ownedView=0;g_world->ownedBound=0;++g_world->destructions;
    if(g_mirrors)*g_mirrors={};
    if(g_actors)*g_actors={};if(g_effects)*g_effects={};
    g_haveModelFocus=false;
}
// Dev: the slowest run of each part of the scene tick since the last report (microseconds):
// 0 the work before the game's tick, 1 the game's own tick, 2 the second scene's tick, 3 the
// second camera's render call.
uint64_t g_slowest[4]{};
// Dev: game frames counted, the frame the live picture was last taken in, and over the report
// period the fewest and most frames it was old when the portal was drawn with it.
uint64_t g_frameNo{},g_liveFrameNo{},g_lagMin{~0ull},g_lagMax{};
// Dev: per bake entry (root +0x16a78 list), the most pieces waiting (+0xc0) seen in a frame
// and in how many frames any were waiting, since the last report.
uint32_t g_bakeMax[4]{},g_bakeFrames[4]{};
void WatchBake() {
    __try {
        if(!g_world||!g_world->root)return;
        auto* root=reinterpret_cast<uint8_t*>(g_world->root);
        int index=0;
        for(auto* item=At<uint8_t*>(root,0x16a78);item&&index<4;item=At<uint8_t*>(item,0x10),++index){
            const uint32_t waiting=At<uint32_t>(item,0xc0);
            g_bakeMax[index]=std::max(g_bakeMax[index],waiting);
            g_bakeFrames[index]+=waiting!=0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
struct Stopwatch {
    int part;LARGE_INTEGER start{};
    explicit Stopwatch(int p):part(p){QueryPerformanceCounter(&start);}
    ~Stopwatch(){
        LARGE_INTEGER end{},frequency{};QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
        const uint64_t micro=uint64_t(end.QuadPart-start.QuadPart)*1000000ull/uint64_t(frequency.QuadPart);
        if(micro>g_slowest[part])g_slowest[part]=micro;
    }
};
void LiveLifecycle(void* scene) {
    static uint64_t stepAt=0;
    const uint64_t now=GetTickCount64();
#ifdef TOWN_PORTAL_DEV
    // Dev: every 5 s, the game's frames a second, the slowest frame, and how many of the frames
    // had a live view frame taken (to see what the live view costs and whether frames stall).
    {
        static uint64_t since=0,last=0,frames=0,worst=0,renders=0;
        LARGE_INTEGER counter{},frequency{};
        QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);
        const uint64_t micro=uint64_t(counter.QuadPart)*1000000ull/uint64_t(frequency.QuadPart);
        // Dev: with vsync a frame takes one or two display refreshes; the share of each says how
        // far the frame's work is over the refresh time, which the average hides.
        static uint32_t quick=0,middle=0,slow=0;
        if(last){const uint64_t took=micro-last;++(took<20000?quick:took<28000?middle:slow);}
        if(last)worst=std::max(worst,micro-last);
        last=micro;++frames;
        if(!since){since=micro;renders=g_ownedRenderCalls;}
        else if(micro-since>=5000000){
            char line[260]{};
            const double seconds=double(micro-since)/1e6;
            std::snprintf(line,sizeof(line),"Town Portal: frames %.1f a second, slowest %.1f ms, live view frames %.1f a second; slowest parts: before %.1f, game tick %.1f, second scene %.1f, second render %.1f ms",
                          double(frames)/seconds,double(worst)/1000.0,double(g_ownedRenderCalls-renders)/seconds,
                          g_slowest[0]/1000.0,g_slowest[1]/1000.0,g_slowest[2]/1000.0,g_slowest[3]/1000.0);
            greeneye::Host()->log(line);
            {
                auto* owned=g_target&&g_target->view?At<uint8_t*>(g_target->view,0xa0):nullptr;
                auto* world=WorldState();
                std::snprintf(line,sizeof(line),"Town Portal: frame times: under 20 ms %u, 20 to 28 ms %u, over 28 ms %u; game's frame time measure: second camera %.2f (scale %.2f), player's %.2f (scale %.2f)",
                              quick,middle,slow,owned?At<float>(owned,0xb4):0.f,owned?At<float>(owned,0xb8):0.f,world?At<float>(world,0xb4):0.f,world?At<float>(world,0xb8):0.f);
                greeneye::Host()->log(line);
                quick=middle=slow=0;
                std::snprintf(line,sizeof(line),"Town Portal: effect table: wipes %llu, left out after the second camera %llu",g_tableWipes.exchange(0),g_tableWipesSkipped.exchange(0));
                greeneye::Host()->log(line);
                // Dev: each view's texture sampling values (view +0xdc mip bias, +0xe0 anisotropy,
                // +0xe4.. its four samplers), and the words round them.
                for(int who=0;who<2;++who){
                    auto* view=who?(g_target?static_cast<uint8_t*>(g_target->view):nullptr):(g_world?reinterpret_cast<uint8_t*>(g_world->scene):nullptr);
                    if(!view)continue;
                    view=At<uint8_t*>(view,0xa0); // the values are in the view's state
                    if(!view)continue;
                    int n=std::snprintf(line,sizeof(line),"Town Portal: %s view %p sampling: bias %.3f, anisotropy %.3f, samplers %x %x %x %x; +0xc0..:",who?"second camera's":"player's",static_cast<void*>(view),
                                        At<float>(view,0xdc),At<float>(view,0xe0),At<uint32_t>(view,0xe4),At<uint32_t>(view,0xe8),At<uint32_t>(view,0xec),At<uint32_t>(view,0xf0));
                    for(size_t o=0xc0;o<0x110&&n<int(sizeof(line))-12;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(view,o));
                    greeneye::Host()->log(line);
                }
#ifdef TOWN_PORTAL_DEV
                {   // Dev: tools/tex.algorithm holds the game's texture streaming algorithm (its
                    // byte at exe+0x1fd9150 in the build of 2026-11-02: 0 unload, 1 full
                    // resolution for everything, 2 ideal, 3 balanced, 4 adaptive = the default),
                    // to see whether the far view's blobby leaves are unloaded texture detail.
                    static int applied=-1;int want=-1;
                    if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/tex.algorithm",L"r")){
                        if(std::fscanf(f,"%d",&want)!=1||want<0||want>4)want=-1;
                        std::fclose(f);
                    }
                    if(want<0&&applied>=0)want=4;
                    if(want>=0&&want!=applied){
                        auto* value=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr))+0x1fd9150;
                        __try {
                            std::snprintf(line,sizeof(line),"Town Portal: texture streaming algorithm %u -> %d",unsigned(*value),want);
                            *value=uint8_t(want);applied=want==4?-1:want;
                            greeneye::Host()->log(line);
                        } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
#endif
                // Dev: every small number in the player's model scene and its systems, a line each
                // 5 s with the player's camera position, to tools/scene-counts.txt: what changes
                // when the player nears the altar bowl and its flame starts?
                if(auto* scene=g_world&&g_world->scene?At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0x98):nullptr){
                    static char text[24000];
                    int n=std::snprintf(text,sizeof(text),"%llu cam %.1f %.1f %.1f |",GetTickCount64(),g_playerCamera.x,g_playerCamera.y,g_playerCamera.z);
                    const auto put=[&](const char* name,uint8_t* base,size_t from,size_t to){
                        if(!base)return;
                        for(size_t o=from;o<to&&n<int(sizeof(text))-64;o+=8){
                            const uint64_t v=At<uint64_t>(base,o);
                            if(v&&v<0x200000)n+=std::snprintf(text+n,sizeof(text)-n," %s%zx:%llx",name,o,v);
                        }
                    };
                    __try {
                        put("s",scene,0x20,0xc00);
                        put("v",At<uint8_t*>(scene,0x950),0,0x200);
                        put("l",At<uint8_t*>(scene,0x9f8),0,0x200);
                        put("w",At<uint8_t*>(scene,0xa38),0,0x100);
                        put("c",At<uint8_t*>(scene,0x9f0),0,0x100);
                        put("a",At<uint8_t*>(scene,0x310),0,0x100);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    // And the altar bowl's whole instance record (model 4fbfe08e940863bb), as words.
                    __try {
                        auto* instances=At<uint8_t*>(scene,0x1b0);const uint64_t capacity=At<uint64_t>(scene,0x1d0);
                        for(uint64_t i=0;instances&&i<capacity&&i<65536;++i){
                            auto* in=instances+i*0x1b0;auto* model=At<uint8_t*>(in,0x108);
                            if(!model||IsBadReadPtr(model,0x80)||At<uint64_t>(model,0x18)!=0x4fbfe08e940863bbull)continue;
                            n+=std::snprintf(text+n,sizeof(text)-n," | bowl %llu:",i);
                            for(size_t o=0;o<0x1b0&&n<int(sizeof(text))-16;o+=4)n+=std::snprintf(text+n,sizeof(text)-n," %x",At<uint32_t>(in,o));
                            break;
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    text[n++]=10;
                    HANDLE f=CreateFileW(L"C:/Mods/Enshrouded/TownPortal/tools/scene-counts.txt",FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,0,nullptr);
                    if(f!=INVALID_HANDLE_VALUE){DWORD wrote{};WriteFile(f,text,DWORD(n),&wrote,nullptr);CloseHandle(f);}
                }
                // Dev: the model-effect volumes (scene +0x9f8, drawn by the fog pass from the
                // boxes FUN_1401593e0 puts at state +0xef0 when state +0xed8 and +0xec8 are there).
                for(int who=0;who<2;++who){
                    auto* state=who?owned:world;
                    auto* view=who?(g_target?static_cast<uint8_t*>(g_target->view):nullptr):reinterpret_cast<uint8_t*>(g_world->scene);
                    auto* scene=view?At<uint8_t*>(view,0x98):nullptr;
                    auto* system=scene?At<uint8_t*>(scene,0x9f8):nullptr;
                    if(scene){
                        auto* root=At<uint8_t*>(scene,0x18);
                        std::snprintf(line,sizeof(line),"Town Portal: %s cloth: scene %p, +0x18 %p (first %p, cloth world +0x16490 %p), cloth system +0x9f0 %p, lists +0xa00 %llu, +0xa18 %llu",
                                      who?"second camera's":"player's",static_cast<void*>(scene),static_cast<void*>(root),root?At<void*>(root,0):nullptr,root?At<void*>(root,0x16490):nullptr,
                                      At<void*>(scene,0x9f0),At<uint64_t>(scene,0xa08),At<uint64_t>(scene,0xa20));
                        greeneye::Host()->log(line);
                    }
                    if(!state||!system)continue;
                    std::snprintf(line,sizeof(line),"Town Portal: %s volumes: state +0xec8 %p, +0xed8 %p, +0xed0 %p, +0x9b0 %p, boxes %llu; system active %llu, removed %llu, +0x78 %x",
                                  who?"second camera's":"player's",At<void*>(state,0xec8),At<void*>(state,0xed8),At<void*>(state,0xed0),At<void*>(state,0x9b0),At<uint64_t>(state,0xef8),
                                  At<uint64_t>(system,0xb0),At<uint64_t>(system,0xf0),At<uint32_t>(system,0x78));
                    greeneye::Host()->log(line);
                }
            }
            if(g_lagMax||g_lagMin!=~0ull){
                std::snprintf(line,sizeof(line),"Town Portal: live picture drawn %llu to %llu frames after it was taken",g_lagMin,g_lagMax);
                greeneye::Host()->log(line);
            }
            g_lagMin=~0ull;g_lagMax=0;
            std::snprintf(line,sizeof(line),"Town Portal: bake waiting (most in a frame / frames with any): entry0 %u/%u, entry1 %u/%u, entry2 %u/%u, entry3 %u/%u",
                          g_bakeMax[0],g_bakeFrames[0],g_bakeMax[1],g_bakeFrames[1],g_bakeMax[2],g_bakeFrames[2],g_bakeMax[3],g_bakeFrames[3]);
            greeneye::Host()->log(line);
            for(auto& v:g_bakeMax)v=0;
            for(auto& v:g_bakeFrames)v=0;
            for(auto& v:g_slowest)v=0;
            since=micro;frames=0;worst=0;renders=g_ownedRenderCalls;
        }
    }
#endif
    if(!g_target||!g_world||g_target->error||now<stepAt)return;
    const bool wanted=g_liveWantTick&&now-g_liveWantTick<500&&LiveAuto(now);
    if(wanted){
        if(!g_target->ticket)return; // the first visible portal allocates the compositor's images
        if(!g_target->view){
            // 2 for the scene, 2 for its water and 1 for attaching it to the world (LiveRoom).
            if(g_world->reservedSlots<4||g_world->scene!=reinterpret_cast<uint64_t>(scene)||!LiveRoom(scene))return;
            CreateOwnedView(scene);
            stepAt=now+250;
        }
        else if(!g_world->ownedBound&&g_world->worldBound){
            if(!QueueCapacity(scene,1))return;
            g_worldBind(At<void*>(g_target->view,0x98),g_world->binding);
            g_world->ownedBound=1;
            greeneye::Host()->log("Town Portal: live view: second camera attached to the world");
            stepAt=now+250;
        }
    }
    else if(g_target->view&&now>=g_displayProbeUntil&&g_liveWantTick&&now-g_liveWantTick>600000&&LiveAuto(now)){
        ReleaseOwnedView();
        greeneye::Host()->log("Town Portal: live view: second camera released (no portal on screen for 10 minutes)");
        stepAt=now+1000;
    }
}
void LiveLifecycleGuard(void* scene) {
    NoteGraphicsLock();
    __try {LiveLifecycle(scene);}
    __except(TargetFault(GetExceptionInformation())) {}
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

bool MapCameraEnds(std::array<uint8_t,0x420>* out,const void* input,const PortalEndpoint& from,const PortalEndpoint& to);
bool MapCamera(std::array<uint8_t,0x420>* out,const void* input,const PortalInstance* portal,bool field) {
    return MapCameraEnds(out,input,field?portal->fieldEnd:portal->baseEnd,field?portal->baseEnd:portal->fieldEnd);
}
// The render input with its camera carried from one endpoint to another: standing d in front of
// `from` and looking into it becomes standing d behind `to`, looking out along its normal.
bool MapCameraEnds(std::array<uint8_t,0x420>* out,const void* input,const PortalEndpoint& from,const PortalEndpoint& to) {
    __try {
        std::memcpy(out->data(),input,out->size());
        auto* data=out->data();
        FixedPos fixed{};
        std::memcpy(&fixed,data+0x38,sizeof(fixed));
        math::Vec3 camera{};
        ToFloat(fixed,camera.x,camera.y,camera.z);
        if (!math::Finite(camera)||camera.x<100||camera.x>12000||camera.z<100||camera.z>12000) return false;
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

// The part of the player's screen the portal's opening covered when the second camera was last
// aimed (0..1). A small portal on screen gets a live frame every second or third game frame:
// the second camera costs nearly half the frame rate at full pace, and at that size the
// steps do not show (the portal follows the eye between frames anyway).
std::atomic<float> g_liveCoverage{1.f};
// Turns the carried camera (render input at pose: position +0x38, rotation +0x50, vertical field
// of view +0x78) to frame the part of the far endpoint's opening that is on the player's screen,
// so every pixel of the second camera's frame is spent on what the portal shows: the whole
// opening from afar, a small part of it from close by (framing the whole opening there left a
// few hundred pixels stretched over the screen, 2026-11-09). right, up, forward: the player's
// camera in the world; tanY its half field of view (tangent), aspect its width over height.
// The position stays the viewer's eye carried through the portal, so what it sees through the
// opening is unchanged; the portal finds its pixels by the camera the frame was taken with.
bool AimAtExit(uint8_t* pose,math::Vec3 right,math::Vec3 up,math::Vec3 forward,float tanY,float aspect,
               const PortalEndpoint& from,const PortalEndpoint& to) {
    FixedPos fixed{};std::memcpy(&fixed,pose+0x38,sizeof(fixed));
    math::Vec3 eye{};ToFloat(fixed,eye.x,eye.y,eye.z);
    if(!math::Finite(right)||!math::Finite(up)||!math::Finite(forward)||!(tanY>0.01f)||!(aspect>0.1f))return false;
    // The player's camera axes on the far side.
    const math::Vec3 fr{from.rx,0,from.rz},fn{from.nx,0,from.nz},tr{to.rx,0,to.rz},tn{to.nx,0,to.nz};
    math::Vec3 r=math::MapPortalDirection(right,fr,fn,tr,tn),u=math::MapPortalDirection(up,fr,fn,tr,tn);
    math::Vec3 f=math::MapPortalDirection(forward,fr,fn,tr,tn);
    // Those axes are the frame before's (the world view's matrices), while the pose is this
    // frame's: as the player turned, the part framed was the part seen a frame ago and the
    // newly seen edge fell outside the picture, where the portal shows a blurred stand-in
    // (smears while turning, 2026-11-09). The carried pose's own rotation is this frame's:
    // each axis is replaced by the axis of that rotation it lies along.
    {
        float q[4]{};std::memcpy(q,pose+0x50,sizeof(q));
        const float x=q[0],y=q[1],z=q[2],w=q[3];
        const math::Vec3 axes[6]={
            {1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)},{2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)},{2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)},
            {1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)},{2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)},{2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)}};
        float bests[3]{};int picks[3]{-1,-1,-1};int which=0;
        auto snap=[&](math::Vec3& axis){
            float best=0.9f;math::Vec3 pick=axis;
            for(int i=0;i<6;++i){
                const float d=math::Dot(axis,axes[i]);
                if(std::fabs(d)>best){best=std::fabs(d);pick=axes[i]*(d<0?-1.f:1.f);picks[which]=d<0?-(i+1):i+1;}
            }
            bests[which++]=best;
            axis=pick;
        };
        snap(r);snap(u);snap(f);
        static uint64_t told=0;
        if(const uint64_t now=GetTickCount64();now-told>5000){
            told=now;char line[200]{};
            std::snprintf(line,sizeof(line),"Town Portal: aim axes: right %d (%.4f), up %d (%.4f), forward %d (%.4f); q %.3f %.3f %.3f %.3f",
                          picks[0],bests[0],picks[1],bests[1],picks[2],bests[2],x,y,z,w);
            greeneye::Host()->log(line);
        }
    }
    // The opening (1.19 m to each side, 1.65 m up and down) in that camera's tangent plane,
    // kept to the screen.
    const math::Vec3 centre{to.wx,to.wy+1.62f,to.wz};
    const float limitX=tanY*aspect,limitY=tanY;
    // The opening's outline (32 points), cut where it passes behind the camera: the part in
    // front is what can be on screen. (Taking the whole screen as soon as any point was behind
    // spread the frame over all of a 32:9 screen whenever the viewer stood close, and the far
    // side came out blurred, 2026-11-09.)
    float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f;int kept=0;
    constexpr int kOutline=32;constexpr float kFront=0.05f;
    math::Vec3 outline[kOutline]{};
    for(int i=0;i<kOutline;++i){
        const float a=float(i)*(6.2831853f/float(kOutline));
        const math::Vec3 d=centre+tr*(std::cos(a)*1.19f)+math::Vec3{0,std::sin(a)*1.65f,0}-eye;
        outline[i]={math::Dot(d,r),math::Dot(d,u),math::Dot(d,f)};
    }
    auto take=[&](const math::Vec3& c){
        const float x=c.x/c.z,y=c.y/c.z;
        minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);++kept;
    };
    for(int i=0;i<kOutline;++i){
        const math::Vec3& p0=outline[i];const math::Vec3& p1=outline[(i+1)%kOutline];
        if(p0.z>=kFront)take(p0);
        if((p0.z>=kFront)!=(p1.z>=kFront)){
            const float t=(kFront-p0.z)/(p1.z-p0.z);
            take(p0+(p1-p0)*t);
        }
    }
    if(!kept)return false;
    minX=std::max(minX,-limitX);maxX=std::min(maxX,limitX);minY=std::max(minY,-limitY);maxY=std::min(maxY,limitY);
    if(!(maxX-minX>1e-4f)||!(maxY-minY>1e-4f))return false; // not on screen
    // How much of the player's screen the opening takes (for how often to take a live frame).
    g_liveCoverage=(maxX-minX)*(maxY-minY)/(4.f*limitX*limitY);
    const float cx=0.5f*(minX+maxX),cy=0.5f*(minY+maxY);
    math::Vec3 goal=f+r*cx+u*cy;
    goal=goal*(1.f/std::sqrt(math::LengthSq(goal)));
    // The new camera's own axes (level with the old one's), and the kept part's reach in them.
    math::Vec3 side{u.y*goal.z-u.z*goal.y,u.z*goal.x-u.x*goal.z,u.x*goal.y-u.y*goal.x};
    const float sideLength=std::sqrt(math::LengthSq(side));
    if(!(sideLength>1e-4f))return false;
    side=side*(1.f/sideLength);
    const math::Vec3 lift{goal.y*side.z-goal.z*side.y,goal.z*side.x-goal.x*side.z,goal.x*side.y-goal.y*side.x};
    float reachX=0,reachY=0;
    for(int i=0;i<4;++i){
        const math::Vec3 d=f+r*((i&1)?maxX:minX)+u*((i&2)?maxY:minY);
        const float z=math::Dot(d,goal);
        if(z<1e-3f)continue;
        reachX=std::max(reachX,std::fabs(math::Dot(d,side))/z);reachY=std::max(reachY,std::fabs(math::Dot(d,lift))/z);
    }
    // The shortest turn taking the carried forward to the goal, applied in world space.
    const float d=math::Dot(f,goal);
    if(d<-0.999f)return false;
    const math::Vec3 axis{f.y*goal.z-f.z*goal.y,f.z*goal.x-f.x*goal.z,f.x*goal.y-f.y*goal.x};
    float a[4]={axis.x,axis.y,axis.z,1.f+d};
    const float norm=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]+a[3]*a[3]);
    if(!(norm>1e-6f))return false;
    for(float& v:a)v/=norm;
    float q[4]{};std::memcpy(q,pose+0x50,sizeof(q));
    const float turned[4]={a[3]*q[0]+a[0]*q[3]+a[1]*q[2]-a[2]*q[1],
                           a[3]*q[1]-a[0]*q[2]+a[1]*q[3]+a[2]*q[0],
                           a[3]*q[2]+a[0]*q[1]-a[1]*q[0]+a[2]*q[3],
                           a[3]*q[3]-a[0]*q[0]-a[1]*q[1]-a[2]*q[2]};
    std::memcpy(pose+0x50,turned,sizeof(turned));
    // Tall enough for the kept part's height, and for its width in the second camera's frame;
    // a fifth more for the rim and for the viewer moving between frames.
    ChooseOwnedSize(reachX*1.2f,reachY*1.2f,d);
    const float frame=float(g_ownedSize.wantWidth)/float(g_ownedSize.wantHeight);
    const float fov=std::clamp(2.f*std::atan(std::max(reachY,reachX/frame)*1.2f),0.08f,1.9f);
    std::memcpy(pose+0x78,&fov,sizeof(fov));
    return true;
}

void* ProjectionDescHook(void* output,const void* camera,float aspect) {
    // Retain the already-installed descriptor hook across hot reloads. Native
    // culling/effects also consume this descriptor: leave its type/near/far intact.
    // The second camera's frame is not always the shape of its largest size (ApplyOwnedSize).
    if(g_ownedAspectNow>0&&std::fabs(aspect-float(kOwnedWidth)/float(kOwnedHeight))<2e-3f)aspect=g_ownedAspectNow;
    return g_projectionDesc(output,camera,aspect);
}
void ProjectionMatrixHook(float* output,const void* description) {
    const bool ownedCall=g_clipScope.active&&_ReturnAddress()==g_projectionCaller;
    greeneye::Lease lease;
    g_projectionMatrix(output,description);
    if(lease&&ownedCall&&*static_cast<const uint32_t*>(description)==0){
        if(g_clipScope.nearDistance>0){
            // The near plane moved out to the exit, keeping the projection an ordinary one
            // (column-major, reverse depth: depth = [10] z + [14] over w = z, so the near plane
            // is at z = [14] / (1 - [10]); both scale with it while the far plane is far off).
            // The oblique clip at the exit's own plane (ClipReverseZ) bent the depth values,
            // and the game's brick pass, which works its depth out from an ordinary
            // projection, then came out in front of the terrain meshes: far walls and cliffs
            // showed as plain-coloured bricks (2026-11-09).
            const float denominator=1.f-output[10];
            const float nearNow=std::fabs(denominator)>1e-6f?output[14]/denominator:0.f;
            if(std::isfinite(nearNow)&&nearNow>1e-4f&&g_clipScope.nearDistance>nearNow){
                const float k=g_clipScope.nearDistance/nearNow;
                output[10]*=k;output[14]*=k;++g_exitClipFrames;
            } else ++g_exitClipRejected;
        }
        else if(math::ClipReverseZ(output,g_clipScope.plane))++g_exitClipFrames;
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
        if(destination&&g_exitClipEnabled&&!g_farShot.active){ // a snapshot frame looks away from the exit
            // Where the view leads, the way round the snapshots have it (portal_types.h Turned).
            const PortalEndpoint end=g_nowField?g_nowPortal.baseEnd:g_nowPortal.fieldEnd.Turned();
            FixedPos fixed{};std::memcpy(&fixed,mapped+0x38,sizeof(fixed));
            math::Vec3 position{};ToFloat(fixed,position.x,position.y,position.z);
            float q[4]{};std::memcpy(q,mapped+0x50,sizeof(q));
            const math::Vec3 normal{end.nx,0,end.nz},center{end.wx,end.wy+1.62f,end.wz};
            g_clipScope.plane={math::InverseRotate(normal,q),math::Dot(normal,position-center)-0.015f};
            g_clipScope.active=true;
            // The near plane's distance: the nearest point of the exit's opening that this
            // camera has in its frame (outline points, and where the frame's centre and
            // corners meet the opening), a little less. Camera space: z forward.
            static uint64_t obliqueChecked=0;static bool oblique=false;
            if(const uint64_t now=GetTickCount64();now-obliqueChecked>1000){
                obliqueChecked=now;
                oblique=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\exit-clip.oblique")!=INVALID_FILE_ATTRIBUTES;
            }
            if(!oblique){
                float fov=1.f;std::memcpy(&fov,mapped+0x78,sizeof(fov));
                const float tanY=std::tan(std::clamp(fov,0.05f,2.8f)*0.5f)*1.05f,tanX=tanY*OwnedAspect();
                const math::Vec3 right{end.rx,0,end.rz};
                float nearest=1e9f;
                for(int i=0;i<32;++i){
                    const float a=float(i)*0.19634954f;
                    const math::Vec3 world=center+right*(std::cos(a)*1.19f)+math::Vec3{0,std::sin(a)*1.65f,0};
                    const math::Vec3 c=math::InverseRotate(world-position,q);
                    if(c.z>0.02f&&std::fabs(c.x)<=tanX*c.z&&std::fabs(c.y)<=tanY*c.z)nearest=std::min(nearest,c.z);
                }
                const math::Vec3 planeNormal=g_clipScope.plane.normal;
                const math::Vec3 planePoint=math::InverseRotate(center-position,q);
                const math::Vec3 rightCamera=math::InverseRotate(right,q),upCamera=math::InverseRotate(math::Vec3{0,1,0},q);
                for(int i=0;i<5;++i){
                    const math::Vec3 ray{i==4?0.f:((i&1)?tanX:-tanX),i==4?0.f:((i&2)?tanY:-tanY),1.f};
                    const float along=math::Dot(ray,planeNormal);
                    if(std::fabs(along)<1e-5f)continue;
                    const float t=math::Dot(planePoint,planeNormal)/along;
                    if(!(t>0.02f))continue;
                    const math::Vec3 hit=ray*t-planePoint;
                    const float u=math::Dot(hit,rightCamera)/1.19f,v=math::Dot(hit,upCamera)/1.65f;
                    if(u*u+v*v<=1.f)nearest=std::min(nearest,t);
                }
                if(nearest<1e8f)g_clipScope.nearDistance=std::max(0.05f,nearest-0.05f);
            }
        }
        g_ownedAspectNow=OwnedAspect();
        g_ownedEffectsTicked=false;
        // (A test that pointed this scene's effect system, +0x950 and its link in the view part,
        // at the player's for the far render hung the graphics driver for good, 2026-11-10: not
        // a way to get the altar's flame.)
        // The near plane is given to the game in the camera itself (render input +0x70; +0x74 is
        // the far plane, +0x78 the field of view), not only scaled into the projection matrix
        // afterwards: the game's effects work depth out from the camera's near and far, and with
        // the matrix alone moved out (0.01 m in the camera, metres in the matrix) every flame,
        // spark and smoke puff counted as behind the scenery and was faded to nothing: the far
        // view never showed one (2026-11-10). tools/near.matrix (dev) keeps the old way.
        uint8_t withNear[0x420];
        const uint8_t* pose=mapped;
        if(g_clipScope.nearDistance>0&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/near.matrix")==INVALID_FILE_ATTRIBUTES){
            std::memcpy(withNear,mapped,sizeof(withNear));
            float nearNow=0,farNow=0;std::memcpy(&nearNow,withNear+0x70,4);std::memcpy(&farNow,withNear+0x74,4);
            if(std::isfinite(nearNow)&&nearNow>0&&nearNow<1.f&&farNow>g_clipScope.nearDistance*4.f){
                const float moved=g_clipScope.nearDistance;
                std::memcpy(withNear+0x70,&moved,4);
                pose=withNear;
            }
        }
        CallWithCamera(g_next,result,g_target->view,context,input,target,pose,true);
        g_ownedEffectsTicked=true; // its own wipe is over; the player's render comes next
    } __finally {g_clipScope=saved;g_ownedAspectNow=0;}
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
// Dev: counts a terrain mesher's tree nodes (0x68 bytes each: children +0x10, their count
// +0x18, state +0x28, level +0x48; FUN_1401e7220) by level and by state, to compare the second
// camera's tree with the player's.
struct TreeCount {uint32_t level[8]{},state[8]{},flag22{},flag23{},flag24{},nodes{};};
void CountNodes(uint8_t* node,TreeCount& count,int depth) {
    if(depth>8||count.nodes>200000)return;
    ++count.nodes;
    ++count.level[std::min<uint32_t>(At<uint32_t>(node,0x48),7)];
    ++count.state[std::min<uint32_t>(At<uint32_t>(node,0x28),7)];
    count.flag22+=At<uint8_t>(node,0x22)!=0;count.flag23+=At<uint8_t>(node,0x23)!=0;count.flag24+=At<uint8_t>(node,0x24)!=0;
    auto* children=At<uint8_t*>(node,0x10);
    const uint64_t n=At<uint64_t>(node,0x18);
    if(!children||n>8)return;
    for(uint64_t i=0;i<n;++i)CountNodes(children+i*0x68,count,depth+1);
}
void DescribeTree(const char* who,uint8_t* mesher) {
    __try {
        auto* lock=reinterpret_cast<CRITICAL_SECTION*>(mesher+0x338);
        if(!TryEnterCriticalSection(lock))return;
        TreeCount count{};
        auto* roots=At<uint8_t*>(mesher,0x390);
        const uint64_t n=At<uint64_t>(mesher,0x398);
        if(roots&&n<=4096&&!At<uint8_t>(mesher,0x368))for(uint64_t i=0;i<n;++i)CountNodes(roots+i*0x68,count,0);
        LeaveCriticalSection(lock);
        char line[300]{};
        std::snprintf(line,sizeof(line),"Town Portal: %s terrain tree: %u nodes; by level %u %u %u %u %u %u %u %u; by state %u %u %u %u %u %u %u %u; flags %u %u %u",
                      who,count.nodes,count.level[0],count.level[1],count.level[2],count.level[3],count.level[4],count.level[5],count.level[6],count.level[7],
                      count.state[0],count.state[1],count.state[2],count.state[3],count.state[4],count.state[5],count.state[6],count.state[7],
                      count.flag22,count.flag23,count.flag24);
        greeneye::Host()->log(line);
        // The settings the mesher was last updated with (FUN_1401e6760 copies 0x28 bytes to +0xc478).
        std::snprintf(line,sizeof(line),"Town Portal: %s mesher settings: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f (%u) %u; c420 %u c424 %u c428 %u c429 %u c42a %u c42b %u c42c %u c42d %u c430 %u c434 %u",
                      who,At<float>(mesher,0xc478),At<float>(mesher,0xc47c),At<float>(mesher,0xc480),At<float>(mesher,0xc484),
                      At<float>(mesher,0xc488),At<float>(mesher,0xc48c),At<float>(mesher,0xc490),At<float>(mesher,0xc494),
                      At<float>(mesher,0xc498),At<uint32_t>(mesher,0xc498),At<uint32_t>(mesher,0xc49c),
                      At<uint32_t>(mesher,0xc420),At<uint32_t>(mesher,0xc424),unsigned(At<uint8_t>(mesher,0xc428)),unsigned(At<uint8_t>(mesher,0xc429)),
                      unsigned(At<uint8_t>(mesher,0xc42a)),unsigned(At<uint8_t>(mesher,0xc42b)),unsigned(At<uint8_t>(mesher,0xc42c)),unsigned(At<uint8_t>(mesher,0xc42d)),
                      At<uint32_t>(mesher,0xc430),At<uint32_t>(mesher,0xc434));
        greeneye::Host()->log(line);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void RefreshDestinationTerrain() {
    const auto now=GetTickCount64();
    if(!g_haveModelFocus||!g_terrainRequest||(now>=g_displayProbeUntil&&now>=g_liveUntil+2000)||now-g_gameplayHudTick.load()>=250)return;
    terrain::Bounds bounds{};
    if(!terrain::DestinationBounds(g_modelFocus,bounds))return;
    auto* state=At<void*>(g_target->view,0xa0);
    auto* mesher=At<uint8_t*>(state,0x978);
    if(!mesher)return;
    // Dev: where each view's terrain mesher takes its detail from (its focus, floats at +0x3b0;
    // FUN_1401e7220 splits a node by its distance from there), every 5 s.
    {
        static uint64_t told=0;
        if(now-told>5000){
            told=now;
            auto* worldState=static_cast<uint8_t*>(WorldState());
            auto* worldMesher=worldState?At<uint8_t*>(worldState,0x978):nullptr;
            char line[260]{};
            std::snprintf(line,sizeof(line),"Town Portal: mesher focus: second camera %.1f %.1f %.1f (on %u, quality %.2f), player's %.1f %.1f %.1f (on %u, quality %.2f); second camera at %.1f %.1f %.1f",
                          At<float>(mesher,0x3b0),At<float>(mesher,0x3b4),At<float>(mesher,0x3b8),unsigned(At<uint8_t>(mesher,0xc428)),At<float>(mesher,0xc438),
                          worldMesher?At<float>(worldMesher,0x3b0):0.f,worldMesher?At<float>(worldMesher,0x3b4):0.f,worldMesher?At<float>(worldMesher,0x3b8):0.f,
                          worldMesher?unsigned(At<uint8_t>(worldMesher,0xc428)):0u,worldMesher?At<float>(worldMesher,0xc438):0.f,
                          g_modelFocus.x,g_modelFocus.y,g_modelFocus.z);
            greeneye::Host()->log(line);
            std::snprintf(line,sizeof(line),"Town Portal: view feature words: second camera %08x %08x, player's %08x %08x",
                          At<uint32_t>(state,0x10),At<uint32_t>(state,0x14),worldState?At<uint32_t>(worldState,0x10):0u,worldState?At<uint32_t>(worldState,0x14):0u);
            greeneye::Host()->log(line);
            // What internal feature 16 made for each view (state +0x988), first 0x60 bytes.
            for(int who=0;who<2;++who){
                auto* st=who?worldState:static_cast<uint8_t*>(state);
                auto* made=st?At<uint8_t*>(st,0x988):nullptr;
                int n=std::snprintf(line,sizeof(line),"Town Portal: %s feature-16 object %p flags 0x%x:",who?"player's":"second camera's",static_cast<void*>(made),who?0u:g_ownedRenderFlags);
                __try {
                    if(made)for(int i=0;i<0x60&&n<240;i+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(made,i));
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
                greeneye::Host()->log(line);
            }
            // Both meshers' pointer fields (+0x360 .. +0x440), once a minute, to spot one the second
            // camera's lacks.
            {
                static uint64_t dumped=0;
                if(now-dumped>60000){
                    dumped=now;
                    for(int who=0;who<2;++who){
                        auto* m=who?worldMesher:mesher;
                        if(!m)continue;
                        for(int row=0;row<4;++row){
                            int n=std::snprintf(line,sizeof(line),"Town Portal: %s mesher +0x%x:",who?"player's":"second camera's",0x360+row*0x38);
                            for(int i=0;i<7;++i)n+=std::snprintf(line+n,sizeof(line)-n," %llx",At<uint64_t>(m,0x360+row*0x38+i*8));
                            greeneye::Host()->log(line);
                        }
                    }
                }
            }
            for(int who=0;who<2;++who){
                auto* m=who?worldMesher:mesher;
                if(!m)continue;
                int n=std::snprintf(line,sizeof(line),"Town Portal: %s mesher +0xc440:",who?"player's":"second camera's");
                for(int o=0xc440;o<0xc478;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(m,o));
                n+=std::snprintf(line+n,sizeof(line)-n," | +0x3e0: %08x %08x | +0xd4c8: %08x %08x %08x %08x %08x %08x",At<uint32_t>(m,0x3e0),At<uint32_t>(m,0x3e4),
                                 At<uint32_t>(m,0xd4c8),At<uint32_t>(m,0xd4cc),At<uint32_t>(m,0xd4d0),At<uint32_t>(m,0xd4d4),At<uint32_t>(m,0xd4d8),At<uint32_t>(m,0xd4dc));
                greeneye::Host()->log(line);
            }
            // The terrain material bake's list (root +0x16a78, next at +0x10; FUN_1401189d0 bakes an
            // entry only when the view state it points to at +0x40 has its byte +0x970 set and
            // the entry's count at +0xc0 is not 0).
            __try {
                auto* root=reinterpret_cast<uint8_t*>(g_world->root);
                int index=0;
                for(auto* item=At<uint8_t*>(root,0x16a78);item&&index<8;item=At<uint8_t*>(item,0x10),++index){
                    auto* st=At<uint8_t*>(item,0x40);
                    FixedPos at{};if(st)std::memcpy(&at,st+0x958,sizeof(at));
                    math::Vec3 pos{};ToFloat(at,pos.x,pos.y,pos.z);
                    std::snprintf(line,sizeof(line),"Town Portal: bake entry %d %p: scene %p, view state %p (second camera's %p, player's %p), on %u, at %.1f %.1f %.1f, count %u, waiting %lld",
                                  index,static_cast<void*>(item),At<void*>(item,0),static_cast<void*>(st),static_cast<void*>(state),static_cast<void*>(worldState),
                                  st?unsigned(At<uint8_t>(st,0x970)):99u,pos.x,pos.y,pos.z,At<uint32_t>(item,0xc0),
                                  (long long)((At<uint8_t*>(item,0x98)-At<uint8_t*>(item,0x90))));
                    greeneye::Host()->log(line);
                }
                std::snprintf(line,sizeof(line),"Town Portal: view states +0x970: second camera %u, player's %u",unsigned(At<uint8_t>(state,0x970)),worldState?unsigned(At<uint8_t>(worldState,0x970)):99u);
                greeneye::Host()->log(line);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
            for(int who=0;who<2;++who){
                auto* st=who?worldState:static_cast<uint8_t*>(state);
                if(!st)continue;
                int n=std::snprintf(line,sizeof(line),"Town Portal: %s view state +0x40:",who?"player's":"second camera's");
                for(int o=0x40;o<0xb0;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(st,o));
                n+=std::snprintf(line+n,sizeof(line)-n," | +0x1120: %llx %llx %llx %llx",At<uint64_t>(st,0x1120),At<uint64_t>(st,0x1128),At<uint64_t>(st,0x1130),At<uint64_t>(st,0x1138));
                greeneye::Host()->log(line);
                {   // The effect pictures (state +0x120..+0x220) as the mod's own image reader sees them.
                    // (FUN_141030c60 on these handles jumped to a null address, 2026-11-10: not for them.)
                    char fx[600]{};int m=std::snprintf(fx,sizeof(fx),"Town Portal: %s effect pictures: parity %u, frame %ux%u / %ux%u;",who?"player's":"second camera's",
                        At<uint32_t>(st,0x850),At<uint32_t>(st,0x9c),At<uint32_t>(st,0xa0),At<uint32_t>(st,0xa4),At<uint32_t>(st,0xa8));
                    for(int o=0x120;o<=0x220;o+=0x10){
                        const auto info=InspectImage(At<ImageView>(st,o));
                        m+=std::snprintf(fx+m,sizeof(fx)-m," +0x%x %ux%u f%u%s",o,info.width,info.height,info.format,info.valid?"":"!");
                    }
                    greeneye::Host()->log(fx);
                }
                {   // Effects: the view's output sizes and buffers, and its effect part's emitters.
                    char fx[700]{};int m=std::snprintf(fx,sizeof(fx),"Town Portal: %s effects: state +0xb0:",who?"player's":"second camera's");
                    for(int o=0xb0;o<0xd8;o+=4)m+=std::snprintf(fx+m,sizeof(fx)-m," %x",At<uint32_t>(st,o));
                    m+=std::snprintf(fx+m,sizeof(fx)-m," | +0xb50:");
                    for(int o=0xb50;o<0xb80;o+=8)m+=std::snprintf(fx+m,sizeof(fx)-m," %llx",At<uint64_t>(st,o));
                    auto* part=At<uint8_t*>(st,0x9a8);
                    if(part&&!IsBadReadPtr(part,0x7a0)){
                        m+=std::snprintf(fx+m,sizeof(fx)-m," | part %p system %p, +0x38.. %llx %llx, emitters +0xc8.. %llx %llx, active +0xf8.. %llx %llx, parity %u",
                            static_cast<void*>(part),At<void*>(part,0),At<uint64_t>(part,0x38),At<uint64_t>(part,0x40),At<uint64_t>(part,0xc8),At<uint64_t>(part,0xd0),At<uint64_t>(part,0xf8),At<uint64_t>(part,0x100),At<uint32_t>(part,0x79c));
                    }
                    greeneye::Host()->log(fx);
                }
            }
            // Dev test: tools\state6d copies the player's view state byte +0x6d (1 there, 0 here)
            // to the second camera's, to see what it switches.
            if(worldState&&GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\state6d")!=INVALID_FILE_ATTRIBUTES)
                At<uint8_t>(state,0x6d)=At<uint8_t>(worldState,0x6d);
            for(int who=0;who<2;++who){
                auto* st=who?worldState:static_cast<uint8_t*>(state);
                if(!st)continue;
                int n=std::snprintf(line,sizeof(line),"Town Portal: %s view state +0xec0:",who?"player's":"second camera's");
                for(int o=0xec0;o<0xf40;o+=8)n+=std::snprintf(line+n,sizeof(line)-n," %llx",At<uint64_t>(st,o));
                greeneye::Host()->log(line);
            }
            DescribeTree("second camera's",mesher);
            if(worldMesher)DescribeTree("player's",worldMesher);
        }
    }
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
    NoteGraphicsLock();
    __try {
        // Render and sample the same retained graph resource. Importing the
        // physical texture again hides the write/read dependency from the graph.
        // The native BeginFrame uploads every scene's pending data. Updating
        // here, alongside the primary view, keeps those uploads ahead of draw.
        g_ownedPrepared=g_sceneReady(At<void*>(g_target->view,0x98))!=0;

    } __except(TargetFault(GetExceptionInformation())) { g_renderOnce=false; }
}
// Where a remembered object with this model at this place is kept (or would be).
uint64_t RememberHash(const uint64_t guid[2],math::Vec3 at) {
    const int64_t qx=int64_t(std::floor(at.x*4.f+0.5f)),qy=int64_t(std::floor(at.y*4.f+0.5f)),qz=int64_t(std::floor(at.z*4.f+0.5f));
    uint64_t h=guid[0]^(guid[1]*0x9e3779b97f4a7c15ull)^uint64_t(qx*73856093)^uint64_t(qy*19349663)^uint64_t(qz*83492791);
    h^=h>>29;h*=0xbf58476d1ce4e5b9ull;h^=h>>32;
    return h;
}
int FindRemembered(const uint64_t guid[2],math::Vec3 at) {
    if(!g_objectMemory)return -1;
    const uint64_t h=RememberHash(guid,at);
    for(uint32_t probe=0;probe<24;++probe){
        const uint32_t slot=uint32_t((h+probe)%kRememberSlots);
        const auto& o=g_objectMemory->slots[slot];
        if(!o.used)return -1;
        if(o.used!=3&&o.guid[0]==guid[0]&&o.guid[1]==guid[1]&&std::fabs(o.x-at.x)<=0.2f&&std::fabs(o.y-at.y)<=0.2f&&std::fabs(o.z-at.z)<=0.2f)return int(slot);
    }
    return -1;
}
// Animated things (characters, creatures, players, pets) in the second camera's scene.
//
// The game draws one as a group: a skeleton group in the model scene (pool at scene +0x3b0,
// records of 0x98 bytes; the record proper starts 8 bytes in: +0 the skeleton resource, whose
// first 16 bytes are its GUID; +8 where its joints start in the scene's pose array (scene
// +0x8c0, 0x30 bytes a joint); +0xc how many joints; +0x48 its first model instance, each
// instance's +0x100 the next) and the model instances bound to it (instance +0x130 = the
// group's handle). The static mirroring above leaves those instances out. Here every group
// near the second camera gets a twin in the second camera's scene, made with the game's own
// calls: a group from the same skeleton (FUN_1400fd680), the same models bound to it
// (FUN_140112b60), and every tick the player's scene's joint poses copied across
// (FUN_140113260) and the instances moved. The game only has such things near the player,
// so this shows them when both ends of the portal are near each other (and the player
// themself, seen through their own portal). tools\actors.disabled (dev) turns it off.
using GroupCreateFn=uint32_t(*)(void* scene,const void* guid);
using GroupBindFn=void(*)(void* scene,const NativeSpan* instances,uint64_t group);
using GroupPoseFn=void(*)(void* scene,uint32_t group,const NativeSpan* pose);
using GroupDestroyFn=void(*)(void* scene,uint32_t group);
GroupCreateFn g_groupCreate{};GroupBindFn g_groupBind{};GroupPoseFn g_groupPose{};GroupDestroyFn g_groupDestroy{};
uint8_t* GroupRecord(uint8_t* scene,uint32_t handle) {
    if(!handle)return nullptr;
    const uint32_t low=handle&0xffff;
    const uint64_t page=low>>(At<uint64_t>(scene,0x3e8)&0x3f),slot=At<uint64_t>(scene,0x3f8)&low;
    if(page>=At<uint64_t>(scene,0x3c8))return nullptr;
    auto* pageRecord=At<uint8_t*>(scene,0x3c0)+page*0x40;
    if(slot>=At<uint64_t>(pageRecord,0x10))return nullptr;
    auto* entry=At<uint8_t*>(pageRecord,8)+slot*0x98;
    if((At<uint32_t>(entry,0)&0x7fffffff)!=handle>>16)return nullptr;
    return entry+8;
}
void DropActor(void* owned,ActorRecord& a) {
    for(uint32_t i=0;i<a.parts;++i)if(a.part[i].owned){NativeSpan ids{&a.part[i].owned,1};g_instanceRemove(owned,&ids);}
    if(a.ownedGroup)g_groupDestroy(owned,a.ownedGroup);
    a={};--g_actors->count;++g_actors->removed;
}
// An instance's switch at +0x13c (the game sets it with FUN_140112dd0): on for an altar's bowl
// whose flame burns. A copy made without it had its model's effect but dark: the flame's light
// came from elsewhere and no fire showed in the far view (2026-11-09). Copies follow it.
using InstanceFlagFn=void(*)(void* scene,uint32_t handle,uint8_t on);
InstanceFlagFn g_instanceFlag{};
void FollowInstanceFlag(uint8_t* ownedScene,uint32_t ownedHandle,const uint8_t* original) {
    if(!g_instanceFlag||!ownedHandle)return;
    const uint32_t index=ownedHandle&0xffffff;
    auto* instances=At<uint8_t*>(ownedScene,0x1b0);
    if(!instances||index>=At<uint64_t>(ownedScene,0x1d0))return;
    const uint8_t want=original[0x13c];
    if(At<uint8_t>(instances+size_t(index)*0x1b0,0x13c)!=want)g_instanceFlag(ownedScene,ownedHandle,want);
}
// What the game sets on an instance after making it, which a twin made from the model alone
// lacks: the player's character came out with wrong colours in the far view (grey suit and
// hair, no glowing lines, 2026-11-10). The game's setters for these (FUN_140112540 +0x1a4[3],
// _1127a0.. +0x84, _1128f0 +0x88 four floats, _112a40 +0x74, _112a80 +0x1a0, _112ad0 +0x78,
// _112e10 +0x7c, and +0x80) each write the field and call FUN_1400f85d0(scene, instance, 2),
// which marks the instance changed (+0x110) and lists it for the scene's next update. Here
// the twin gets the original's values the same way. The mark call is taken from the flag
// setter (its call at +0x2e). tools/params.off (dev) leaves it out.
using InstanceMarkFn=void(*)(void* scene,uint8_t* instance,uint8_t what);
InstanceMarkFn g_instanceMark{};
uint64_t g_paramCopies{};
void FollowInstanceParams(uint8_t* ownedScene,uint32_t ownedHandle,const uint8_t* original) {
    if(!g_instanceMark||!ownedHandle)return;
    static uint64_t checked=0;static bool off=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/params.off")!=INVALID_FILE_ATTRIBUTES;}
    if(off)return;
    const uint32_t index=ownedHandle&0xffffff;
    auto* instances=At<uint8_t*>(ownedScene,0x1b0);
    if(!instances||index>=At<uint64_t>(ownedScene,0x1d0))return;
    auto* twin=instances+size_t(index)*0x1b0;
    if(At<void*>(twin,0x108)!=At<void*>(const_cast<uint8_t*>(original),0x108))return;
    const bool same=std::memcmp(twin+0x74,original+0x74,4)==0&&twin[0x78]==original[0x78]
        &&std::memcmp(twin+0x7c,original+0x7c,0x1c)==0&&std::memcmp(twin+0x1a0,original+0x1a0,0x10)==0
        &&std::memcmp(twin+0x14c,original+0x14c,0x51)==0;
    if(same)return;
    std::memcpy(twin+0x74,original+0x74,4);twin[0x78]=original[0x78];
    std::memcpy(twin+0x7c,original+0x7c,0x1c);std::memcpy(twin+0x1a0,original+0x1a0,0x10);
    // Up to four entries of 0x14 bytes (a GUID and a strength) at +0x14c, how many at +0x19c
    // (FUN_140112600): the player's parts have one or two, and with them their real colours.
    if(original[0x19c]<=4)std::memcpy(twin+0x14c,original+0x14c,0x51);
    g_instanceMark(ownedScene,twin,2);
    if(++g_paramCopies<=5||g_paramCopies%500==0){
        char line[120]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view: a twin given its original's settings (%llu so far)",g_paramCopies);
        greeneye::Host()->log(line);
    }
}
// A part that can be twinned: it has a model with meshes, and the model has no cloth and no
// chains. A part with cloth (a cape) gets its cloth made with it, but nothing simulates that
// in the second scene, and the scene's own pass over it faulted (a null read at exe+0x11df820
// right after the player's character, cape and all, was twinned, 2026-11-09).
// Animated things with cloth are twinned too (an NPC whose one model has cloth was left out of
// the far view; with this it was twinned and ran without a fault, 2026-11-10; dev:
// tools/cloth.off leaves them out again). The fault of 2026-11-09 was
// the cloth pass (FUN_1400ee380, exe+0xefd44) reading a cloth piece's simulation object
// (piece +0, pieces of 0x68 bytes at instance +0xa0, count +0xa8) that was null, so a twin is
// checked for that when made and every tick, before the scene's tick, and dropped for good.
bool g_clothTwins{true};
uint32_t g_badCloth[32]{};uint32_t g_badClothNext{};
bool OwnedClothSound(uint8_t* ownedScene,uint32_t handle) {
    const uint32_t index=handle&0xffffff;
    auto* instances=At<uint8_t*>(ownedScene,0x1b0);
    if(!instances||index>=At<uint64_t>(ownedScene,0x1d0))return true;
    auto* instance=instances+size_t(index)*0x1b0;
    const uint32_t count=At<uint32_t>(instance,0xa8);
    auto* pieces=At<uint8_t*>(instance,0xa0);
    if(!count)return true;
    if(!pieces||count>64)return false;
    for(uint32_t i=0;i<count;++i)if(!At<void*>(pieces+size_t(i)*0x68,0))return false;
    return true;
}
bool ActorPartEligible(uint8_t* instance,bool actor=false) {
    auto* model=At<uint8_t*>(instance,0x108);
    if(actor&&g_clothTwins)return model&&At<uint32_t>(instance,0x114);
    // Cloth, as the game tells it (FUN_1400e8be0): the model has a cloth resource (+0x60) or
    // its data (+0x38) lists cloth parts (+0xe0). The create call then makes cloth pieces
    // (instance +0xa0 / +0xa8), which the scene's pass FUN_1400ee380 walks.
    if(!model||!At<uint32_t>(instance,0x114))return false;
    auto* data=At<uint8_t*>(model,0x38);
    if(At<uint64_t>(model,0x60)||(data&&At<int32_t>(data,0xe0)))return false;
    return !At<uint64_t>(instance,0xa8)&&!At<uint64_t>(instance,0xc0)&&!At<uint64_t>(instance,0xd8);
}
void SyncOwnedActors() {
    if(!g_actors||!g_groupCreate||!g_groupBind||!g_groupPose||!g_groupDestroy||!g_haveModelFocus)return;
    auto* source=At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0x98);
    auto* owned=At<uint8_t*>(g_target->view,0x98);
    if(!source||!owned||!g_sceneReady(owned))return;
    static uint64_t checked=0;static bool disabled=false;
    const uint64_t now=GetTickCount64();
    if(now-checked>1000){checked=now;disabled=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\actors.disabled")!=INVALID_FILE_ATTRIBUTES;g_clothTwins=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/cloth.off")==INVALID_FILE_ATTRIBUTES;}
    const auto capacity=At<uint64_t>(source,0x1d0);
    auto* generations=At<uint8_t*>(source,0x1c8);
    auto* instances=At<uint8_t*>(source,0x1b0);
    auto* poses=At<uint8_t*>(source,0x8c0);
    if(capacity>65536||!generations||!instances||!poses)return;
    auto handleOf=[&](uint8_t* instance)->uint32_t {
        if(instance<instances)return 0;
        const uint64_t index=uint64_t(instance-instances)/0x1b0;
        if(index>=capacity||instances+index*0x1b0!=instance)return 0;
        return (uint32_t(generations[index])<<24)|uint32_t(index);
    };
    {   // Dev: tools/actor.dump -> both instances of every twinned part, once per file.
        static uint64_t dumpChecked=0;
        if(now-dumpChecked>2000){
            dumpChecked=now;
            if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/actor.dump")!=INVALID_FILE_ATTRIBUTES){
                DeleteFileW(L"C:/Mods/Enshrouded/TownPortal/tools/actor.dump");
                if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/actor-dump.txt","w")){
                    auto* ownInstances=At<uint8_t*>(owned,0x1b0);
                    for(auto& a:g_actors->records)if(a.ownedGroup)for(uint32_t i=0;i<a.parts;++i){
                        const uint32_t si=a.part[i].source&0xffffff,oi=a.part[i].owned&0xffffff;
                        if(si>=capacity||!ownInstances||oi>=At<uint64_t>(owned,0x1d0))continue;
                        uint8_t* pair[2]={instances+size_t(si)*0x1b0,ownInstances+size_t(oi)*0x1b0};
                        for(int w=0;w<2;++w){
                            std::fprintf(f,"%s group %u part %u at %p model %p:",w?"twin":"orig",a.sourceGroup,i,pair[w],At<void*>(pair[w],0x108));
                            for(size_t o=0;o<0x1b0;o+=4)std::fprintf(f," %08x",At<uint32_t>(pair[w],o));
                            std::fprintf(f,"%c",10);
                        }
                    }
                    std::fclose(f);
                }
            }
        }
    }
    // The twins there are: still the same group with the same parts, and near? Then pose and move.
    for(auto& a:g_actors->records)if(a.ownedGroup){
        auto* record=disabled?nullptr:GroupRecord(source,a.sourceGroup);
        bool keep=record&&At<uint8_t*>(record,0)&&std::memcmp(At<uint8_t*>(record,0),a.guid,16)==0;
        uint32_t seen=0;
        if(keep){
            for(auto* instance=At<uint8_t*>(record,0x48);instance&&seen<=kActorParts;instance=At<uint8_t*>(instance,0x100)){
                if(!ActorPartEligible(instance,true))continue;
                const uint32_t handle=handleOf(instance);
                bool known=false;
                for(uint32_t i=0;i<a.parts;++i)if(a.part[i].source==handle){
                    known=true;
                    FollowInstanceFlag(owned,a.part[i].owned,instance);
                    FollowInstanceParams(owned,a.part[i].owned,instance);
                    if(std::memcmp(a.part[i].transform,instance+0x38,56)!=0){
                        NativeSpan ids{&a.part[i].owned,1},transform{instance+0x38,1};
                        g_instanceTransform(owned,&ids,&transform,0);
                        std::memcpy(a.part[i].transform,instance+0x38,56);
                    }
                    break;
                }
                if(!known){keep=false;break;}
                ++seen;
            }
            if(seen!=a.parts)keep=false;
        }
        if(keep)for(uint32_t i=0;i<a.parts;++i)if(!OwnedClothSound(owned,a.part[i].owned)){
            keep=false;g_badCloth[g_badClothNext++%32]=a.sourceGroup;
            greeneye::Host()->log("Town Portal: live view: a twin's cloth lost its simulation object; dropped");
            break;
        }
        if(keep&&a.parts){
            FixedPos fixed{};std::memcpy(&fixed,a.part[0].transform,sizeof(fixed));
            math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
            if(!math::Finite(at)||math::LengthSq(at-g_modelFocus)>140.0f*140.0f)keep=false;
        }
        if(!keep){DropActor(owned,a);continue;}
        // The pose, when both groups have their joints (the twin's come with its skeleton).
        auto* twin=GroupRecord(owned,a.ownedGroup);
        const uint32_t joints=At<uint32_t>(record,0xc);
        if(twin&&joints&&joints<=1024&&At<uint32_t>(twin,0xc)==joints){
            NativeSpan pose{poses+size_t(At<uint32_t>(record,8))*0x30,joints};
            g_groupPose(owned,a.ownedGroup,&pose);
            ++g_actors->posed;
        }
    }
    if(disabled)return;
    // One new twin a tick: the nearest group without one within 120 m of the second camera.
    static uint64_t nextCreate=0;
    if(now<nextCreate)return;
    nextCreate=now+50;
    const uint64_t pages=At<uint64_t>(source,0x3c8);
    const uint64_t shift=At<uint64_t>(source,0x3e8)&0x3f;
    auto* pageRecords=At<uint8_t*>(source,0x3c0);
    if(!pageRecords||pages>256)return;
    // Groups that could not be twinned (too many parts, or a part's model not there yet) are
    // left alone for a while.
    static uint32_t skipped[32]{};static uint64_t skippedAt=0;static uint32_t skippedNext=0;
    if(now-skippedAt>10000){skippedAt=now;for(auto& v:skipped)v=0;}
    auto skip=[&](uint32_t handle){skipped[skippedNext++%32]=handle;};
    uint32_t best=0;float bestDistance=120.0f*120.0f;
    // Dev: why groups are or are not twinned, every 10 s.
    static uint64_t told=0;const bool tell=now-told>10000;
    uint32_t groups=0,inRange=0,parts=0,noModel=0,noMesh=0,clothModel=0,clothPieces=0,other=0;
    for(uint64_t page=0;page<pages;++page){
        auto* entries=At<uint8_t*>(pageRecords+page*0x40,8);
        const uint64_t slots=At<uint64_t>(pageRecords+page*0x40,0x10);
        if(!entries||slots>65536)continue;
        for(uint64_t slot=0;slot<slots;++slot){
            auto* entry=entries+slot*0x98;
            const uint32_t header=At<uint32_t>(entry,0);
            if(header&0x80000000u)continue;
            auto* record=entry+8;
            auto* first=At<uint8_t*>(record,0x48);
            if(!At<uint8_t*>(record,0)||!first||!At<uint32_t>(record,0xc))continue;
            const uint32_t handle=(header<<16)|uint32_t((page<<shift)|slot);
            if(GroupRecord(source,handle)!=record)continue; // the handle as the game forms it
            bool have=false;
            for(const auto& a:g_actors->records)if(a.ownedGroup&&a.sourceGroup==handle){have=true;break;}
            for(uint32_t v:skipped)if(v==handle){have=true;break;}
            for(uint32_t v:g_badCloth)if(v==handle){have=true;break;}
            if(have)continue;
            FixedPos fixed{};std::memcpy(&fixed,first+0x38,sizeof(fixed));
            math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
            if(!math::Finite(at))continue;
            const float d=math::LengthSq(at-g_modelFocus);
            ++groups;
            if(tell&&d<120.0f*120.0f){
                ++inRange;uint32_t walked=0;
                for(auto* instance=first;instance&&walked<64;instance=At<uint8_t*>(instance,0x100),++walked){
                    ++parts;auto* model=At<uint8_t*>(instance,0x108);
                    if(!model)++noModel;
                    else if(!At<uint32_t>(instance,0x114))++noMesh;
                    else if(At<uint64_t>(model,0x60)||(At<uint8_t*>(model,0x38)&&At<int32_t>(At<uint8_t*>(model,0x38),0xe0)))++clothModel;
                    else if(At<uint64_t>(instance,0xa8))++clothPieces;
                    else if(At<uint64_t>(instance,0xc0)||At<uint64_t>(instance,0xd8))++other;
                }
            }
            if(d<bestDistance){bestDistance=d;best=handle;}
        }
    }
    if(tell){
        told=now;char line[240]{};
        std::snprintf(line,sizeof(line),"Town Portal: animated groups without a twin: %u, within 120 m %u with %u parts (no model %u, no meshes %u, cloth model %u, cloth pieces %u, other %u); twins %u",
                      groups,inRange,parts,noModel,noMesh,clothModel,clothPieces,other,g_actors->count);
        greeneye::Host()->log(line);
#ifdef TOWN_PORTAL_DEV
        // Dev: the twins with a real skeleton (creatures, people; swaying plants have ~4 joints):
        // joints and metres from the far end, to tell whether enemies reach this game at all.
        char list[400]{};size_t used=0;uint32_t plants=0;
        for(const auto& a:g_actors->records){
            if(!a.ownedGroup)continue;
            auto* record=GroupRecord(source,a.sourceGroup);
            if(!record)continue;
            const uint32_t joints=At<uint32_t>(record,0xc);
            if(joints<20){++plants;continue;}
            float d=-1;
            if(auto* first=At<uint8_t*>(record,0x48)){
                FixedPos fixed{};std::memcpy(&fixed,first+0x38,sizeof(fixed));
                math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                if(math::Finite(at))d=std::sqrt(math::LengthSq(at-g_modelFocus));
            }
            if(used+24<sizeof(list))used+=size_t(std::snprintf(list+used,sizeof(list)-used," %uj@%.0fm",joints,d));
        }
        std::snprintf(line,sizeof(line),"Town Portal (dev): twins with skeletons:%s; %u small (plants)",used?list:" none",plants);
        greeneye::Host()->log(line);
#endif
    }
    if(!best)return;
    auto* record=GroupRecord(source,best);
    ActorRecord* slot=nullptr;
    for(auto& a:g_actors->records)if(!a.ownedGroup){slot=&a;break;}
    // All twins in use (a busy base has more than 150 animated things within 120 m): the one
    // farthest from the second camera makes way for a nearer one, so whoever stands by the far
    // end (the player, other players) is always among them. Full, the player was not shown
    // (user, 2026-11-10).
    if(!slot){
        ActorRecord* farthest=nullptr;float farthestDistance=0;
        for(auto& a:g_actors->records){
            if(!a.ownedGroup)continue;
            auto* r=GroupRecord(source,a.sourceGroup);auto* first=r?At<uint8_t*>(r,0x48):nullptr;
            if(!first)continue;
            FixedPos fixed{};std::memcpy(&fixed,first+0x38,sizeof(fixed));
            math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
            const float d=math::Finite(at)?math::LengthSq(at-g_modelFocus):1e30f;
            if(d>farthestDistance){farthestDistance=d;farthest=&a;}
        }
        if(farthest&&std::sqrt(farthestDistance)>std::sqrt(bestDistance)+5.f){DropActor(owned,*farthest);slot=farthest;}
    }
    if(!record||!slot)return;
    uint32_t eligible=0,walked=0;
    for(auto* instance=At<uint8_t*>(record,0x48);instance&&walked<256;instance=At<uint8_t*>(instance,0x100),++walked)
        if(ActorPartEligible(instance,true))++eligible;
    if(!eligible||eligible>kActorParts||walked>=256){skip(best);return;}
    ActorRecord made{};
    made.sourceGroup=best;std::memcpy(made.guid,At<uint8_t*>(record,0),16);
    made.ownedGroup=g_groupCreate(owned,made.guid);
    if(!made.ownedGroup){skip(best);return;}
    uint32_t ids[kActorParts]{};bool failed=false;
    for(auto* instance=At<uint8_t*>(record,0x48);instance&&made.parts<kActorParts;instance=At<uint8_t*>(instance,0x100)){
        if(!ActorPartEligible(instance,true))continue;
        const uint32_t created=g_instanceCreate(owned,At<uint8_t*>(instance,0x108)+0x18,instance+0x38);
        if(!created){failed=true;break;}
        if(!OwnedClothSound(owned,created)){
            NativeSpan one{&created,1};g_instanceRemove(owned,&one);failed=true;
            g_badCloth[g_badClothNext++%32]=best;
            greeneye::Host()->log("Town Portal: live view: an animated thing's cloth came without its simulation object; left out");
            break;
        }
        auto& part=made.part[made.parts];
        part.source=handleOf(instance);part.owned=created;std::memcpy(part.transform,instance+0x38,56);
        ids[made.parts++]=created;
    }
    if(failed||made.parts!=eligible){
        for(uint32_t i=0;i<made.parts;++i){NativeSpan one{&made.part[i].owned,1};g_instanceRemove(owned,&one);}
        g_groupDestroy(owned,made.ownedGroup);skip(best);return;
    }
    NativeSpan span{ids,made.parts};
    g_groupBind(owned,&span,made.ownedGroup);
    *slot=made;++g_actors->count;++g_actors->created;
    char line[200]{};
    std::snprintf(line,sizeof(line),"Town Portal: live view: animated thing %u mirrored (%u parts, %u joints, %.0f m from the second camera); %u in all",
                  g_actors->created,made.parts,At<uint32_t>(record,0xc),std::sqrt(bestDistance),g_actors->count);
    greeneye::Host()->log(line);
}
// The world's own plants in the second camera's scene.
//
// Trees and bushes that came with the world are not model instances: each scene draws them
// from the world's data with a loader of its own (scene +0xa90: 256 m cells of 0x48 bytes at
// +0x20, +0x28 by +0x2c of them; a cell's data at +0x18, its chunk handle at +0x40; the scene
// it belongs to at +0x10). What is gone (cleared for a base, felled) is only hidden, per
// scene: a bit per plant (chunk system, scene +0xa30: bits at +0x550, changed marks at +0x568
// and +0x580; a chunk's entry of 0x110 bytes has its first bit at +0xe4, how many at +0xe8,
// its kind at +0xf0, 1 for these). The game hides them in the player's scene only, so the far
// view showed trees that were cleared long ago, at the altar (2026-11-10). Two ways in:
// the build zones (FUN_140112760(scene, boxes, no-build boxes), which keeps the boxes in the
// loader, +0x30 / +0x38 and +0x2040 / +0x2048, and hides what stands in them, also in cells
// that load later), and one plant at a time (FUN_140112780). Here the second scene's loader
// is given the player's boxes, and where both scenes have a cell loaded its bits follow the
// player's. tools/plants.off (dev) leaves it out.
using ZonesFn=void(*)(void* scene,const void* boxes,const void* noBuild);
ZonesFn g_plantZones{};
uint8_t* PlantChunk(uint8_t* chunks,uint32_t handle) {
    if(!handle)return nullptr;
    const uint32_t low=handle&0xffff;
    const uint64_t block=low>>(At<uint64_t>(chunks,0x40)&0x3f),slot=At<uint64_t>(chunks,0x50)&low;
    if(block>=At<uint64_t>(chunks,0x20))return nullptr;
    auto* blocks=At<uint8_t*>(chunks,0x18)+block*0x40;
    if(slot>=At<uint64_t>(blocks,0x10))return nullptr;
    auto* entry=At<uint8_t*>(blocks,8)+slot*0x110;
    return (At<uint32_t>(entry,0)&0x7fffffff)==handle>>16?entry:nullptr;
}
void SyncOwnedPlants(uint8_t* source,uint8_t* owned) {
    static uint64_t next=0,checked=0,zonesHash=0,hidden=0,shown=0;static bool off=false;static uint8_t* zonesFor=nullptr;
    const uint64_t now=GetTickCount64();
    if(now<next)return;
    next=now+500;
    if(now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/plants.off")!=INVALID_FILE_ATTRIBUTES;}
    if(off)return;
    auto* from=At<uint8_t*>(source,0xa90);auto* to=At<uint8_t*>(owned,0xa90);
    if(!from||!to||from==to)return;
    if(At<uint8_t*>(from,0x10)!=source||At<uint8_t*>(to,0x10)!=owned)return;
    // The boxes.
    if(g_plantZones){
        struct Span{const uint8_t* data;uint64_t count;};
        const Span boxes{At<uint8_t*>(from,0x30),At<uint64_t>(from,0x38)},noBuild{At<uint8_t*>(from,0x2040),At<uint64_t>(from,0x2048)};
        if(boxes.count<=256&&noBuild.count<=256&&(boxes.data||!boxes.count)&&(noBuild.data||!noBuild.count)){
            uint64_t hash=1469598103934665603ull;
            auto mix=[&](const uint8_t* d,size_t n){for(size_t i=0;i<n;++i){hash^=d[i];hash*=1099511628211ull;}};
            mix(reinterpret_cast<const uint8_t*>(&boxes.count),8);mix(boxes.data,boxes.count*0x20);
            mix(reinterpret_cast<const uint8_t*>(&noBuild.count),8);mix(noBuild.data,noBuild.count*0x20);
            if(hash!=zonesHash||zonesFor!=to){
                zonesHash=hash;zonesFor=to;
                g_plantZones(owned,&boxes,&noBuild);
                char line[160]{};
                std::snprintf(line,sizeof(line),"Town Portal: live view: the far scene given the build zones (%llu, %llu no-build); cleared plants are hidden there",boxes.count,noBuild.count);
                greeneye::Host()->log(line);
            }
        }
    }
    // The bits, where both have the cell.
    auto* fromChunks=At<uint8_t*>(source,0xa30);auto* toChunks=At<uint8_t*>(owned,0xa30);
    const uint32_t cells=At<uint32_t>(from,0x28)*At<uint32_t>(from,0x2c);
    if(!fromChunks||!toChunks||fromChunks==toChunks||cells!=At<uint32_t>(to,0x28)*At<uint32_t>(to,0x2c)||cells>16384)return;
    auto* fromCells=At<uint8_t*>(from,0x20);auto* toCells=At<uint8_t*>(to,0x20);
    auto* fromBits=At<uint64_t*>(fromChunks,0x550);auto* toBits=At<uint64_t*>(toChunks,0x550);auto* toMarks=At<uint64_t*>(toChunks,0x568);
    if(!fromCells||!toCells||!fromBits||!toBits||!toMarks)return;
    uint64_t hid=0,showed=0;
    for(uint32_t c=0;c<cells;++c){
        auto* a=fromCells+size_t(c)*0x48;auto* b=toCells+size_t(c)*0x48;
        if(!At<void*>(a,0x18)||!At<void*>(b,0x18))continue;
        auto* ea=PlantChunk(fromChunks,At<uint32_t>(a,0x40));auto* eb=PlantChunk(toChunks,At<uint32_t>(b,0x40));
        if(!ea||!eb||At<uint8_t>(ea,0xf0)!=1||At<uint8_t>(eb,0xf0)!=1)continue;
        const uint32_t count=At<uint32_t>(ea,0xe8);
        if(count!=At<uint32_t>(eb,0xe8)||count>200000)continue;
        const uint32_t fa=At<uint32_t>(ea,0xe4),fb=At<uint32_t>(eb,0xe4);
        for(uint32_t i=0;i<count;++i){
            const uint32_t ia=fa+i,ib=fb+i;
            const bool want=(fromBits[ia>>6]>>(ia&63))&1,have=(toBits[ib>>6]>>(ib&63))&1;
            if(want==have)continue;
            if(want){toBits[ib>>6]|=1ull<<(ib&63);++showed;}else{toBits[ib>>6]&=~(1ull<<(ib&63));++hid;}
            const uint64_t before=toMarks[ib>>13],after=before|1ull<<((ib>>7)&63);
            toMarks[ib>>13]=after;
            if(before!=after)++At<int32_t>(toChunks,0x580);
        }
    }
    if(hid||showed){
        hidden+=hid;shown+=showed;
        char line[160]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view: the world's plants in the far scene: %llu hidden, %llu shown again (%llu and %llu in all)",hid,showed,hidden,shown);
        greeneye::Host()->log(line);
    }
}
// Effects in the second camera's scene.
//
// The game keeps running effects (an altar's flame, smoke, sparks) in an effect system per model
// scene (scene +0x950): entries of 0xc0 bytes (array at +0x50 and +0x60, how many at +0x58),
// the running ones chained from +0x70 (index + 1; an entry's +4 is the next): +8 its handle,
// +0x18 where it is (0x38 bytes), +0x68 flags, +0x70 which effect (an index into the table
// both systems share at +8: slots keyed by the effect resource's GUID at +0x1c00, index by slot
// at +0x1c18, slot bitmap at +0x1be0, slot count at +0x1be8), +0x74 its 0x40 bytes of settings.
// The game only starts effects in the player's scene, so the far view had none: no flame on
// the altar (2026-11-09). Here every running effect within 60 m of the far endpoint gets a twin
// started with the game's own call (FUN_140197de0) from the same GUID, place and settings,
// kept in place while its original runs and stopped (FUN_14019c9f0) when that is gone.
// tools\effects.disabled (dev) turns it off.
using EffectCreateFn=uint32_t*(*)(uint32_t* handle,void* system,const void* guid,const void* transform,const void* settings);
using EffectDestroyFn=void(*)(void* system,uint32_t handle);
EffectCreateFn g_effectCreate{};EffectDestroyFn g_effectDestroy{};
uint8_t* EffectEntry(uint8_t* system,uint32_t handle) {
    if(!handle||(handle&0xffff)>=At<uint64_t>(system,0x58))return nullptr;
    auto* entry=At<uint8_t*>(system,0x50)+size_t(handle&0xffff)*0xc0;
    return At<uint32_t>(entry,8)==handle?entry:nullptr;
}
void SyncOwnedEffects() {
    if(!g_effects||!g_effectCreate||!g_effectDestroy||!g_liveView.since)return;
    static uint64_t next=0,checked=0;static bool disabled=false,told=false;
    const uint64_t now=GetTickCount64();
    if(now<next)return;
    next=now; // every frame: effects that move (a wand's flame) follow at once
    if(now-checked>1000){checked=now;disabled=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\effects.disabled")!=INVALID_FILE_ATTRIBUTES;}
    auto* sourceScene=At<uint8_t*>(reinterpret_cast<void*>(g_world->scene),0x98);
    auto* ownedScene=At<uint8_t*>(g_target->view,0x98);
    if(!sourceScene||!ownedScene)return;
    auto* source=At<uint8_t*>(sourceScene,0x950);
    auto* owned=At<uint8_t*>(ownedScene,0x950);
    if(!source||!owned||source==owned)return;
    {   // Every effect system starts its random numbers from the same constant (42), so the
        // n-th effect made in the far scene gets the same seed as the n-th in the player's, and
        // what the shared effect manager keeps from frame to frame may be found by it. The far
        // system gets a start of its own before its first twin. tools/fx.seed.off (dev) leaves it.
        static uint8_t* seeded=nullptr;
        if(seeded!=owned){
            seeded=owned;
            if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.seed.off")==INVALID_FILE_ATTRIBUTES){
                At<uint32_t>(owned,0x10)^=0x9e3779b9u;At<uint32_t>(owned,0x14)^=0x7f4a7c15u;
                greeneye::Host()->log("Town Portal: live view: the far scene's effects get random numbers of their own");
            }
        }
    }
    auto* table=At<uint8_t*>(source,8);
    if(!table||table!=At<uint8_t*>(owned,8)){
        if(!told){told=true;greeneye::Host()->log("Town Portal: live view: the two scenes' effect systems share no effect table; no effects in the far view");}
        return;
    }
    {   // Dev: both effect systems' heads every 10 s (does the far one's clock run?).
        static uint64_t toldAt=0;
        if(now-toldAt>10000){
            toldAt=now;
            for(int who=0;who<2;++who){
                auto* sys=who?source:owned;char line[400]{};
                int n=std::snprintf(line,sizeof(line),"Town Portal: %s effect system:",who?"player's":"second camera's");
                for(int o=0x10;o<0x80;o+=8)n+=std::snprintf(line+n,sizeof(line)-n," %llx",At<uint64_t>(sys,o));
                n+=std::snprintf(line+n,sizeof(line)-n," | +0x828 %x",At<uint32_t>(sys,0x828));
                greeneye::Host()->log(line);
            }
        }
    }
    const math::Vec3 spot{g_liveView.destination.wx,g_liveView.destination.wy,g_liveView.destination.wz};
    auto isNear=[&](uint8_t* entry){
        FixedPos fixed{};std::memcpy(&fixed,entry+0x18,sizeof(fixed));
        math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
        return math::Finite(at)&&math::LengthSq(at-spot)<60.0f*60.0f;
    };
    // The twins there are: original still running and near? Then follow it; else stop the twin.
    for(auto& r:g_effects->records)if(r.owned){
        auto* original=disabled?nullptr:EffectEntry(source,r.source);
        auto* twin=EffectEntry(owned,r.owned);
        {   // Dev: originals within 2.5 m of the far endpoint's altar side, with their twins, once a minute.
            static uint64_t dumpAt=0;static uint32_t dumped=0;
            if(now-dumpAt>60000){dumpAt=now;dumped=0;}
            if(original&&twin&&dumped<4){
                FixedPos fixed{};std::memcpy(&fixed,original+0x18,sizeof(fixed));
                math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                if(math::LengthSq(at-spot)<4.5f*4.5f){
                    ++dumped;
                    for(int who=0;who<2;++who){
                        auto* e=who?original:twin;char line[520]{};
                        int n=std::snprintf(line,sizeof(line),"Town Portal: effect %s kind %u at %.1f %.1f %.1f:",who?"original":"twin",At<uint32_t>(e,0x70),at.x,at.y,at.z);
                        for(int o=0;o<0xc0;o+=4){if(o>=0x18&&o<0x50)continue;n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(e,o));}
                        greeneye::Host()->log(line);
                    }
                }
            }
        }
        if(original&&twin&&isNear(original)&&At<uint32_t>(original,0x70)==At<uint32_t>(twin,0x70)){
            // With the game's own setters (FUN_1401a50b0 place, FUN_1401a4f00 settings: each
            // compares, writes and marks the effect for upload). Written straight into the entry,
            // a moved effect (the flame of a wand in the hand) was not uploaded and trailed
            // behind (2026-11-10). tools/fx.memcpy (dev) writes straight as before.
            if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.memcpy")==INVALID_FILE_ATTRIBUTES){
                // DEV: by fixed address in the game build of 2026-11-02 (Native() only knows functions
                // found by signature and gives null for others: a call through it jumped to address
                // 0). Before any release these two need signatures of their own; the bytes at the
                // two entries are checked so another build falls back to writing straight.
                auto* exe=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
                static const uint8_t headPlace[8]={0x48,0x83,0xec,0x28,0x4d,0x8b,0xc8,0x4c},headSettings[8]={0x85,0xd2,0x0f,0x84,0x07,0x01,0x00,0x00};
                if(std::memcmp(exe+0x1a50b0,headPlace,8)==0&&std::memcmp(exe+0x1a4f00,headSettings,8)==0){
                    reinterpret_cast<void(*)(void*,uint32_t,const void*)>(exe+0x1a50b0)(owned,r.owned,original+0x18);
                    reinterpret_cast<void(*)(void*,uint32_t,const void*)>(exe+0x1a4f00)(owned,r.owned,original+0x74);
                } else {
                    std::memcpy(twin+0x18,original+0x18,0x38);
                    std::memcpy(twin+0x74,original+0x74,0x40);
                }
            } else {
                std::memcpy(twin+0x18,original+0x18,0x38);
                std::memcpy(twin+0x74,original+0x74,0x40);
            }
            // The game's create call (FUN_140197de0) leaves entry +0x58..+0x6b as the pool had
            // them: in the far system that was old heap content (pieces of log text), where
            // the player's own entries have zeros (2026-11-10). The twin takes the original's.
            std::memcpy(twin+0x58,original+0x58,0x14);
            continue;
        }
        if(twin)g_effectDestroy(owned,r.owned);
        r={};--g_effects->count;++g_effects->removed;
    }
    {   static uint64_t fileAt=0;
        if(now-fileAt>5000){
            fileAt=now;
            {   // Dev: every running effect of both scenes into tools\effects-all.txt.
                if(HANDLE f=CreateFileW(L"C:/Mods/Enshrouded/TownPortal/tools/effects-all.txt",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);f!=INVALID_HANDLE_VALUE){
                    for(int who=0;who<2;++who){
                        auto* sys=who?owned:source;
                        const uint64_t c2=At<uint64_t>(sys,0x58);auto* a2=At<uint8_t*>(sys,0x60);uint32_t w2=0;
                        for(uint32_t link=At<uint32_t>(sys,0x70);a2&&link&&link<=c2&&w2<8192;++w2){
                            auto* e=a2+size_t(link-1)*0xc0;link=At<uint32_t>(e,4);
                            FixedPos fixed{};std::memcpy(&fixed,e+0x18,sizeof(fixed));
                            math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                            char row[240]{};const int len=std::snprintf(row,sizeof(row),"%s kind %u at %.1f %.1f %.1f handle %x flags %x %x group %x runtime %x age %llx\n",who?"owned":"player",
                                         At<uint32_t>(e,0x70),at.x,at.y,at.z,At<uint32_t>(e,8),At<uint32_t>(e,0x68),At<uint32_t>(e,0x6c),At<uint32_t>(e,0xb4),At<uint32_t>(e,0xb8),At<uint64_t>(e,0x50));
                            DWORD wrote{};WriteFile(f,row,DWORD(len),&wrote,nullptr);
                            // The altar's own effects (its flame is 1.85 m above it), whole.
                            const uint32_t kind=At<uint32_t>(e,0x70);
                            if((kind==1039||kind==1296)&&std::fabs(at.x-3777.6f)<1.5f&&std::fabs(at.z-1410.6f)<1.5f){
                                char hex[900]{};int h=std::snprintf(hex,sizeof(hex),"%s entry %u:",who?"owned":"player",kind);
                                for(size_t o=0;o<0xc0;o+=4)h+=std::snprintf(hex+h,sizeof(hex)-h," %x",At<uint32_t>(e,o));
                                hex[h++]=10;WriteFile(f,hex,DWORD(h),&wrote,nullptr);
                            }
                            // Its runtime record (view part = system +0x80: runtimes of 0x58 bytes at +0x38, index
                            // the low word of entry +0xb8) and emitter records (0xb8 bytes at part +0xc8; the
                            // runtime's emitter indices at +0x40, how many at +0x48), with the alive counts the
                            // game last read back for them (ids at emitter +0xa0.., FUN_1400aa860).
                            if((kind==1039||kind==1296)&&std::fabs(at.x-3777.6f)<1.5f&&std::fabs(at.z-1410.6f)<1.5f){
                                __try {
                                    auto* part=sys+0x80;
                                    const uint32_t ri=At<uint32_t>(e,0xb8)&0xffff;
                                    auto* runtimes=At<uint8_t*>(part,0x38);auto* emitters=At<uint8_t*>(part,0xc8);
                                    if(runtimes&&emitters&&ri<At<uint64_t>(part,0x40)){
                                        auto* r=runtimes+size_t(ri)*0x58;
                                        char t[1400]{};int n=std::snprintf(t,sizeof(t),"%s runtime %u of kind %u:",who?"owned":"player",ri,kind);
                                        for(size_t o=0;o<0x58;o+=4)n+=std::snprintf(t+n,sizeof(t)-n," %x",At<uint32_t>(r,o));
                                        t[n++]=10;WriteFile(f,t,DWORD(n),&wrote,nullptr);
                                        auto* list=At<uint32_t*>(r,0x40);const uint64_t count=At<uint64_t>(r,0x48);
                                        void* counters=At<void*>(reinterpret_cast<void*>(g_world->root),0x1a0);
                                        for(uint64_t k=0;list&&k<count&&k<16;++k){
                                            auto* em=emitters+size_t(list[k]&0xffff)*0xb8;
                                            n=std::snprintf(t,sizeof(t),"%s   emitter %x:",who?"owned":"player",list[k]);
                                            for(size_t o=0;o<0xb8;o+=4)n+=std::snprintf(t+n,sizeof(t)-n," %x",At<uint32_t>(em,o));
                                            n+=std::snprintf(t+n,sizeof(t)-n," | read back:");
                                            for(size_t o:{size_t(0xa0),size_t(0xa4),size_t(0xa8),size_t(0xb0),size_t(0xb4)}){
                                                const uint32_t id=At<uint32_t>(em,o);
                                                auto* manager=At<uint8_t*>(sys,8);
                                                auto* table=manager?At<uint8_t*>(manager,0x158):nullptr;
                                                auto* records=table?At<uint8_t*>(table,0x48):nullptr;
                                                auto* rec=id&&records?records+size_t(id&0xffff)*0x40:nullptr;
                                                if(rec&&!IsBadReadPtr(rec,0x40)&&At<uint32_t>(rec,0x38)==id)
                                                    n+=std::snprintf(t+n,sizeof(t)-n," +%zx: %x %x %x %x %x %x %x %x",o,At<uint32_t>(rec,0),At<uint32_t>(rec,4),At<uint32_t>(rec,8),At<uint32_t>(rec,0xc),At<uint32_t>(rec,0x10),At<uint32_t>(rec,0x14),At<uint32_t>(rec,0x18),At<uint32_t>(rec,0x1c));
                                                else n+=std::snprintf(t+n,sizeof(t)-n," +%zx: -",o);
                                            }
                                            t[n++]=10;WriteFile(f,t,DWORD(n),&wrote,nullptr);
                                        }
                                    }
                                } __except(EXCEPTION_EXECUTE_HANDLER) {}
                            }
                        }
                        {   // The system's own small numbers, and its view part's (state +0x9a8).
                            char hex[4000]{};int h=std::snprintf(hex,sizeof(hex),"%s system:",who?"owned":"player");
                            for(size_t o=0;o<0x200&&h<3900;o+=8){const uint64_t v=At<uint64_t>(sys,o);if(v&&v<0x10000000)h+=std::snprintf(hex+h,sizeof(hex)-h," %zx:%llx",o,v);}
                            auto* state=who?(g_target&&g_target->view?At<uint8_t*>(g_target->view,0xa0):nullptr):WorldState();
                            auto* part=state?At<uint8_t*>(state,0x9a8):nullptr;
                            h+=std::snprintf(hex+h,sizeof(hex)-h," | view part %p:",static_cast<void*>(part));
                            if(part&&!IsBadReadPtr(part,0x200))for(size_t o=0;o<0x200&&h<3900;o+=8){const uint64_t v=At<uint64_t>(part,o);if(v&&v<0x10000000)h+=std::snprintf(hex+h,sizeof(hex)-h," %zx:%llx",o,v);}
                            hex[h++]=10;DWORD wrote{};WriteFile(f,hex,DWORD(h),&wrote,nullptr);
                        }
                    }
                    for(int who=0;who<2;++who){
                        auto* scene=who?ownedScene:sourceScene;
                        const auto icap=At<uint64_t>(scene,0x1d0);auto* act=At<uint64_t*>(scene,0x208);auto* inst=At<uint8_t*>(scene,0x1b0);
                        for(uint32_t word=0;act&&inst&&word<(icap+63)/64;++word){
                            uint64_t bits=act[word];
                            while(bits){
                                unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
                                auto* in=inst+size_t(word*64+bit)*0x1b0;
                                FixedPos fixed{};std::memcpy(&fixed,in+0x38,sizeof(fixed));
                                math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                                if(!math::Finite(at)||math::LengthSq(at-spot)>=10.0f*10.0f)continue;
                                auto* model=At<uint8_t*>(in,0x108);
                                char row[240]{};const int len=std::snprintf(row,sizeof(row),"%s instance at %.1f %.1f %.1f group %x meshes %u model %016llx fog %x flags %x %x\n",who?"owned":"player",
                                    at.x,at.y,at.z,At<uint32_t>(in,0x130),At<uint32_t>(in,0x114),model?At<uint64_t>(model,0x18):0ull,At<uint32_t>(in,0x134),At<uint32_t>(in,0x110),At<uint32_t>(in,0x13c));
                                DWORD wrote{};WriteFile(f,row,DWORD(len),&wrote,nullptr);
                            }
                        }
                    }
                    CloseHandle(f);
                }
            }
        }
    }
    {   // Dev: every running effect of the player's scene within 8 m of the far endpoint, once a minute.
        static uint64_t listAt=0;
        if(now-listAt>60000){
            listAt=now;
            const uint64_t cap=At<uint64_t>(source,0x58);auto* all=At<uint8_t*>(source,0x60);
            uint32_t walked2=0,total=0,shown=0;
            for(uint32_t link=At<uint32_t>(source,0x70);all&&link&&link<=cap&&walked2<8192;++walked2){
                auto* e=all+size_t(link-1)*0xc0;link=At<uint32_t>(e,4);++total;
                FixedPos fixed{};std::memcpy(&fixed,e+0x18,sizeof(fixed));
                math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                const bool attached=At<uint32_t>(e,0xb4)!=0;
                static uint32_t attachedShown=0;if(walked2==0)attachedShown=0;
                const bool nearby=math::Finite(at)&&math::LengthSq(at-spot)<8.0f*8.0f;
                if(!nearby&&!(attached&&attachedShown<16))continue;
                if(!nearby)++attachedShown;
                if(shown>=40)continue;
                ++shown;bool twin=false;
                for(auto& r:g_effects->records)if(r.owned&&r.source==At<uint32_t>(e,8)){twin=true;break;}
                char line[220]{};
                std::snprintf(line,sizeof(line),"Town Portal: effect near the far end: kind %u at %.1f %.1f %.1f, flags %x %x, runtime %x, twin %d, group %x",
                              At<uint32_t>(e,0x70),at.x,at.y,at.z,At<uint32_t>(e,0x68),At<uint32_t>(e,0x6c),At<uint32_t>(e,0xb8),int(twin),At<uint32_t>(e,0xb4));
                greeneye::Host()->log(line);
            }
            char line[160]{};
            std::snprintf(line,sizeof(line),"Town Portal: effects running in the player's scene: %u (list says %u), far endpoint at %.1f %.1f %.1f",total,At<uint32_t>(source,0x78),spot.x,spot.y,spot.z);
            greeneye::Host()->log(line);
        }
    }
    if(disabled)return;
    // New ones: up to four a pass.
    uint32_t budget=16,walked=0;
    const uint64_t slots=At<uint64_t>(table,0x1be8);
    auto* bitmap=At<uint64_t*>(table,0x1be0);
    auto* keys=At<uint8_t*>(table,0x1c00);
    auto* indexBySlot=At<uint32_t*>(table,0x1c18);
    if(!bitmap||!keys||!indexBySlot||slots>65536)return;
    const uint64_t capacity=At<uint64_t>(source,0x58);
    auto* entries=At<uint8_t*>(source,0x60);
    if(!entries||capacity>65536)return;
    for(uint32_t link=At<uint32_t>(source,0x70);link&&link<=capacity&&walked<8192&&budget&&g_effects->count<kEffects;++walked){
        auto* entry=entries+size_t(link-1)*0xc0;
        link=At<uint32_t>(entry,4);
        const uint32_t handle=At<uint32_t>(entry,8);
        if(!handle||!isNear(entry))continue;
        bool have=false;EffectRecord* empty=nullptr;
        for(auto& r:g_effects->records){if(r.owned&&r.source==handle){have=true;break;}if(!r.owned&&!empty)empty=&r;}
        if(have||!empty)continue;
        // The effect's GUID: the table slot that leads to its index. Without one (never seen)
        // the game's call would stop the game on "corrupted vfx resource reference".
        const uint32_t type=At<uint32_t>(entry,0x70);
        const uint8_t* guid=nullptr;
        for(uint64_t slot=0;slot<slots;++slot)
            if((bitmap[slot>>6]>>(slot&63)&1)&&indexBySlot[slot]==type){guid=keys+slot*0x10;break;}
        if(!guid||(!At<uint64_t>(const_cast<uint8_t*>(guid),0)&&!At<uint64_t>(const_cast<uint8_t*>(guid),8))){++g_effects->refused;continue;}
        if(!At<uint64_t>(owned,0x28)){++g_effects->refused;break;} // the far system is full
        alignas(8) uint8_t settings[0x48]{};
        std::memcpy(settings,entry+0x74,0x40);
        settings[0x40]=1;                         // these settings, not the effect's defaults
        settings[0x44]=At<uint8_t>(entry,0x68)&1;
        uint32_t made=0;
        g_effectCreate(&made,owned,guid,entry+0x18,settings);
        --budget;
        if(!made){++g_effects->refused;continue;}
        if(auto* twin=EffectEntry(owned,made))std::memcpy(twin+0x58,entry+0x58,0x14);
        *empty={handle,made};++g_effects->count;++g_effects->created;
        if(g_effects->created<=12||g_effects->created%50==0){
            char line[160]{};
            std::snprintf(line,sizeof(line),"Town Portal: live view: effect %u twinned (kind %u); %u running in the far view",g_effects->created,type,g_effects->count);
            greeneye::Host()->log(line);
        }
    }
}
void SyncOwnedInstances() {
    // Native instance creation acquires a reference to the shared model asset;
    // transforms and instance handles belong exclusively to the owned scene.
    // No primary GPU buffer, skeleton, linked-list node or destructor is copied.
    // Every 15 ms: carried tools and weapons are static objects that move with their owner, and at ten times a second they trailed behind the copied character.
    if(!g_mirrors||!g_haveModelFocus||GetTickCount64()-g_mirrors->tick<15)return;
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
        if(At<uint32_t>(instance,0x130)||!ActorPartEligible(instance))return 1e30f;
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
    for(auto& entry:g_mirrors->records)if(entry.owned&&!entry.memory){
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
        if(keep){FollowInstanceFlag(static_cast<uint8_t*>(owned),entry.owned,instance);FollowInstanceParams(static_cast<uint8_t*>(owned),entry.owned,instance);}
    }
    uint32_t budget=8;
    for(size_t candidate=0;candidate<count&&budget&&g_mirrors->count<256;++candidate){
        const uint32_t index=candidates[candidate].index;
        const uint32_t handle=(uint32_t(generations[index])<<24)|index;
        MirrorRecord* empty=nullptr;bool exists=false;
        for(auto& entry:g_mirrors->records){if(entry.source==handle&&entry.owned&&!entry.memory){exists=true;break;}if(!entry.owned&&!empty)empty=&entry;}
        if(exists||!empty)continue;
        auto* instance=instances+size_t(index)*0x1b0;
        auto* guid=At<uint8_t*>(instance,0x108)+0x18;
        uint32_t created=g_instanceCreate(owned,guid,instance+0x38);--budget;
        if(created){empty->source=handle;empty->owned=created;std::memcpy(empty->guid,guid,16);std::memcpy(empty->transform,instance+0x38,56);++g_mirrors->count;++g_mirrors->created;}
    }
    // Dev: what each model scene holds (every 15 s): active instances, how many with a model,
    // skinned, without a model; and the first one without a model in full (terrain pieces?).
    {
        static uint64_t counted=0;
        if(const uint64_t now=GetTickCount64();now-counted>15000){
            counted=now;
            for(int who=0;who<2;++who){
                auto* scene=who?source:static_cast<uint8_t*>(owned);
                const auto cap=At<uint64_t>(scene,0x1d0);
                auto* act=At<uint64_t*>(scene,0x208);
                auto* inst=At<uint8_t*>(scene,0x1b0);
                if(cap>65536||!act||!inst)continue;
                uint32_t total=0,model=0,skinned=0,none=0,meshes=0;uint8_t* sample=nullptr;uint8_t* sampleModel=nullptr;uint8_t* skinnedSample=nullptr;
                for(uint32_t word=0;word<(cap+63)/64;++word){
                    uint64_t bits=act[word];
                    while(bits){
                        unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
                        const uint32_t index=word*64+bit;
                        if(index>=cap)break;
                        auto* i=inst+size_t(index)*0x1b0;
                        ++total;
                        if(At<uint32_t>(i,0x130)){++skinned;if(who&&!skinnedSample)skinnedSample=i;}
                        if(At<void*>(i,0x108)){++model;if(!sampleModel)sampleModel=i;}else{++none;if(!sample)sample=i;}
                        if(At<uint32_t>(i,0x114))++meshes;
                    }
                }
                char line[700]{};
                std::snprintf(line,sizeof(line),"Town Portal: %s model scene %p: %u active of %llu, %u with a model, %u skinned, %u without, %u with meshes",
                              who?"player's":"second camera's",static_cast<void*>(scene),total,cap,model,skinned,none,meshes);
                greeneye::Host()->log(line);
                // Dev: every instance within 3 m of the live view's far endpoint, once a minute: is
                // it one that gets mirrored, and does its model carry an effect (+0x6c) or more (+0x70)?
                {
                    static uint64_t toldNear[2]{};
                    if(const uint64_t t=GetTickCount64();t-toldNear[who]>60000&&g_liveView.since){
                        toldNear[who]=t;
                        const math::Vec3 spot{g_liveView.destination.wx,g_liveView.destination.wy,g_liveView.destination.wz};
                        __try {
                            int shown=0;
                            for(uint32_t word=0;word<(cap+63)/64&&shown<40;++word){
                                uint64_t bits=act[word];
                                while(bits&&shown<40){
                                    unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
                                    auto* i=inst+size_t(word*64+bit)*0x1b0;
                                    FixedPos fixed{};std::memcpy(&fixed,i+0x38,sizeof(fixed));
                                    math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                                    if(!math::Finite(at)||math::LengthSq(at-spot)>=7.0f*7.0f)continue;
                                    auto* model=At<uint8_t*>(i,0x108);
                                    ++shown;
                                    std::snprintf(line,sizeof(line),"Town Portal:   %s near the far end: at %.1f %.1f %.1f, group %x, meshes %u, model %016llx effect %x other %x cloth %d, instance effect %x / %x",
                                                  who?"player's":"second camera's",at.x,at.y,at.z,At<uint32_t>(i,0x130),At<uint32_t>(i,0x114),
                                                  model?At<uint64_t>(model,0x18):0ull,model?At<uint32_t>(model,0x6c):0u,model?At<uint32_t>(model,0x70):0u,
                                                  model?int(At<uint64_t>(model,0x60)!=0):-1,At<uint32_t>(i,0x134),At<uint32_t>(i,0x138));
                                    greeneye::Host()->log(line);
                                    {int n=std::snprintf(line,sizeof(line),"Town Portal:     its +0x70..0xa0 and +0x110..0x1b0:");
                                     for(int o=0x70;o<0xa0;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(i,o));
                                     n+=std::snprintf(line+n,sizeof(line)-n," |");
                                     for(int o=0x110;o<0x1b0;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(i,o));
                                     greeneye::Host()->log(line);}
                                }
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
                // Dev: the scene's effect and light systems, once a minute (what the far view lacks
                // for the altar's flame).
                {
                    static uint64_t toldVfx[2]{};
                    if(const uint64_t t=GetTickCount64();t-toldVfx[who]>60000){
                        toldVfx[who]=t;
                        __try {
                            std::snprintf(line,sizeof(line),"Town Portal: %s scene systems: +0x9d0 %p, +0x9f0 %p, +0x9f8 %p, +0xa30 %p, +0xa38 %p, +0xa40 %p, +0xa48 %p, +0x950 %p; pool +0x138: %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx",
                                          who?"player's":"second camera's",At<void*>(scene,0x9d0),At<void*>(scene,0x9f0),At<void*>(scene,0x9f8),At<void*>(scene,0xa30),At<void*>(scene,0xa38),At<void*>(scene,0xa40),At<void*>(scene,0xa48),At<void*>(scene,0x950),
                                          At<uint64_t>(scene,0x138),At<uint64_t>(scene,0x140),At<uint64_t>(scene,0x148),At<uint64_t>(scene,0x150),At<uint64_t>(scene,0x158),At<uint64_t>(scene,0x160),
                                          At<uint64_t>(scene,0x168),At<uint64_t>(scene,0x170),At<uint64_t>(scene,0x178),At<uint64_t>(scene,0x180),At<uint64_t>(scene,0x188),At<uint64_t>(scene,0x190));
                            greeneye::Host()->log(line);
                            for(uint32_t offset:{0x9f8u,0xa38u}){
                                auto* system=At<uint8_t*>(scene,offset);
                                if(!system)continue;
                                for(int row=0;row<3;++row){
                                    int n=std::snprintf(line,sizeof(line),"Town Portal:   %s +0x%x system +0x%02x:",who?"player's":"second camera's",offset,row*0x60);
                                    for(int o=0;o<0x60;o+=8)n+=std::snprintf(line+n,sizeof(line)-n," %llx",At<uint64_t>(system,row*0x60+o));
                                    greeneye::Host()->log(line);
                                }
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
                // The scene's brick tree (model scene +0xa80; FUN_14005afa0 sets its focus at +0x160
                // and its detail threshold at +0x178; FUN_1400592d0 splits a node only while its
                // pools have room: +0x10398, +0x103c0 - +0x103b8, +0x230 - +0x228).
                __try {
                    if(auto* tree=At<uint8_t*>(scene,0xa80)){
                        FixedPos at{};std::memcpy(&at,tree+0x160,sizeof(at));
                        math::Vec3 focus{};ToFloat(at,focus.x,focus.y,focus.z);
                        std::snprintf(line,sizeof(line),"Town Portal: %s brick tree %p: focus %.1f %.1f %.1f, threshold %.1f, quality %.3f, pools %llu / %d / %lld, nodes +0x80 %llu, sources %p %p",
                                      who?"player's":"second camera's",static_cast<void*>(tree),focus.x,focus.y,focus.z,At<float>(tree,0x178),At<float>(tree,0x10494),
                                      At<uint64_t>(tree,0x10398),int(At<int32_t>(tree,0x103c0)-At<int32_t>(tree,0x103b8)),
                                      (long long)(At<int64_t>(tree,0x230)-At<int64_t>(tree,0x228)),At<uint64_t>(tree,0x80),At<void*>(tree,0x20),At<void*>(tree,0x28));
                        greeneye::Host()->log(line);
                        static uint64_t dumpedAt[2]{};
                        if(const uint64_t now2=GetTickCount64();now2-dumpedAt[who]>60000){
                            dumpedAt[who]=now2;
                            for(uint32_t base:{0x0u,0x10300u,0x10400u}){
                                for(uint32_t row=0;row<(base?4u:8u);++row){
                                    int n=std::snprintf(line,sizeof(line),"Town Portal:   %s tree +0x%x:",who?"player's":"second camera's",base+row*0x40);
                                    for(uint32_t i=0;i<8;++i)n+=std::snprintf(line+n,sizeof(line)-n," %llx",At<uint64_t>(tree,base+row*0x40+i*8));
                                    greeneye::Host()->log(line);
                                }
                            }
                        }
                    } else {
                        std::snprintf(line,sizeof(line),"Town Portal: %s model scene has no brick tree at +0xa80",who?"player's":"second camera's");
                        greeneye::Host()->log(line);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
                // The scene's terrain pieces (pages at +0x2b0, count +0x2b8; a page is 0x40 bytes
                // with its entries at +8 and their count at +0x10; an entry is 0x110 bytes, the
                // piece from +0x10, its hidden flag at +0xef; FUN_140113840).
                {
                    auto* pages=At<uint8_t*>(scene,0x2b0);
                    const auto pageCount=At<uint64_t>(scene,0x2b8);
                    uint32_t slots=0,hiddenCount=0,nonzero=0,baked=0,shownBaked=0;uint8_t* shown=nullptr;uint8_t* shownList[6]{};int shownN=0;
                    uint32_t byScale[8]{};uint8_t* coarse[4]{};int coarseN=0;
                    uint32_t byDistance[6]{},hiddenByDistance[6]{};float nearestShown=1e9f,farthestShown=0;
                    uint32_t drawnByScale[8]{},drawnFlags[8][8]{},drawnByte14[8]{},drawnHasC0[8]{},drawnHasF8[8]{};uint8_t* drawnSample[3]{};int drawnSampleN=0;
                    const math::Vec3 centreOfView=who?g_playerCamera:g_modelFocus;
                    auto pieceDistance=[&](uint8_t* entry){
                        // The piece's corner (fixed point at +0x18) and its 16 m (32 voxels) extent.
                        FixedPos corner{};std::memcpy(&corner,entry+0x18,sizeof(corner));
                        math::Vec3 c{};ToFloat(corner,c.x,c.y,c.z);
                        const float size=32.f*At<float>(entry,0x40);
                        const math::Vec3 d{std::max({c.x-centreOfView.x,centreOfView.x-(c.x+size),0.f}),std::max({c.y-centreOfView.y,centreOfView.y-(c.y+size),0.f}),std::max({c.z-centreOfView.z,centreOfView.z-(c.z+size),0.f})};
                        return std::sqrt(math::LengthSq(d));
                    };
                    auto bin=[](float d){return d<16?0:d<32?1:d<48?2:d<64?3:d<128?4:5;};
                    if(pages&&pageCount<=4096)for(uint64_t pg=0;pg<pageCount;++pg){
                        auto* entries=At<uint8_t*>(pages+pg*0x40,8);
                        const auto n=At<uint64_t>(pages+pg*0x40,0x10);
                        if(!entries||n>65536)continue;
                        for(uint64_t e=0;e<n;++e){
                            auto* entry=entries+e*0x110;
                            ++slots;
                            if(!(At<uint32_t>(entry,0)&0x80000000u)&&(At<uint64_t>(entry,0x10)||At<uint64_t>(entry,0x18))){++nonzero;const bool b=At<uint32_t>(entry,0x10c)!=0;baked+=b;if(At<uint8_t>(entry,0xef)){++hiddenCount;++hiddenByDistance[bin(pieceDistance(entry))];
                                // +0xef set = drawn (FUN_1401e7eb0 sets it while the node's children are not all ready).
                                const float scale=At<float>(entry,0x40);
                                int sb=0;for(float v=0.5f;sb<7&&scale>v*1.01f;v*=2)++sb;
                                ++drawnByScale[sb];
                                const uint8_t flags=At<uint8_t>(entry,0x108);
                                for(int f=0;f<8;++f)if(flags&(1<<f))++drawnFlags[sb][f];
                                if(At<uint8_t>(entry,0x14))++drawnByte14[sb];
                                if(At<uint32_t>(entry,0xc0))++drawnHasC0[sb];
                                if(At<uint32_t>(entry,0x10c))++drawnHasF8[sb];
                                if(drawnSampleN<3&&pieceDistance(entry)<24.f)drawnSample[drawnSampleN++]=entry;
                            }else{shownBaked+=b;
                                {const float d=pieceDistance(entry);++byDistance[bin(d)];nearestShown=std::min(nearestShown,d);farthestShown=std::max(farthestShown,d);}if(!shown)shown=entry;if(shownN<6)shownList[shownN++]=entry;
                                const float scale=At<float>(entry,0x40);
                                int bin=0;for(float v=0.5f;bin<7&&scale>v*1.01f;v*=2)++bin;
                                ++byScale[bin];
                                if(scale>1.5f&&coarseN<4)coarse[coarseN++]=entry;}}
                        }
                    }
                    std::snprintf(line,sizeof(line),"Town Portal: %s terrain pieces: %llu pages, %u slots, %u in use, %u of those hidden; +0x10c set on %u (%u of the shown ones)",
                                  who?"player's":"second camera's",pageCount,slots,nonzero,hiddenCount,baked,shownBaked);
                    greeneye::Host()->log(line);
                    std::snprintf(line,sizeof(line),"Town Portal:   %s shown pieces by size (0.5, 1, 2, 4, 8, 16, 32 m voxels): %u %u %u %u %u %u %u",
                                  who?"player's":"second camera's",byScale[0],byScale[1],byScale[2],byScale[3],byScale[4],byScale[5],byScale[6]);
                    greeneye::Host()->log(line);
                    std::snprintf(line,sizeof(line),"Town Portal:   %s shown pieces by distance from its camera (<16, <32, <48, <64, <128, more): %u %u %u %u %u %u (hidden: %u %u %u %u %u %u), nearest %.0f m, farthest %.0f m",
                                  who?"player's":"second camera's",byDistance[0],byDistance[1],byDistance[2],byDistance[3],byDistance[4],byDistance[5],
                                  hiddenByDistance[0],hiddenByDistance[1],hiddenByDistance[2],hiddenByDistance[3],hiddenByDistance[4],hiddenByDistance[5],nearestShown,farthestShown);
                    greeneye::Host()->log(line);
                    for(int sb=0;sb<7;++sb)if(drawnByScale[sb]){
                        std::snprintf(line,sizeof(line),"Town Portal:   %s drawn pieces of %.1f m voxels: %u; flag bits %u %u %u %u %u %u %u %u; +0x14 set %u, +0xc0 set %u, +0x10c set %u",
                                      who?"player's":"second camera's",0.5f*float(1<<sb),drawnByScale[sb],
                                      drawnFlags[sb][0],drawnFlags[sb][1],drawnFlags[sb][2],drawnFlags[sb][3],drawnFlags[sb][4],drawnFlags[sb][5],drawnFlags[sb][6],drawnFlags[sb][7],
                                      drawnByte14[sb],drawnHasC0[sb],drawnHasF8[sb]);
                        greeneye::Host()->log(line);
                    }
                    for(int k=0;k<drawnSampleN;++k){
                        int n=std::snprintf(line,sizeof(line),"Town Portal:   %s drawn near piece scale %.1f (+0x00..):",who?"player's":"second camera's",At<float>(drawnSample[k],0x40));
                        for(int o=0x0;o<0x110;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(drawnSample[k],o));
                        greeneye::Host()->log(line);
                    }
                    for(int k=0;k<coarseN;++k){
                        int n=std::snprintf(line,sizeof(line),"Town Portal:   coarse piece %d scale %.1f (+0x90..):",k,At<float>(coarse[k],0x40));
                        for(int o=0x90;o<0x110;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %x",At<uint32_t>(coarse[k],o));
                        greeneye::Host()->log(line);
                    }
                    for(int k=0;k<0;++k){
                        int n=std::snprintf(line,sizeof(line),"Town Portal:   shown piece %d (+0x00, +0x10, then +0xd0..):  %08x %08x %08x |",k,At<uint32_t>(shownList[k],0),At<uint32_t>(shownList[k],0x10),At<uint32_t>(shownList[k],0x14));
                        for(int o=0xd0;o<0x110;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(shownList[k],o));
                        greeneye::Host()->log(line);
                    }
                    if(false)for(int row=0;row<2;++row){
                        int n=std::snprintf(line,sizeof(line),"Town Portal:   piece +0x%02x:",row*0x88);
                        for(int o=0;o<0x88;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(shown,row*0x88+o));
                        greeneye::Host()->log(line);
                    }
                }
                // Dev: one skinned instance of the player's scene in full, once a minute, to learn
                // what a posed model carries (+0x130 and what it points at).
                {
                    static uint64_t dumpedSkinned=0;
                    if(const uint64_t t=GetTickCount64();skinnedSample&&t-dumpedSkinned>60000){
                        dumpedSkinned=t;
                        for(int row=0;row<6;++row){
                            int n=std::snprintf(line,sizeof(line),"Town Portal:   skinned instance +0x%03x:",row*0x48);
                            for(int o=0;o<0x48;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(skinnedSample,row*0x48+o));
                            greeneye::Host()->log(line);
                        }
                        __try {
                            for(int which=0;which<3;++which){
                                auto* target=At<uint8_t*>(skinnedSample,which==0?0x108:which==1?0x128:0x140);
                                if(!target||reinterpret_cast<uintptr_t>(target)<0x10000)continue;
                                int n=std::snprintf(line,sizeof(line),"Town Portal:   skinned +0x%x -> %p:",which==0?0x108:which==1?0x128:0x140,static_cast<void*>(target));
                                for(int o=0;o<0x80;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(target,o));
                                greeneye::Host()->log(line);
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
                for(int k=0;k<0;++k){
                    auto* i=k?sampleModel:sample;
                    if(!i)continue;
                    for(int row=0;row<3;++row){
                        int n=std::snprintf(line,sizeof(line),"Town Portal:   %s instance +0x%02x:",k?"model":"no-model",row*0x90);
                        for(int o=0;o<0x90;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(i,row*0x90+o));
                        greeneye::Host()->log(line);
                    }
                }
            }
        }
    }
    // Dev: which of the world's objects are being mirrored (every 10 s).
    {
        static uint64_t told=0;
        if(const uint64_t now=GetTickCount64();now-told>10000){
            told=now;int shown=0;
            for(auto& entry:g_mirrors->records)if(entry.owned&&!entry.memory&&shown<6){
                ++shown;
                FixedPos fixed{};std::memcpy(&fixed,entry.transform,sizeof(fixed));
                math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                char line[200]{};
                std::snprintf(line,sizeof(line),"Town Portal: mirrored world object %016llx%016llx at %.1f %.1f %.1f (camera %.1f %.1f %.1f, player %.1f %.1f %.1f)",
                              entry.guid[0],entry.guid[1],at.x,at.y,at.z,g_modelFocus.x,g_modelFocus.y,g_modelFocus.z,g_playerCamera.x,g_playerCamera.y,g_playerCamera.z);
                greeneye::Host()->log(line);
            }
        }
    }
    // A far place. The second camera's own scene holds the world as it was made (its trees,
    // rocks and ruins), not what players changed: a tree someone felled is back, their crates
    // are missing. Where the player has been this session the remembered objects say what really
    // stands there: the scene's own objects that were not seen there are put out of sight, and
    // remembered ones the scene lacks are made. Near the player the world's own objects are
    // mirrored above and none of this applies.
    if(!g_objectMemory)return;
    const bool remote=g_mirrors->eligible<8;
    std::array<uint64_t,kRememberSlots/64> chosen{};
    static uint64_t reported=0;
    uint32_t hidden=0,native=0,matched=0;
    // Round the far endpoint, which stays put, not round the second camera, which follows the
    // viewer's eye: as that moved, which 256 were nearest kept changing and objects were
    // removed and made again all the time (1468 removals in three minutes, 2026-11-09).
    math::Vec3 centre=g_modelFocus;
    if(g_liveView.since)centre={g_liveView.destination.wx,g_liveView.destination.wy,g_liveView.destination.wz};
    if(remote){
        auto& memory=*g_objectMemory;
        // Remembered objects round the camera, and the 16 m cells they stand in: a cell with one
        // (or next to one) has been seen.
        std::array<uint64_t,kRememberSlots/64> present{};
        std::array<uint32_t,512> cells{};
        auto cellKey=[](float x,float z){return (uint32_t(int32_t(std::floor(x/16.f))&0xffff)<<16|uint32_t(int32_t(std::floor(z/16.f))&0xffff))|1u;};
        auto cellAdd=[&](uint32_t key){
            for(uint32_t probe=0;probe<cells.size();++probe){
                auto& c=cells[(key*2654435761u+probe)%cells.size()];
                if(c==key)return;
                if(!c){c=key;return;}
            }
        };
        auto cellHas=[&](uint32_t key){
            for(uint32_t probe=0;probe<cells.size();++probe){
                const auto c=cells[(key*2654435761u+probe)%cells.size()];
                if(c==key)return true;
                if(!c)return false;
            }
            return false;
        };
        for(uint32_t slot=0;slot<kRememberSlots;++slot){
            const auto& o=memory.slots[slot];
            if(o.used==2&&math::LengthSq(math::Vec3{o.x,o.y,o.z}-centre)<150.0f*150.0f)cellAdd(cellKey(o.x,o.z));
        }
        auto seen=[&](float x,float z){
            for(int dx=-1;dx<=1;++dx)for(int dz=-1;dz<=1;++dz)if(cellHas(cellKey(x+16.f*dx,z+16.f*dz)))return true;
            return false;
        };
        // The scene's own objects (not the ones made here) within 120 m.
        auto* own=static_cast<uint8_t*>(owned);
        const auto ownCapacity=At<uint64_t>(own,0x1d0);
        auto* ownActive=At<uint64_t*>(own,0x208);
        auto* ownGenerations=At<uint8_t*>(own,0x1c8);
        auto* ownInstances=At<uint8_t*>(own,0x1b0);
        if(ownCapacity<=65536&&ownActive&&ownGenerations&&ownInstances){
            for(uint32_t word=0;word<(ownCapacity+63)/64;++word){
                uint64_t bits=ownActive[word];
                while(bits){
                    unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
                    const uint32_t index=word*64+bit;
                    if(index>=ownCapacity)break;
                    auto* instance=ownInstances+size_t(index)*0x1b0;
                    // Only the kinds of object that are remembered (RememberObjects).
                    if(At<uint32_t>(instance,0x130)||!ActorPartEligible(instance))continue;
                    FixedPos fixed{};std::memcpy(&fixed,instance+0x38,sizeof(fixed));
                    math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
                    if(!math::Finite(at)||math::LengthSq(at-centre)>=120.0f*120.0f)continue;
                    const uint32_t handle=(uint32_t(ownGenerations[index])<<24)|index;
                    bool ours=false;
                    for(const auto& entry:g_mirrors->records)if(entry.owned==handle){ours=true;break;}
                    if(ours)continue;
                    ++native;
                    uint64_t guid[2];std::memcpy(guid,At<uint8_t*>(instance,0x108)+0x18,16);
                    const int slot=FindRemembered(guid,at);
                    if(slot>=0){present[slot/64]|=uint64_t(1)<<(slot%64);++matched;continue;}
                    if(!seen(at.x,at.z))continue; // a place not visited: leave it as the world has it
                    // Seen there and not remembered: it is gone in the real world. Out of sight
                    // (far below), keeping the scene's own handle to it valid.
                    uint8_t away[56];std::memcpy(away,instance+0x38,sizeof(away));
                    FixedPos down=fixed;down.y-=int64_t(30000)<<32;
                    std::memcpy(away,&down,sizeof(down));
                    NativeSpan ids{&handle,1},transform{away,1};
                    g_instanceTransform(owned,&ids,&transform,0);
                    ++hidden;
                }
            }
        }
        // The nearest remembered objects the scene lacks.
        std::array<Candidate,256> remembered{};size_t found=0;
        for(uint32_t slot=0;slot<kRememberSlots;++slot){
            const auto& o=memory.slots[slot];
            if(o.used!=2||(present[slot/64]&(uint64_t(1)<<(slot%64))))continue;
            const float d=math::LengthSq(math::Vec3{o.x,o.y,o.z}-centre);
            if(d>=120.0f*120.0f)continue;
            Candidate candidate{d,slot};
            if(found<remembered.size()){remembered[found++]=candidate;std::push_heap(remembered.begin(),remembered.begin()+found,nearer);}
            else if(nearer(candidate,remembered[0])){std::pop_heap(remembered.begin(),remembered.end(),nearer);remembered.back()=candidate;std::push_heap(remembered.begin(),remembered.end(),nearer);}
        }
        for(size_t i=0;i<found;++i)chosen[remembered[i].index/64]|=uint64_t(1)<<(remembered[i].index%64);
        const uint64_t now=GetTickCount64();
        if(hidden||now-reported>10000){
            reported=now;
            char line[200]{};
            std::snprintf(line,sizeof(line),"Town Portal: live view objects: %u of the far place's own (%u as remembered, %u put out of sight now), %zu remembered ones added, %u remembered in all",
                          native,matched,hidden,found,memory.count);
            greeneye::Host()->log(line);
        }
    }
    for(auto& entry:g_mirrors->records)if(entry.owned&&entry.memory){
        const uint32_t slot=entry.memory-1;
        const bool keep=remote&&slot<kRememberSlots&&(chosen[slot/64]&(uint64_t(1)<<(slot%64)))&&
            g_objectMemory->slots[slot].used==2&&std::memcmp(g_objectMemory->slots[slot].guid,entry.guid,16)==0;
        if(keep){chosen[slot/64]&=~(uint64_t(1)<<(slot%64));continue;} // already made
        NativeSpan ids{&entry.owned,1};
        g_instanceRemove(owned,&ids);entry={};--g_mirrors->count;++g_mirrors->removed;
    }
    uint32_t make=16;
    for(uint32_t word=0;word<chosen.size()&&make&&g_mirrors->count<256;++word){
        uint64_t bits=chosen[word];
        while(bits&&make&&g_mirrors->count<256){
            unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
            const uint32_t slot=word*64+bit;
            auto& o=g_objectMemory->slots[slot];
            MirrorRecord* empty=nullptr;
            for(auto& entry:g_mirrors->records)if(!entry.owned){empty=&entry;break;}
            if(!empty)break;
            const uint32_t created=g_instanceCreate(owned,o.guid,o.transform);--make;
            if(created){
                empty->source=0;empty->owned=created;empty->memory=slot+1;
                std::memcpy(empty->guid,o.guid,16);std::memcpy(empty->transform,o.transform,56);
                ++g_mirrors->count;++g_mirrors->created;
            }
        }
    }
}
// Notes the static objects the world shows within 80 m of the player's camera (twice a second),
// and forgets the ones within 30 m that the world no longer shows (picked up, felled, or a
// carried tool that has moved on).
void RememberObjects(void* scene) {
    static uint64_t next=0;
    const uint64_t now=GetTickCount64();
    if(!g_objectMemory||!g_havePlayerCamera||now<next)return;
    next=now+500;
    auto* source=At<uint8_t*>(scene,0x98);
    if(!source)return;
    const auto capacity=At<uint64_t>(source,0x1d0);
    auto* active=At<uint64_t*>(source,0x208);
    auto* instances=At<uint8_t*>(source,0x1b0);
    if(capacity>65536||!active||!instances)return;
    auto& memory=*g_objectMemory;
    const uint32_t second=uint32_t(now/1000);
    // Mostly slots of things gone: start again (the places near the player fill in at once).
    if(memory.count>=kRememberSlots*3/4){
        uint32_t live=0;
        for(const auto& o:memory.slots)live+=o.used==1||o.used==2;
        if(live<kRememberSlots/2){std::memset(&memory,0,sizeof(memory));}
    }
    for(uint32_t word=0;word<(capacity+63)/64;++word){
        uint64_t bits=active[word];
        while(bits){
            unsigned long bit{};_BitScanForward64(&bit,bits);bits&=bits-1;
            const uint32_t index=word*64+bit;
            if(index>=capacity)break;
            auto* instance=instances+size_t(index)*0x1b0;
            // As SyncOwnedInstances: no skinned or cloth instances, and only ones with a model.
            if(At<uint32_t>(instance,0x130)||!ActorPartEligible(instance))continue;
            FixedPos fixed{};std::memcpy(&fixed,instance+0x38,sizeof(fixed));
            math::Vec3 at{};ToFloat(fixed,at.x,at.y,at.z);
            if(!math::Finite(at)||math::LengthSq(at-g_playerCamera)>=80.0f*80.0f)continue;
            uint64_t guid[2];std::memcpy(guid,At<uint8_t*>(instance,0x108)+0x18,16);
            const uint64_t h=RememberHash(guid,at);
            RememberedObject* gone=nullptr;
            for(uint32_t probe=0;probe<24;++probe){
                auto& o=memory.slots[(h+probe)%kRememberSlots];
                if(o.used==3){if(!gone)gone=&o;continue;}
                if(o.used){
                    if(o.guid[0]!=guid[0]||o.guid[1]!=guid[1]||std::fabs(o.x-at.x)>0.2f||std::fabs(o.y-at.y)>0.2f||std::fabs(o.z-at.z)>0.2f)continue;
                    std::memcpy(o.transform,instance+0x38,56);o.x=at.x;o.y=at.y;o.z=at.z;o.last=second;
                    if(o.used==1&&second-o.first>=5)o.used=2;
                    gone=nullptr;
                    break;
                }
                // Not known: into the first slot of a gone one on the way, or this empty one.
                auto* into=gone?gone:&o;
                if(!gone){if(memory.count>=kRememberSlots*3/4)break;++memory.count;}
                gone=nullptr;
                into->guid[0]=guid[0];into->guid[1]=guid[1];std::memcpy(into->transform,instance+0x38,56);
                into->x=at.x;into->y=at.y;into->z=at.z;into->used=1;into->first=into->last=second;
                break;
            }
            if(gone){ // the chain was full of taken slots: reuse the gone one
                gone->guid[0]=guid[0];gone->guid[1]=guid[1];std::memcpy(gone->transform,instance+0x38,56);
                gone->x=at.x;gone->y=at.y;gone->z=at.z;gone->used=1;gone->first=gone->last=second;
            }
        }
    }
    for(auto& o:memory.slots)
        if((o.used==1||o.used==2)&&second-o.last>=3&&math::LengthSq(math::Vec3{o.x,o.y,o.z}-g_playerCamera)<30.0f*30.0f)o.used=3;
}
void RememberObjectsGuard(void* scene) {
    __try {RememberObjects(scene);}
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}
uint8_t* g_treeSeen{};uint64_t g_treeSeenAt{},g_treeReadyAt{},g_treeMovedAt{};float g_treeFactor{};
// Texture detail for the far view. The game keeps a texture's fine mip levels loaded only while
// the graphics card reports them sampled (GPU data cache, render root +0x16428: a feedback
// buffer of 4096 material slots, read back each frame by FUN_1400c0170 into a "wanted mip" per
// streamed texture, which the task FUN_1400c0ca0 then solves). The second camera's draws leave
// no feedback (why is not known), so what only the far view shows kept its mip tail: leaves
// and the altar's flame were flat blobs after a rejoin far from the base (2026-11-10; with the
// game's algorithm byte forced to "full resolution" they were right). So the textures the
// second scene's models and effects use are collected once a second on the scene's update
// thread (FarTexturesCollect), and the solver task is told to want them at full detail
// (FarMipsHook). tools/far.mips (dev) holds another mip to ask for, "off" for none.
// The chains, from the game's code:
//   model (instance +0x108) +0x68 -> model loader (root +0x16448) record of 0xd0 bytes:
//     +0x70/+0x78 material handles -> material loader (model loader +0x148) item:
//     +0x40/+0x48 texture handles; +0x58, +0x5c card clusters (leaves) -> card cluster loader
//     (model loader +0x150) entry: texture handles at +0x38, +0x40, +0x44, +0x48
//   effect type record (manager +0x1c38, 0xc0 bytes): +0x40/+0x48 and +0x68/+0x70 models,
//     +0x80/+0x88 decals -> decal loader (manager +0x170) item +0x4c -> cache material:
//     +0x10/+0x18 texture handles
//   texture handle (0x10000 | index) -> cache +0xfe8 record of 0x48 bytes, +0x40 its streaming
//     state; wanted index = (state - *(cache +0x1368)) / 0x108
struct FarTextures {
    static constexpr uint32_t kMost=8192;
    uint32_t index[2][kMost]{};
    std::atomic<uint32_t> count[2]{};
    std::atomic<uint32_t> live{0};
    std::atomic<uint64_t> collectedAt{0};
    std::atomic<float> mip{0.f};
    std::atomic<uint64_t> applied{0};
    // Dev: tools/far.range holds "first last": every texture with an index in that range is
    // wanted at full detail too (to find by halving which texture something depends on).
    // Asking for EVERY index (0 to 13930) crashed the game within two seconds (2026-11-10):
    // only indices that belong to a texture record are touched (real, filled by the collector),
    // and at most 600 of a range.
    std::atomic<uint32_t> rangeFirst{0},rangeLast{0},solverCount{0};
    std::atomic<uint64_t> real[1024]{};
    // Dev: tools/far.floor holds a mip level no texture is left coarser than while the far view
    // runs (a blunt stand-in for the feedback the far view's draws do not give). It is brought
    // in 300 textures a second, and how far it has got is written to tools/far-floor.txt, so a
    // crash can be laid at an index.
    std::atomic<float> floorMip{-1.f};
    std::atomic<uint32_t> floorReach{0};
};
FarTextures g_farTextures;
// A pooled entry by handle, as the game looks them up.
uint8_t* PooledEntry(uint8_t* pool,uint32_t handle,uint32_t mask,size_t shiftAt,size_t slotMaskAt,size_t chunksAt,size_t chunkCountAt,size_t size,uint32_t generationShift) {
    if(!pool||!handle)return nullptr;
    const uint64_t at=handle&mask;
    const uint64_t chunk=at>>(At<uint64_t>(pool,shiftAt)&63);
    const uint64_t slot=at&At<uint64_t>(pool,slotMaskAt);
    auto* chunks=At<uint8_t*>(pool,chunksAt);
    if(!chunks||chunk>=At<uint64_t>(pool,chunkCountAt))return nullptr;
    auto* base=At<uint8_t*>(chunks+chunk*0x40,8);
    if(!base||slot>=At<uint64_t>(chunks+chunk*0x40,0x10))return nullptr;
    auto* entry=base+slot*size;
    return (At<uint32_t>(entry,0)&0x7fffffffu)==(handle>>generationShift)?entry:nullptr;
}
struct FarCollector {
    uint8_t* cache{};uint8_t* modelLoader{};uint8_t* materialLoader{};uint8_t* cardLoader{};
    uint32_t* out{};uint32_t count{};
    uint64_t seen[1024]{};                      // 65536 wanted indices
    uint32_t models{},materials{},textures{};
    void Texture(uint32_t handle) {
        if((handle>>16)!=1u||!cache)return;
        auto* records=At<uint8_t*>(cache,0xfe8);
        if(!records)return;
        auto* state=At<uint8_t*>(records+size_t(handle&0xffffu)*0x48,0x40);
        auto* first=At<uint8_t*>(cache,0x1368);
        if(!state||!first||state<first)return;
        const uint64_t i=uint64_t(state-first)/0x108;
        if(i>=65536||first+i*0x108!=state)return;
        if(seen[i>>6]>>(i&63)&1)return;
        seen[i>>6]|=1ull<<(i&63);
        ++textures;
        if(count<FarTextures::kMost)out[count++]=uint32_t(i);
    }
    void Textures(const uint32_t* handles,uint64_t n) {
        if(!handles||n>256)return;
        for(uint64_t i=0;i<n;++i)Texture(handles[i]);
    }
    void ModelHandle(uint8_t* loader,uint32_t handle) {
        if(!loader||!handle)return;
        auto* records=At<uint8_t*>(loader,0x198);
        if(!records)return;
        auto* record=records+size_t(handle&0xffffu)*0xd0;
        if(At<uint8_t>(record,0x12)!=uint8_t(handle>>16)||At<uint8_t>(record,0x14)!=0)return;
        ++models;
        auto* materialHandles=At<uint32_t*>(record,0x70);
        const uint64_t n=At<uint64_t>(record,0x78);
        auto* materialsOf=At<uint8_t*>(loader,0x148);
        if(materialHandles&&n<=256&&materialsOf)for(uint64_t i=0;i<n;++i){
            auto* entry=PooledEntry(materialsOf,materialHandles[i],0xffffu,0x4f0,0x500,0x4c8,0x4d0,0xb0,16);
            if(!entry)continue;
            auto* item=entry+8;
            if(At<uint8_t>(item,6)!=3)continue;
            ++materials;
            Textures(At<uint32_t*>(item,0x40),At<uint64_t>(item,0x48));
        }
        auto* cardsOf=At<uint8_t*>(loader,0x150);
        for(size_t at:{size_t(0x58),size_t(0x5c)}){
            auto* entry=PooledEntry(cardsOf,At<uint32_t>(record,at),0xfffffu,0x68,0x78,0x40,0x48,0x50,20);
            if(!entry)continue;
            for(size_t t:{size_t(0x38),size_t(0x40),size_t(0x44),size_t(0x48)})Texture(At<uint32_t>(entry,t));
        }
    }
};
// One instance slot: a free slot's model field is not a model, so each is tried on its own.
void FarInstanceModel(FarCollector& c,uint8_t* instance,uint8_t*& lastModel) {
    __try {
        auto* model=At<uint8_t*>(instance,0x108);
        if(!model||model==lastModel||!At<uint32_t>(instance,0x114))return;
        lastModel=model;
        c.ModelHandle(c.modelLoader,At<uint32_t>(model,0x68));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
int g_farStage{};
void FarTexturesCollectBody(uint8_t* ownedScene) {
    g_farStage=1;
    auto* root=At<uint8_t*>(ownedScene,0x18);
    if(!root)return;
    static FarCollector c;                      // large: not on the stack
    c=FarCollector{};
    c.cache=At<uint8_t*>(root,0x16428);
    c.modelLoader=At<uint8_t*>(root,0x16448);
    const uint32_t spare=1u-g_farTextures.live.load();
    c.out=g_farTextures.index[spare];
    if(!c.cache||!c.modelLoader)return;
    // Every model of the second scene.
    g_farStage=2;
    auto* instances=At<uint8_t*>(ownedScene,0x1b0);
    const uint64_t capacity=At<uint64_t>(ownedScene,0x1d0);
    uint8_t* lastModel=nullptr;
    for(uint64_t i=0;instances&&i<capacity&&i<65536;++i)FarInstanceModel(c,instances+i*0x1b0,lastModel);
    // Every effect running in it: its mesh models and decals.
    g_farStage=3;
    if(auto* system=At<uint8_t*>(ownedScene,0x950)){
        auto* manager=At<uint8_t*>(system,8);
        auto* entries=At<uint8_t*>(system,0x60);
        const uint64_t most=At<uint64_t>(system,0x58);
        auto* types=manager?At<uint8_t*>(manager,0x1c38):nullptr;
        auto* effectModels=manager?At<uint8_t*>(manager,0x160):nullptr;
        auto* decals=manager?At<uint8_t*>(manager,0x170):nullptr;
        uint64_t typeSeen[64]{};
        uint32_t walked=0;
        for(uint32_t link=At<uint32_t>(system,0x70);types&&entries&&link&&link<=most&&walked<8192;++walked){
            auto* e=entries+size_t(link-1)*0xc0;link=At<uint32_t>(e,4);
            const uint32_t type=At<uint32_t>(e,0x70);
            if(type>=4096||(typeSeen[type>>6]>>(type&63)&1))continue;
            typeSeen[type>>6]|=1ull<<(type&63);
            auto* r=types+size_t(type)*0xc0;
            if(auto* list=At<uint8_t*>(r,0x40)){const uint64_t n=At<uint64_t>(r,0x48);for(uint64_t i=0;i<n&&i<64;++i)c.ModelHandle(effectModels,At<uint32_t>(list+i*0x10,8));}
            if(auto* list=At<uint32_t*>(r,0x68)){const uint64_t n=At<uint64_t>(r,0x70);for(uint64_t i=0;i<n&&i<64;++i)c.ModelHandle(effectModels,list[i]);}
            if(auto* list=At<uint32_t*>(r,0x80)){
                const uint64_t n=At<uint64_t>(r,0x88);
                for(uint64_t i=0;decals&&i<n&&i<64;++i){
                    auto* entry=PooledEntry(decals+0x28,list[i],0xffffu,0x38,0x48,0x10,0x18,0x60,16);
                    if(!entry)continue;
                    auto* item=entry+8;
                    if(At<uint32_t>(item,0x54)!=2)continue;
                    auto* material=PooledEntry(c.cache,At<uint32_t>(item,0x4c),0xfffffu,0x1148,0x1158,0x1120,0x1128,0x40,20);
                    if(material)c.Textures(At<uint32_t*>(material,0x10),At<uint64_t>(material,0x18));
                }
            }
        }
    }
    g_farStage=4;
    {   // Which wanted indices belong to a texture record (cache +0xfe8, count at +0xfdc).
        static uint64_t real[1024];
        std::memset(real,0,sizeof(real));
        auto* records=At<uint8_t*>(c.cache,0xfe8);
        auto* first=At<uint8_t*>(c.cache,0x1368);
        const uint32_t n=std::min<uint32_t>(At<uint32_t>(c.cache,0xfdc),65536u);
        for(uint32_t k=0;records&&first&&k<n;++k){
            auto* state=At<uint8_t*>(records+size_t(k)*0x48,0x40);
            if(!state||state<first)continue;
            const uint64_t i=uint64_t(state-first)/0x108;
            if(i<65536&&first+i*0x108==state)real[i>>6]|=1ull<<(i&63);
        }
        for(int k=0;k<1024;++k)g_farTextures.real[k].store(real[k]);
    }
    g_farTextures.count[spare].store(c.count);
    g_farTextures.live.store(spare);
    static uint64_t told=0;
    if(const uint64_t now=GetTickCount64();now-told>20000){
        told=now;char line[200]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view: far textures: %u models, %u materials, %u textures wanted at mip %.1f; asked %llu times",
                      c.models,c.materials,c.textures,g_farTextures.mip.load(),g_farTextures.applied.load());
        greeneye::Host()->log(line);
    }
}
void FarTexturesCollect(uint8_t* ownedScene) {
    const uint64_t now=GetTickCount64();
    if(now-g_farTextures.collectedAt.load()<1000)return;
    g_farTextures.collectedAt.store(now);
    float mip=0.f;
    if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/far.mips",L"r")){
        char text[16]{};
        if(std::fgets(text,sizeof(text),f)){if(!std::strncmp(text,"off",3))mip=-1.f;else mip=std::clamp(float(std::atof(text)),0.f,15.f);}
        std::fclose(f);
    }
    g_farTextures.mip.store(mip);
    {
        float floorMip=-1.f;
        if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/far.floor",L"r")){
            if(std::fscanf(f,"%f",&floorMip)!=1||floorMip<0.f||floorMip>12.f)floorMip=-1.f;
            std::fclose(f);
        }
        if(floorMip<0.f)g_farTextures.floorReach.store(0);
        else {
            const uint32_t reach=std::min<uint32_t>(g_farTextures.floorReach.load()+300u,65536u);
            g_farTextures.floorReach.store(reach);
            if(reach<=g_farTextures.solverCount.load()+300u){
                char text[64]{};const int n=std::snprintf(text,sizeof(text),"%u of %u at mip %.1f%c",reach,g_farTextures.solverCount.load(),floorMip,10);
                HANDLE f=CreateFileW(L"C:/Mods/Enshrouded/TownPortal/tools/far-floor.txt",GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,0,nullptr);
                if(f!=INVALID_HANDLE_VALUE){DWORD wrote{};WriteFile(f,text,DWORD(n),&wrote,nullptr);FlushFileBuffers(f);CloseHandle(f);}
            }
        }
        g_farTextures.floorMip.store(floorMip);
    }
    {
        unsigned first=0,last=0;
        if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/far.range",L"r")){
            if(std::fscanf(f,"%u %u",&first,&last)!=2||last<first){first=0;last=0;}
            std::fclose(f);
        }
        if(first!=g_farTextures.rangeFirst.load()||last!=g_farTextures.rangeLast.load()){
            char line[120]{};
            std::snprintf(line,sizeof(line),"Town Portal: live view: far textures: range %u to %u wanted too (%u textures in all)",first,last,g_farTextures.solverCount.load());
            greeneye::Host()->log(line);
        }
        g_farTextures.rangeFirst.store(first);g_farTextures.rangeLast.store(last);
    }
    __try {FarTexturesCollectBody(ownedScene);}
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_farTextures.count[0].store(0);g_farTextures.count[1].store(0);
        static uint64_t told=0;
        if(now-told>20000){told=now;char line[120]{};std::snprintf(line,sizeof(line),"Town Portal: live view: far textures: the walk faulted at stage %d",g_farStage);greeneye::Host()->log(line);}
    }
}
using MipSolverFn=void(*)(uint8_t** task);
MipSolverFn g_mipSolver{};
void FarMipsApply(uint8_t** task) {
    __try {
        auto* context=task?*task:nullptr;
        if(!context)return;
        auto* wanted=At<float*>(context,0x20);
        const uint32_t most=At<uint32_t>(context,0x10);
        const float mip=g_farTextures.mip.load();
#ifdef TOWN_PORTAL_DEV
        {   // Dev: tools/tex.log (a file that exists) makes a line every two seconds in
            // tools/tex-wanted.txt: the tick, then index:mip for every texture the graphics card
            // reported as sampled (wanted under 16), before the far view's own wishes go in.
            static uint64_t wroteAt=0;
            const uint64_t tick=GetTickCount64();
            if(wanted&&tick-wroteAt>2000&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/tex.log")!=INVALID_FILE_ATTRIBUTES){
                wroteAt=tick;
                static char text[400000];
                int n=std::snprintf(text,sizeof(text),"%llu",tick);
                for(uint32_t i=0;i<most&&i<65536&&n<int(sizeof(text))-24;++i)
                    if(wanted[i]<15.5f)n+=std::snprintf(text+n,sizeof(text)-n," %x:%d",i,int(wanted[i]));
                text[n++]=10;
                HANDLE f=CreateFileW(L"C:/Mods/Enshrouded/TownPortal/tools/tex-wanted.txt",FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,0,nullptr);
                if(f!=INVALID_HANDLE_VALUE){DWORD wrote{};WriteFile(f,text,DWORD(n),&wrote,nullptr);CloseHandle(f);}
            }
        }
#endif
        // Only while the far view was collected for in the last three seconds.
        if(!wanted||mip<0.f||GetTickCount64()-g_farTextures.collectedAt.load()>3000)return;
        const uint32_t live=g_farTextures.live.load();
        const uint32_t count=std::min(g_farTextures.count[live].load(),FarTextures::kMost);
        for(uint32_t k=0;k<count;++k){
            const uint32_t i=g_farTextures.index[live][k];
            if(i<most&&wanted[i]>mip)wanted[i]=mip;
        }
        g_farTextures.solverCount.store(most);
        const uint32_t first=g_farTextures.rangeFirst.load(),last=std::min(g_farTextures.rangeLast.load(),most);
        uint32_t forced=0;
        for(uint32_t i=first;i<last&&i<65536&&forced<600;++i)
            if((g_farTextures.real[i>>6].load()>>(i&63)&1)&&wanted[i]>0.f){wanted[i]=0.f;++forced;}
        if(const float floorMip=g_farTextures.floorMip.load();floorMip>=0.f){
            const uint32_t reach=std::min(g_farTextures.floorReach.load(),most);
            for(uint32_t i=0;i<reach;++i)if(wanted[i]>floorMip)wanted[i]=floorMip;
        }
        ++g_farTextures.applied;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
void FarMipsHook(uint8_t** task) {
    {
        greeneye::Lease lease;
        if(lease)FarMipsApply(task);
    }
    g_mipSolver(task);
}
void TickOwned(const void* description) {
    NoteGraphicsLock();
    __try {
        UpdateGoneTree();
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
        EnsurePoolHook();
        SyncOwnedActors();
        SyncOwnedPlants(primary,owned);
        SyncOwnedEffects();
        FarTexturesCollect(owned);
#ifdef TOWN_PORTAL_DEV
        {   // Dev: what the scene tick is given (is there a camera position in it?), every 30 s.
            static uint64_t told=0;
            if(const uint64_t now=GetTickCount64();now-told>30000){
                told=now;char line[420]{};
                int n=std::snprintf(line,sizeof(line),"Town Portal: scene tick description:");
                for(int o=0;o<0xa0&&n<400;o+=4)n+=std::snprintf(line+n,sizeof(line)-n," %08x",At<uint32_t>(const_cast<void*>(description),o));
                greeneye::Host()->log(line);
            }
        }
#endif
        g_sceneTick(g_target->view,description);
        // Dev: tools/tree.ticks holds a number of extra scene ticks a frame while the far
        // terrain data is still coming in (does it load by the frame or by the clock?).
        if(g_treeSeen&&!g_treeReadyAt){
            static uint64_t checked=0;static unsigned extra=0;
            if(const uint64_t now=GetTickCount64();now-checked>1000){
                checked=now;extra=0;
                if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/tree.ticks",L"r")){
                    if(std::fscanf(f,"%u",&extra)!=1||extra>16)extra=0;
                    std::fclose(f);
                }
            }
            for(unsigned i=0;i<extra;++i)g_sceneTick(g_target->view,description);
        }
    }
    __except(TargetFault(GetExceptionInformation())) {g_renderOnce=false;}
}
void MotionPipelineGuard(); // the motion pass, below
void LiveLifecycleGuard(void* scene);
void CompositeRefreshGuard() {
    NoteGraphicsLock();
    __try {CompositeRefreshPipelines();}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;}
}
void SceneTickHook(void* scene,const void* description) {
    greeneye::Lease lease;
    // Allocation, binding and destruction must precede BeginFrame and its
    // command recording; never perform them inside the main render pass.
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        Stopwatch watch(0);
        ++g_frameNo;
        WatchBake();
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
            MotionPipelineGuard();
            SharedGpuTickGuard();
            GagGpuTickGuard();
        }
        if(auto action=g_targetRequest.exchange(0)) {
            std::lock_guard lock(g_lock);
            ++g_target->requested;
            ProcessTargetRequest(scene,action);
            ++g_target->completed;
        }
        {
            std::lock_guard lock(g_lock);
            LiveLifecycleGuard(scene);
            RememberObjectsGuard(scene);
        }
    }
    {
        Stopwatch watch(1);
        g_sceneTick(scene,description);
    }
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        Stopwatch watch(2);
        std::lock_guard lock(g_lock);g_nativeThreads->update=GetCurrentThreadId();
        // Keep native scene time, asset transfers and world subscriptions current
        // even when GPU draws are throttled. The update description is a delta.
        if(g_target->view&&g_world->ownedBound&&!g_target->error)
            TickOwned(description);
    }
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene) {
        std::lock_guard lock(g_lock);
        const auto now=GetTickCount64();
        static uint32_t frame=0;++frame;
        const float coverage=g_liveCoverage.load();
        // Every game frame, however small the portal is on screen. (It was every second or third
        // frame under 5% / 1.2% of the screen, for speed. But effects keep their state in a
        // table whose halves swap with the game's frame number: a far view that sits out a
        // frame finds the other half next time, without its entries, and its flame stood
        // frozen as flat shapes, 2026-11-10. tools/live.skip (dev) brings the skipping back.)
        static uint64_t skipChecked=0;static bool skipping=false;
        if(now-skipChecked>1000){skipChecked=now;skipping=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.skip")!=INVALID_FILE_ATTRIBUTES;}
        const uint32_t every=g_liveEveryMs.load()||!skipping?1u:coverage<0.012f?3u:coverage<0.05f?2u:1u;
        if(now-g_gameplayHudTick.load()<250&&(now<g_liveUntil||now<g_displayProbeUntil)&&now-g_ownedRenderTick>=(now<g_liveUntil?g_liveEveryMs.load():100u)&&g_world->ownedBound&&
           (frame%every==0||now>=g_liveUntil))
            g_renderOnce=true;
    }
    if(!lease||!g_world||reinterpret_cast<uint64_t>(scene)!=g_world->scene||
       !g_renderOnce.load()||g_ownedPrepared.load()) return;
    std::lock_guard lock(g_lock);
    if(!g_target->view||!g_world->ownedBound||g_target->error) return;
    PrepareOwned();
}
// The second camera's terrain meshes get their surface materials baked once, from its brick
// tree as it is at that moment (the game's bake_voxel_materials pass). A new tree is coarse for
// its first half minute or so, and meshes made in that time kept flat, untextured surfaces for
// good (towers and walls in one plain colour, 2026-11-09). So until the tree has settled, the
// view is rendered with its terrain mesher out of sight: the game then makes no meshes
// (FUN_14012c3b0 skips the mesher's update when the view has none), and makes them all once
// it is back. Settled: the tree's detail factor (+0x10494, which the game steers by how full
// the tree's pools are) and its pool's free count have not moved for a second and a half,
// two seconds or more after the tree was first seen; or 90 s whatever they do.
bool OwnedTreeReady() {
    auto* scene=At<uint8_t*>(g_target->view,0x98);
    auto* tree=scene?At<uint8_t*>(scene,0xa80):nullptr;
    if(!tree)return true; // no tree of its own: nothing to wait for
    const uint64_t now=GetTickCount64();
    const float factor=At<float>(tree,0x10494);
    if(tree!=g_treeSeen){
        g_treeSeen=tree;g_treeSeenAt=now;g_treeReadyAt=0;g_treeMovedAt=now;g_treeFactor=factor;
        // A new tree starts at the coarsest detail (factor 16) and the game's controller
        // (FUN_14005afa0) brings it down by about a hundredth of itself a frame while the
        // tree's pools are under 85% full: over 20 s to reach the 1.5 it settles at here.
        // So it starts from 2 instead (dev: tools/tree.start holds another value, 16 for the
        // game's own course); the controller still raises it when the pools fill (95%).
        // Measured 2026-11-10: data settled after 2 to 4 s in place of 22.
        float start=2.f;
        if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/tree.start",L"r")){
            if(std::fscanf(f,"%f",&start)!=1||!(start>=0.2f))start=2.f;
            std::fclose(f);
        }
        if(start<16.f&&factor>start){At<float>(tree,0x10494)=start;g_treeFactor=start;}
    }
    if(g_treeReadyAt)return true;
    if(std::fabs(factor-g_treeFactor)>0.02f){g_treeFactor=factor;g_treeMovedAt=now;}
    {   // Data still coming in: the pool's free count (+0x10398 of +0x110) is still changing.
        // (While the game loads, its controller leaves the factor alone, so a still factor
        // alone does not mean the data is there.)
        static uint8_t* countedTree=nullptr;static uint32_t counted=0;
        const uint32_t free=At<uint32_t>(tree,0x10398),all=At<uint32_t>(tree,0x110);
        if(tree!=countedTree){countedTree=tree;counted=free;g_treeMovedAt=now;}
        else if((free>counted?free-counted:counted-free)>all/400u){counted=free;g_treeMovedAt=now;}
        if(all&&free>=all-all/50u)g_treeMovedAt=now; // nothing loaded yet
    }
    const uint64_t age=now-g_treeSeenAt;
    // Dev: tools/tree.settle holds "least still" in milliseconds in place of 10000 and 5000.
    static uint64_t checked=0;static unsigned least=2000,still=1500;
    if(now-checked>1000){
        checked=now;least=2000;still=1500;
        if(FILE* f=_wfopen(L"C:/Mods/Enshrouded/TownPortal/tools/tree.settle",L"r")){
            unsigned a=0,b=0;
            if(std::fscanf(f,"%u %u",&a,&b)==2&&a>=500&&b>=500){least=a;still=b;}
            std::fclose(f);
        }
    }
    {   // Dev: the factor's course, twice a second.
        static uint64_t told=0;
        if(now-told>500){told=now;char line[120]{};std::snprintf(line,sizeof(line),"Town Portal: far terrain data at %.1f s: detail factor %.3f, pool %u of %u free",double(age)/1000.0,factor,At<uint32_t>(tree,0x10398),At<uint32_t>(tree,0x110));greeneye::Host()->log(line);}
    }
    if((age>least&&now-g_treeMovedAt>still)||age>90000){
        g_treeReadyAt=now;
        char line[160]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view: the far terrain data settled after %.0f s (detail factor %.2f); meshing starts",double(age)/1000.0,factor);
        greeneye::Host()->log(line);
        return true;
    }
    return false;
}
void RenderHoldingMesher(void* result,void* context,void* input,void* target,const uint8_t* mapped,bool destination) {
    auto* state=At<uint8_t*>(g_target->view,0xa0);
    void* mesher=state?At<void*>(state,0x978):nullptr;
    // Dev test: tools\mesher.hidden keeps the mesher out of sight for good (bricks only).
    static uint64_t hiddenChecked=0;static bool hidden=false;
    if(const uint64_t now=GetTickCount64();now-hiddenChecked>1000){hiddenChecked=now;hidden=GetFileAttributesW(L"C:\Mods\Enshrouded\TownPortal\tools\mesher.hidden")!=INVALID_FILE_ATTRIBUTES;}
    const bool hold=mesher&&(!OwnedTreeReady()||hidden);
    {   // Dev test: tools\state6d sets the view state's byte +0x6d (1 in the player's view) every frame.
        static uint64_t checked=0;static bool on=false;
        if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;on=GetFileAttributesW(L"C:\Mods\Enshrouded\TownPortal\tools\state6d")!=INVALID_FILE_ATTRIBUTES;}
        if(on&&state)At<uint8_t>(state,0x6d)=1;
    }
    {   // Dev: tools\exit-clip.disabled turns the exit clip plane off (read every second).
        static uint64_t clipChecked=0;
        if(const uint64_t now=GetTickCount64();now-clipChecked>1000){
            clipChecked=now;
            const bool clip=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\exit-clip.disabled")==INVALID_FILE_ATTRIBUTES;
            g_forceCameraCut=g_forceCameraCut||clip!=g_exitClipEnabled;g_exitClipEnabled=clip;
        }
    }
    // Dev test: tools\tree.hidden renders with the scene's brick tree out of sight (no bricks).
    static uint64_t treeChecked=0;static bool treeHidden=false;
    if(const uint64_t now=GetTickCount64();now-treeChecked>1000){treeChecked=now;treeHidden=GetFileAttributesW(L"C:\Mods\Enshrouded\TownPortal\tools\tree.hidden")!=INVALID_FILE_ATTRIBUTES;}
    auto* modelScene=At<uint8_t*>(g_target->view,0x98);
    void* tree=modelScene&&treeHidden?At<void*>(modelScene,0xa80):nullptr;
    if(tree)At<void*>(modelScene,0xa80)=nullptr;
    if(hold)At<void*>(state,0x978)=nullptr;
    __try {RenderWithExitPlane(result,context,input,target,mapped,destination);}
    __finally {if(hold)At<void*>(state,0x978)=mesher;if(tree)At<void*>(modelScene,0xa80)=tree;}
}
bool FarShotFrame(const void* input,const PortalEndpoint& to,uint8_t* pose);
void FarShotChoose(const void* input);
void RenderOwnedOnce(void* context,void* input) {
    NoteGraphicsLock();
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
            // Two ends in sight whose far sides are near each other (the two ends of one portal
            // side by side): the one second camera renders for them in turn, each every other
            // frame, into an image of its own. tools/live.one (dev) keeps it to the nearest.
            // The frame a render call gives is the one for the camera of the call BEFORE it
            // (found 2026-11-10: with each frame stored for the end of its own call both ends
            // came out empty, only the swirl; stored for the end of the call before, both show).
            // So while taking turns a call's frame is stored for the other end.
            bool second=false;
            {
                static uint64_t checked=0;static bool one=false;static bool turn=false;
                const uint64_t now=GetTickCount64();
                if(now-checked>1000){checked=now;one=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.one")!=INVALID_FILE_ATTRIBUTES;}
                FarShotChoose(input);
                bool can=!one&&!g_farShot.active&&g_liveSecond&&g_liveSecond->ticket&&g_liveSecond->image.context&&g_liveView.ready&&g_livePortal2.active&&now-std::min(now,g_liveSecondTick)<250;
                if(can){
                    const PortalEndpoint& a=g_liveField?g_livePortal.baseEnd:g_livePortal.fieldEnd;
                    const PortalEndpoint& b=g_liveField2?g_livePortal2.baseEnd:g_livePortal2.fieldEnd;
                    const float dx=a.wx-b.wx,dy=a.wy-b.wy,dz=a.wz-b.wz;
                    can=dx*dx+dy*dy+dz*dz<40.f*40.f;
                }
                static bool told=false;
                if(can&&!told){told=true;greeneye::Host()->log("Town Portal: live view: two ends in sight take turns with the second camera");}
                // Dev: tools/live.pairs gives each end two frames in a row and stores only the second
                // (is what the capture reads of the view one frame old?).
                static uint32_t phase=0;
                if(can&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.pairs")!=INVALID_FILE_ATTRIBUTES){
                    phase=(phase+1)&3;turn=phase>=2;second=turn;g_liveNoStore=!(phase&1);
                } else {turn=can&&!turn;second=turn;g_liveNoStore=false;}
                g_liveAlternating=can;
                static uint64_t toldAt=0;
                if(can&&now-toldAt>2000){
                    toldAt=now;char line[260]{};
                    std::snprintf(line,sizeof(line),"Town Portal: turns: first captures %llu skipped %llu valid %d ready %d age %llu ms; second captures %llu skipped %llu valid %d ready %d age %llu ms; frame %ux%u want %ux%u",
                        g_liveView.captures,g_liveView.skipped,int(g_liveView.snapshot.valid),int(g_liveView.ready),now-std::min(now,g_liveView.snapshot.tick),
                        g_liveView2.captures,g_liveView2.skipped,int(g_liveView2.snapshot.valid),int(g_liveView2.ready),now-std::min(now,g_liveView2.snapshot.tick),
                        g_ownedSize.width,g_ownedSize.height,g_ownedSize.wantWidth,g_ownedSize.wantHeight);
                    greeneye::Host()->log(line);
                }
            }
            g_nowPortal=second?g_livePortal2:g_livePortal;g_nowField=second?g_liveField2:g_liveField;
            // Dev: tools/live.same renders the first end on the second's turn too (into the second's image).
            if(second&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.same")!=INVALID_FILE_ATTRIBUTES){g_nowPortal=g_livePortal;g_nowField=g_liveField;}
            g_liveNow=second?&g_liveView2:&g_liveView;
            if(g_farShot.active){g_nowPortal=g_farShot.portal;g_nowField=g_farShot.field;g_liveNow=&g_liveView;}
            PortalEndpoint from=g_nowField?g_nowPortal.fieldEnd:g_nowPortal.baseEnd;
            const PortalEndpoint to=g_nowField?g_nowPortal.baseEnd:g_nowPortal.fieldEnd.Turned();
            // Seen from behind, a portal shows the same view turned round (portal_composite.frag
            // Window, side): the camera is carried as if the viewer stood in front, turned half
            // way round the portal. Without this it came out in front of the far endpoint,
            // looking at what stands behind it with nothing cut away (2026-11-09).
            {
                FixedPos at{};std::memcpy(&at,static_cast<const uint8_t*>(input)+0x38,sizeof(at));
                math::Vec3 camera{};ToFloat(at,camera.x,camera.y,camera.z);
                if((camera.x-from.wx)*from.nx+(camera.z-from.wz)*from.nz<0)from=from.Turned();
            }
            if(!MapCameraEnds(&pose,input,from,to)) return;
            const bool farShot=FarShotFrame(input,to,pose.data());
            // Where the player's camera looks, from the world view's matrices (last frame's):
            // the view's third row, along the sign the projection gives depth.
            if(auto* world=WorldState();world&&!g_liveNoAim&&!farShot){
                // The player's camera in the world, from the world view's matrices (last
                // frame's): the view's rows are its right, up and (along the sign the
                // projection gives depth) forward; the projection's [5] is 1 / tan(half fov).
                const float* view=&At<float>(world,0x1000);const float* projection=&At<float>(world,0x10c0);
                const float sign=projection[11]<0?-1.f:1.f;
                const float tanY=std::fabs(projection[5])>1e-4f?1.f/std::fabs(projection[5]):0.f;
                const float aspect=std::fabs(projection[0])>1e-4f?std::fabs(projection[5]/projection[0]):1.f;
                g_ownedSize.mainDensity=tanY>0.01f?float(At<uint32_t>(world,0xb0))/(2.f*tanY):0.f;
                AimAtExit(pose.data(),{view[0],view[4],view[8]},{view[1],view[5],view[9]},
                          {view[2]*sign,view[6]*sign,view[10]*sign},tanY,aspect,from,to);
            }
            std::memcpy(mapped,pose.data(),sizeof(mapped));destination=true;
            // This frame becomes the portal's live view (LiveCapture, from UpscaleHook).
            FixedPos eye{};std::memcpy(&eye,mapped+0x38,sizeof(eye));
            auto& l=*g_liveNow;
            if(!farShot&&(l.armedPortal!=g_nowPortal.portalId||l.armedField!=g_nowField||!l.since)){l.since=GetTickCount64();l.ready=false;l.shownSince=0;l.snapshot.valid=false;}
            if(farShot)l.armed=false;
            else{l.armed=!g_liveNoStore;l.armedPortal=g_nowPortal.portalId;l.armedField=g_nowField;l.destination=to;}
            if(g_liveNow==&g_liveView2)l.ready=g_liveView.ready; // the same far scene
            // Taking turns is no camera cut: the view carries its last frame over by its real
            // matrices, whichever end that was for.
            // Dev: tools/live.lag. If the frame a render call gives is the one for the camera of
            // the call BEFORE it, then while taking turns this call's frame is the other end's:
            // it is stored for that one (with its own destination and camera, kept from its turn).
            ToFloat(eye,l.camera.x,l.camera.y,l.camera.z);
            l.cameraShown=l.camera;
            // (tools/live.lag, dev: the frame stored for the other end, the stopgap used before the
            // frame was stored with its own matrices.)
            if(g_liveAlternating&&!g_liveNoStore&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.lag")!=INVALID_FILE_ATTRIBUTES){
                LiveView& other=g_liveNow==&g_liveView2?g_liveView:g_liveView2;
                l.armed=false;
                if(&other==&g_liveView2)other.ready=g_liveView.ready;
                other.armed=other.since!=0;
                other.cameraShown=other.camera;
                g_liveNow=&other;
            }
            mapped[0]=!g_liveAlternating&&(g_renderedPortal!=g_nowPortal.portalId||g_renderedField!=g_nowField);
            // Without a cut both ends came out empty (the swirl only): the view's culling works
            // from its last frame, which was the other end's. tools/live.nocut (dev) tries it so.
            if(g_liveAlternating&&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.nocut")==INVALID_FILE_ATTRIBUTES)mapped[0]=1;
            if(farShot&&g_farShot.frames==1)mapped[0]=1; // a cut into the snapshot pose (and back: FarShotDoneNow)
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
        {
            ApplyOwnedSize(destination&&GetTickCount64()<g_liveUntil);
            {   // Dev: the render input's view mask and focus fields, far against player's.
                static uint64_t toldAt=0;
                if(const uint64_t t=GetTickCount64();t-toldAt>15000){
                    toldAt=t;char line[300]{};
                    uint8_t* in=const_cast<uint8_t*>(static_cast<const uint8_t*>(input));
                    std::snprintf(line,sizeof(line),"Town Portal: render input: far +0x78 %x %x %x %x, +0x2a8 %x %x %x; player's +0x78 %x %x %x %x, +0x2a8 %x %x %x",
                        At<uint32_t>(mapped,0x78),At<uint32_t>(mapped,0x7c),At<uint32_t>(mapped,0x80),At<uint32_t>(mapped,0x84),At<uint32_t>(mapped,0x2a8),At<uint32_t>(mapped,0x2ac),At<uint32_t>(mapped,0x2b0),
                        At<uint32_t>(in,0x78),At<uint32_t>(in,0x7c),At<uint32_t>(in,0x80),At<uint32_t>(in,0x84),At<uint32_t>(in,0x2a8),At<uint32_t>(in,0x2ac),At<uint32_t>(in,0x2b0));
                    greeneye::Host()->log(line);
                }
            }
            LARGE_INTEGER start{},end{},frequency{};QueryPerformanceCounter(&start);
            RenderHoldingMesher(result,context,input,target,mapped,destination);
            QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
            const uint64_t micro=uint64_t(end.QuadPart-start.QuadPart)*1000000ull/uint64_t(frequency.QuadPart);
            if(micro>g_slowest[3])g_slowest[3]=micro;
        }
        g_liveView.armed=false;g_liveView2.armed=false;
        ++g_ownedRenderCalls;g_ownedRenderTick=GetTickCount64();
        g_ownedRenderFlags=At<uint32_t>(result,0);
        // Shown live once the far end has loaded for the second camera: every readiness flag
        // set or four seconds of frames, and its terrain meshes under way for six seconds
        // (the snapshot stays up while the far terrain data settles, see OwnedTreeReady).
        // The meshes are there when the game's own finished flags (voxel meshing is bit 7)
        // have all stayed set for 20 frames in a row, a second or more after meshing began.
        static uint32_t finishedRun=0;static uint64_t flagsTold=0;
        if(!destination||g_liveView.ready||!g_treeReadyAt)finishedRun=0;
        else finishedRun=(g_ownedRenderFlags&0x7ff)==0x7ff?finishedRun+1:0;
        if(destination&&!g_liveView.ready&&g_treeReadyAt&&GetTickCount64()-flagsTold>250){
            flagsTold=GetTickCount64();char line[140]{};
            std::snprintf(line,sizeof(line),"Town Portal: far meshes at %.1f s: finished flags 0x%x, %u frames in a row",double(GetTickCount64()-g_treeReadyAt)/1000.0,g_ownedRenderFlags,finishedRun);
            greeneye::Host()->log(line);
        }
        if(destination&&!g_liveView.ready&&g_liveView.since&&
           ((g_ownedRenderFlags&0x7ff)==0x7ff||GetTickCount64()-g_liveView.since>4000)&&
           g_treeSeen==At<uint8_t*>(At<uint8_t*>(g_target->view,0x98),0xa80)&&
           (!g_treeSeen||(g_treeReadyAt&&(GetTickCount64()-g_treeReadyAt>6000||(finishedRun>=20&&GetTickCount64()-g_treeReadyAt>1000))))){
            g_liveView.ready=true;
            greeneye::Host()->log("Town Portal: live view: the far end is ready; the portal shows it live");
        }
        if(destination&&(g_ownedRenderFlags&1)){g_renderedPortal=g_nowPortal.portalId;g_renderedField=g_nowField;++g_mappedFrames;}
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
// (system local_player_hide, RVA 0x382ba0: Hide[1] = threshold > camera distance; the
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
// Whether this frame of the second camera is a FarShot frame (see FarShot): if so the pose is
// the snapshot camera's at `to`, the far end. Called from RenderOwnedOnce with the player's input.
// Starts a FarShot when an end in sight has been live and settled for 2 s and has no picture of
// its own from the last two minutes: the first end, or the second while two take turns (their
// turns pause for the shot's few frames). Render thread, before the end of the frame is chosen.
void FarShotChoose(const void* input) {
    const uint64_t now=GetTickCount64();
    static bool wasReady=false;static uint64_t readySince=0;
    const bool ready=g_liveView.ready;
    if(ready&&!wasReady)readySince=now;
    wasReady=ready;
    if(GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/farshot.off")!=INVALID_FILE_ATTRIBUTES){g_farShot.active=false;return;}
    if(g_farShot.active){
        if(!ready||now>g_liveUntil||now-g_farShot.startedAt>1500){
            if(now-g_farShot.startedAt>1500)greeneye::Host()->log("Town Portal: far snapshot gave up (no frame came back)");
            g_farShot.active=false;g_forceCameraCut=true;
        }
        return;
    }
    static uint64_t lastShot=0;
    // Half a second after the end shows live (the far side has settled by then: "ready").
    if(!ready||now-readySince<500||now-lastShot<1500)return;
    auto due=[&](const PortalInstance& p,bool field){
        if(!p.active)return false;
        const PortalEndpoint to=field?p.baseEnd:p.fieldEnd.Turned();
        for(const auto& d:g_farShotsDone){
            const float dx=d.x-to.wx,dz=d.z-to.wz;
            if(d.tick&&d.portalId==p.portalId&&d.field==field&&dx*dx+dz*dz<0.25f&&now-d.tick<120000)return false;
        }
        return true;
    };
    const PortalInstance* pick=nullptr;bool field=false;
    if(due(g_livePortal,g_liveField)){pick=&g_livePortal;field=g_liveField;}
    else if(g_liveAlternating&&due(g_livePortal2,g_liveField2)){pick=&g_livePortal2;field=g_liveField2;}
    if(!pick)return;
    const PortalEndpoint to=field?pick->baseEnd:pick->fieldEnd.Turned();
    SnapshotRequest r{};r.end=to;r.distance=kFarShotDistance;
    if(!SnapshotPose(input,r))return;
    {
        static uint64_t told=0;
        if(now-told>5000){told=now;char line[120]{};
            std::snprintf(line,sizeof(line),"Town Portal: far snapshot started for portal %llu %s end",static_cast<unsigned long long>(pick->portalId),field?"base":"field");
            greeneye::Host()->log(line);}
    }
    g_farShot={};g_farShot.active=true;g_farShot.startedAt=now;g_farShot.portalId=pick->portalId;g_farShot.field=field;g_farShot.portal=*pick;g_farShot.end=to;
    std::memcpy(g_farShot.pose,r.pose,sizeof(g_farShot.pose));
    lastShot=now;
}
// Whether this frame of the second camera is a FarShot frame: if so its pose is the snapshot
// camera's at the shot's end (RenderOwnedOnce has made that end this frame's).
bool FarShotFrame(const void* input,const PortalEndpoint& to,uint8_t* pose) {
    (void)input;(void)to;
    g_farShot.captureNow=false;
    if(!g_farShot.active)return false;
    std::memcpy(pose+0x38,g_farShot.pose,sizeof(g_farShot.pose));
    At<float>(pose,0x78)=kSnapshotFov;
    if(++g_farShot.frames>=kFarShotFrames)g_farShot.captureNow=true;
    return true;
}
extern int g_farShotWhy;
void FarShotDoneNow(bool stored) {
    if(!stored){
        static uint64_t told=0;
        if(const uint64_t now=GetTickCount64();now-told>5000){
            told=now;char line[120]{};
            std::snprintf(line,sizeof(line),"Town Portal: far snapshot not stored (step %d)",g_farShotWhy);
            greeneye::Host()->log(line);
        }
    }
    if(stored){
        FarShotDone* slot=&g_farShotsDone[0];
        for(auto& d:g_farShotsDone){
            if(d.portalId==g_farShot.portalId&&d.field==g_farShot.field){slot=&d;break;}
            if(d.tick<slot->tick)slot=&d;
        }
        *slot={g_farShot.portalId,g_farShot.field,g_farShot.end.wx,g_farShot.end.wz,GetTickCount64()};
    }
    g_farShot.active=false;g_farShot.captureNow=false;g_forceCameraCut=true;
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
// A whole snapshot image copied to another on the GPU, recorded the way the game copies one
// texture into another (2026-11-02 build, 12ba10): both declared for transfer (layout 8 read
// 0x200000, layout 9 write 0x100000000), the barriers flushed, then the CopyTexture packet
// (148780: list, destination, its layout, source, its layout, regions). A region is a source
// and a destination Region (aspect, mip, layer, layers, offset, extent).
void CopySnapshotImage(void* scope,const ImageView& from,const ImageView& to) {
    const uint64_t read=0x200000,write=0x100000000ull;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&from,8,&read);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&to,9,&write);
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    sharing::native::Region regions[2]{};
    regions[0].width=kSnapshotWidth;regions[0].height=kSnapshotHeight;
    struct {const void* regions;uint64_t count;} list{regions,1};
    void* source=Native<void*(*)(const ImageView*)>(0x10308e0)(&from);
    void* destination=Native<void*(*)(const ImageView*)>(0x10308e0)(&to);
    if(!source||!destination)return;
    Native<void(*)(void*,void*,uint8_t,void*,uint8_t,const void*)>(0x148850)(At<void*>(scope,0x10),destination,9,source,8,&list);
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
    Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
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
    // Draw into the scratch render target and copy it into the slot; a slot image made as a
    // render target (kept from a build before the scratch, across a hot reload) is drawn into.
    const bool scratch=g_snapshotScratch->image.context!=0;
    if(!scratch&&!(InspectImage(snapshot.image).usage&8))return;
    if(scratch)g_liveView.snapshot.valid=false; // the live view's frame is in the scratch target
    const ImageView& drawTarget=scratch?g_snapshotScratch->image:snapshot.image;
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
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&frame,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    const uint8_t attachment[]={0,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&drawTarget,attachment,"town portal snapshot");
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    alignas(8) uint8_t bindings[3][0x28]{};
    for(int i=0;i<2;++i){bindings[i][0]=3;At<ImageView>(bindings[i],8)=i?depth:frame;bindings[i][0x18]=7;}
    bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
    alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
    auto* graph=At<uint8_t*>(scope,0);
    void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
    auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=c.capturePipeline;
    command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
    bool drawn=false;
    if(auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3)){
        std::memcpy(mapped,vertices,sizeof(vertices));drawn=true;
    }
    Native<void(*)(void*)>(0x6c960)(scope);
    if(!drawn)return;
    if(scratch)CopySnapshotImage(scope,drawTarget,snapshot.image);
    g_snapshotShroud[slot]=r.shroud;g_snapshotDefault[slot]=false;
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
// jittered: the frame is the second camera's raw one, where each pixel sits at the projection
// with its jitter; false for a frame its upscaler has already steadied.
bool g_liveFinalTest{}; // dev: tools\\live.final shows the second camera's finished image of the frame before (is the flame in it?)
// The second camera's frame is stored later in the same game frame, from the player's view's
// own render (RunPendingFar, EarlyColourHook). The view state's matrices (state +0x1000..) are
// written at the END of a view's render call, so at the second camera's upscaler they were
// still the call before's: a frame was stored with the camera of the frame before (a small
// slip with one end; with two ends taking turns, the other end's camera). By the player's
// render they are this frame's. The frame's colour and depth are kept until then.
struct PendingFar {bool has{};ImageView color{},depth{};float exposure{},invExposure{};bool jittered{};LiveView* view{};uint8_t* state{};};
PendingFar g_pendingFar;
const ImageView* g_givenDepth{};   // set while a pending frame is stored: its depth
const ImageView* g_givenColor{};   // and its colour, for the portal drawn into it
float g_givenInvExposure{};
LiveView* g_liveRefresh{};bool g_liveRefreshJittered{};
void LiveCapture(void* scope,uint8_t* state,const ImageView& frame,float exposure,bool jittered) {
    auto& c=*g_composite;auto& l=*g_liveNow;
    const bool secondEnd=&l==&g_liveView2;
    l.armed=false;
    if(!c.capturePipeline||!c.snapshotTicket||!g_snapshotScratch->image.context||(secondEnd&&!(g_liveSecond&&g_liveSecond->image.context))){++l.skipped;return;}
    const auto info=InspectImage(frame);
    ImageView depth{};
    if(g_givenDepth)depth=*g_givenDepth;
    else Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
    const auto depthInfo=InspectImage(depth);
    static bool described=false;
    if(!described){
        described=true;char line[200]{};
        std::snprintf(line,sizeof(line),"Town Portal: live view frame %ux%u format %u, depth %ux%u format %u, exposure %.4g",
                      info.width,info.height,info.format,depthInfo.width,depthInfo.height,depthInfo.format,exposure);
        greeneye::Host()->log(line);
    }
    if(!info.valid||!depthInfo.valid||info.width!=depthInfo.width||info.height!=depthInfo.height||(info.format!=0x61&&!g_liveFinalTest)||
       !(exposure>0)||!std::isfinite(exposure)){++l.skipped;return;}
    const PortalEndpoint& end=l.destination;
    const ImageView& drawTarget=secondEnd?g_liveSecond->image:g_snapshotScratch->image;
    // The view state's matrices are written at the END of a render call, so here they are the
    // call before's. With one end that is this end a frame ago. With two ends taking turns it
    // is the OTHER end's camera: stored with those, an end showed the picture taken for the
    // other one, "almost a mirror" (user, 2026-11-10). So while taking turns the state's
    // matrices are kept for the other end, and this end's frame is stored with its own from
    // its turn before.
    const float* view=&At<float>(state,0x1000);
    const float* projection=&At<float>(state,0x10c0);
    if(g_liveAlternating){
        LiveView& other=secondEnd?g_liveView:g_liveView2;
        std::memcpy(other.view,view,sizeof(other.view));std::memcpy(other.projection,projection,sizeof(other.projection));other.haveMatrices=true;
        if(!l.haveMatrices){++l.skipped;return;}
        view=l.view;projection=l.projection;
    } else {g_liveView.haveMatrices=false;g_liveView2.haveMatrices=false;}
    // Window space of the endpoint the view leads to (SnapshotRows): from kWindowStart in front of it.
    const math::Vec3 origin{end.wx+end.nx*kWindowStart,end.wy+1.62f,end.wz+end.nz*kWindowStart};
    const math::Vec3 right{end.rx,0,end.rz},up{0,1,0},normal{end.nx,0,end.nz};
    float local[16]{};
    if(!math::PortalLocalFromScreen(view,projection,origin,right,up,normal,local)){++l.skipped;return;}
    // The anti-aliased colour sits where the projection without its jitter puts it.
    float steady[16];std::memcpy(steady,projection,sizeof(steady));
    if(!jittered){steady[8]=steady[9]=0;}
    double clip[12]{};
    for(int col=0;col<4;++col){
        double v[4]{};
        for(int k=0;k<4;++k)v[k]=view[col*4+k];
        double out[4]{};for(int row=0;row<4;++row)for(int k=0;k<4;++k)out[row]+=double(steady[k*4+row])*v[k];
        clip[col]=out[0];clip[4+col]=out[1];clip[8+col]=out[3];
    }
    for(double value:clip)if(!std::isfinite(value)){++l.skipped;return;}
    Snapshot taken{};
    std::memcpy(taken.clip,clip,sizeof(clip));
    taken.x=end.wx;taken.y=end.wy;taken.z=end.wz;taken.nx=end.nx;taken.nz=end.nz;
    float rows[12]{};
    SnapshotRows(taken,end,rows);
    float vertices[3*kVertexFloats]{};
    const float xy[3][2]={{-1,-1},{-1,3},{3,-1}};
    for(int i=0;i<3;++i){
        float* v=vertices+i*kVertexFloats;v[0]=xy[i][0];v[1]=xy[i][1];
        std::memcpy(v+8,local,sizeof(local));std::memcpy(v+40,rows+8,4*sizeof(float));v[44]=1;v[45]=1;v[46]=1; // whole frame, smooth
    }
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&frame,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    const uint8_t attachment[]={0,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&drawTarget,attachment,"town portal live view");
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    alignas(8) uint8_t bindings[3][0x28]{};
    for(int i=0;i<2;++i){bindings[i][0]=3;At<ImageView>(bindings[i],8)=i?depth:frame;bindings[i][0x18]=7;}
    bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
    alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
    auto* graph=At<uint8_t*>(scope,0);
    void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
    auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=c.capturePipeline;
    command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
    bool drawn=false;
    if(auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3)){
        std::memcpy(mapped,vertices,sizeof(vertices));drawn=true;
    }
    Native<void(*)(void*)>(0x6c960)(scope);
    if(!drawn){++l.skipped;return;}
    taken.image=drawTarget;taken.cx=l.cameraShown.x;taken.cy=l.cameraShown.y;taken.cz=l.cameraShown.z;
    taken.exposure=exposure;taken.tick=GetTickCount64();taken.valid=true;
    l.snapshot=taken;l.portalId=l.armedPortal;l.field=l.armedField;g_liveFrameNo=g_frameNo;
    if(!l.captures++)greeneye::Host()->log("Town Portal: live view: first frame stored for the portal");
    g_liveRefresh=&l;g_liveRefreshJittered=jittered;
}
// The frame just stored was placed with the camera of the call before (see LiveCapture): with
// a fast turn close to a portal the picture then sat a frame's turn off and jerked (user,
// 2026-11-10). Once the second camera's render call is over its view state holds that frame's
// own matrices, so before the player's view draws the portal the stored frame's placement is
// worked out again from them. tools/live.norefresh (dev) leaves it as stored.
void RefreshLivePlacement() {
    LiveView* l=g_liveRefresh;g_liveRefresh=nullptr;
    if(!l||!l->snapshot.valid||!g_target||!g_target->view||g_target->error)return;
    static uint64_t checked=0;static bool off=false;
    const uint64_t now=GetTickCount64();
    if(now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.norefresh")!=INVALID_FILE_ATTRIBUTES;}
    if(off||now-std::min(now,l->snapshot.tick)>200)return;
    auto* state=At<uint8_t*>(g_target->view,0xa0);
    if(!state||!At<uint8_t>(state,0x1110))return;
    const float* view=&At<float>(state,0x1000);
    float steady[16];std::memcpy(steady,&At<float>(state,0x10c0),sizeof(steady));
    if(!g_liveRefreshJittered){steady[8]=steady[9]=0;}
    double clip[12]{};
    for(int col=0;col<4;++col){
        double v[4]{};
        for(int k=0;k<4;++k)v[k]=view[col*4+k];
        double out[4]{};for(int row=0;row<4;++row)for(int k=0;k<4;++k)out[row]+=double(steady[k*4+row])*v[k];
        clip[col]=out[0];clip[4+col]=out[1];clip[8+col]=out[3];
    }
    for(double value:clip)if(!std::isfinite(value))return;
    std::memcpy(l->snapshot.clip,clip,sizeof(clip));
    l->snapshot.cx=l->camera.x;l->snapshot.cy=l->camera.y;l->snapshot.cz=l->camera.z;
}
void LiveCaptureGuard(void* scope,uint8_t* state,const ImageView& frame,float exposure,bool jittered) {
    NoteGraphicsLock();
    __try {LiveCapture(scope,state,frame,exposure,jittered);}
    __except(TargetFault(GetExceptionInformation())) {g_liveView.armed=false;g_liveView.snapshot.valid=false;g_liveView2.armed=false;g_liveView2.snapshot.valid=false;}
}
// A FarShot frame of the second camera stored as the far end's parallax picture: the departure
// snapshot's storing (SnapshotCapture) with the second camera's frame, depth and matrices.
int g_farShotWhy{};
bool FarShotCapture(void* scope,uint8_t* state,const ImageView& frame,float exposure) {
    auto& c=*g_composite;
    if(!c.capturePipeline||!c.snapshotTicket||!g_world||!g_snapshotScratch->image.context){g_farShotWhy=1;return false;}
    if(g_snapshotRequest.active){g_farShotWhy=2;return false;} // a departure snapshot uses the scratch target now
    const PortalEndpoint end=g_farShot.end;
    const float distance=kFarShotDistance;
    const auto info=InspectImage(frame);
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
    const auto depthInfo=InspectImage(depth);
    if(!info.valid||!depthInfo.valid||info.width!=depthInfo.width||info.height!=depthInfo.height||
       !(exposure>0)||!std::isfinite(exposure)){g_farShotWhy=3;return false;}
    const math::Vec3 origin{end.wx+end.nx*distance,end.wy+1.62f,end.wz+end.nz*distance};
    int slot=-1;
    for(int i=0;i<kSnapshots&&slot<0;++i){
        const auto& s=c.snapshots[i];
        const float dx=s.x-end.wx,dz=s.z-end.wz;
        if(s.valid&&dx*dx+dz*dz<0.5f*0.5f&&s.nx*end.nx+s.nz*end.nz>0.95f)slot=i; // this end's own
    }
    if(slot<0){
        slot=0;
        for(int i=0;i<kSnapshots;++i){
            if(!c.snapshots[i].valid){slot=i;break;}
            if(c.snapshots[i].tick<c.snapshots[slot].tick)slot=i;
        }
    }
    auto& snapshot=c.snapshots[slot];
    if(!snapshot.image.context){g_farShotWhy=4;return false;}
    const ImageView& drawTarget=g_snapshotScratch->image;
    g_liveView.snapshot.valid=false; // its frame shares the scratch target
    const float* view=&At<float>(state,0x1000);
    const float* projection=&At<float>(state,0x10c0);
    const math::Vec3 right{end.rx,0,end.rz},up{0,1,0},normal{end.nx,0,end.nz};
    float local[16]{};
    if(!math::PortalLocalFromScreen(view,projection,origin,right,up,normal,local)){g_farShotWhy=5;return false;}
    const float cropX=std::min(1.f,2.75f*std::fabs(projection[0])),cropY=std::min(1.f,1.43f*std::fabs(projection[5]));
    if(!(cropX>0.05f&&cropY>0.05f)){g_farShotWhy=6;return false;}
    float steady[16];std::memcpy(steady,projection,sizeof(steady)); // as SnapshotCapture: the frame's own projection
    double clip[12]{};
    for(int col=0;col<4;++col){
        double v[4]{};
        for(int k=0;k<4;++k)v[k]=view[col*4+k];
        double out[4]{};for(int row=0;row<4;++row)for(int k=0;k<4;++k)out[row]+=double(steady[k*4+row])*v[k];
        clip[col]=out[0]/cropX;clip[4+col]=out[1]/cropY;clip[8+col]=out[3];
    }
    for(double value:clip)if(!std::isfinite(value)){g_farShotWhy=7;return false;}
    Snapshot taken{};
    std::memcpy(taken.clip,clip,sizeof(clip));
    taken.x=end.wx;taken.y=end.wy;taken.z=end.wz;taken.nx=end.nx;taken.nz=end.nz;
    float rows[12]{};
    {
        PortalEndpoint at=end;
        at.wx+=end.nx*(distance-kWindowStart);at.wz+=end.nz*(distance-kWindowStart);
        SnapshotRows(taken,at,rows);
    }
    float vertices[3*kVertexFloats]{};
    const float xy[3][2]={{-1,-1},{-1,3},{3,-1}};
    for(int i=0;i<3;++i){
        float* v=vertices+i*kVertexFloats;v[0]=xy[i][0];v[1]=xy[i][1];
        std::memcpy(v+8,local,sizeof(local));std::memcpy(v+40,rows+8,4*sizeof(float));v[44]=cropX;v[45]=cropY;
    }
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&frame,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    const uint8_t attachment[]={0,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&drawTarget,attachment,"town portal far snapshot");
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    alignas(8) uint8_t bindings[3][0x28]{};
    for(int i=0;i<2;++i){bindings[i][0]=3;At<ImageView>(bindings[i],8)=i?depth:frame;bindings[i][0x18]=7;}
    bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
    alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
    auto* graph=At<uint8_t*>(scope,0);
    void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
    auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=c.capturePipeline;
    command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
    bool drawn=false;
    if(auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3)){
        std::memcpy(mapped,vertices,sizeof(vertices));drawn=true;
    }
    Native<void(*)(void*)>(0x6c960)(scope);
    if(!drawn){g_farShotWhy=8;return false;}
    CopySnapshotImage(scope,drawTarget,snapshot.image);
    g_snapshotShroud[slot]=false;g_snapshotDefault[slot]=false;
    taken.image=snapshot.image;taken.cx=origin.x;taken.cy=origin.y;taken.cz=origin.z;
    taken.exposure=exposure;taken.tick=GetTickCount64();taken.valid=true;
    snapshot=taken;
    ++c.captures;
    char line[200]{};
    std::snprintf(line,sizeof(line),"Town Portal: far snapshot of portal %llu %s end taken from the live camera (slot %d, frame %ux%u, field of view %.0f degrees)",
                  static_cast<unsigned long long>(g_farShot.portalId),g_farShot.field?"base":"field",slot,info.width,info.height,
                  2.f*std::atan(1.f/std::fabs(projection[5]))*57.2958f);
    greeneye::Host()->log(line);
    return true;
}
void FarShotCaptureGuard(void* scope,uint8_t* state,const ImageView& frame,float exposure) {
    NoteGraphicsLock();
    bool stored=false;
    __try {stored=FarShotCapture(scope,state,frame,exposure);}
    __except(TargetFault(GetExceptionInformation())) {}
    FarShotDoneNow(stored);
}
void SnapshotCaptureGuard(void* scope,uint8_t* state,const ImageView& frame,const ImageView& sceneColor,float exposure) {
    NoteGraphicsLock();
    __try {SnapshotCapture(scope,state,frame,sceneColor,exposure);}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;g_snapshotRequest.active=false;}
}
// In-frame drawing: the portal is recorded into the game's own frame, into its HDR scene
// colour at render resolution, just before the upscaler (FSR / DLSS) or the final
// resolve, so the game anti-aliases its edges against characters, hair and grass like any
// other scene content. Evidence: C:\temp\enshrouded-re\out\pre-taa (renderView call
// 0x13d5ab to 0x11a870: rcx renderView's recording scope, rdx state+0xf38, r8 the
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
// The motion pass. The game's anti-aliasing and upscaling (DLSS, FSR, XeSS) carry each pixel's
// history along the motion vectors the frame was drawn with; behind a portal those are the
// wall's, so the view through it left echoes when the camera moved. Each endpoint that shows a
// view writes that view's own motion into the image the upscaler is given (inputs +0x30,
// R16G16_SNORM at render size; the game passes MV.Scale = render size to DLSS, so a value is
// previous minus current position as a fraction of the screen, y down): the point seen through
// the portal is treated as standing behind it. Kept apart from CompositeState (its own keep).
struct MotionState {void* pipeline{};uint32_t shaderHash{},error{};uint64_t frames{};};
MotionState* g_motion{};
void MotionPipeline() {
    auto& c=*g_composite;
    if(!g_motion||g_motion->error||!c.enabled||!c.pipeline||!c.layout||!c.vertices||!c.graphics)return;
    if(g_motion->pipeline&&g_motion->shaderHash==ShaderHash())return;
    CompositeReleaseObject(g_motion->pipeline);
    void* device=At<void*>(c.graphics,0x278);
    // As CompositePipelines' first pipeline (no blending), writing R16G16_SNORM (VkFormat 78).
    alignas(8) uint8_t desc[0x118]{};
    At<const void*>(desc,8)=kComposite_vert;At<uint64_t>(desc,0x10)=sizeof(kComposite_vert);
    At<const void*>(desc,0x48)=kComposite_frag;At<uint64_t>(desc,0x50)=sizeof(kComposite_frag);
    At<void*>(desc,0x58)=c.layout;At<void*>(desc,0x60)=c.vertices;
    At<uint32_t>(desc,0x68)=0x01000001;desc[0x6d]=0x1f;
    for(int i=0;i<7;++i)desc[0x79+i*8]=0xf;
    desc[0x78]=78;
    At<uint32_t>(desc,0xb0)=0xff00;At<uint32_t>(desc,0xb4)=7;
    At<uint32_t>(desc,0xb8)=0x070000ff;At<uint32_t>(desc,0xbc)=0x01000000;
    At<uint32_t>(desc,0xc0)=0x00070000;
    const char* names[]={"main","tcs_main","tes_main","ms_main","main"};
    for(int i=0;i<5;++i){At<const char*>(desc,0xc8+i*16)=names[i];At<uint64_t>(desc,0xd0+i*16)=std::strlen(names[i]);}
    g_motion->pipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    g_motion->shaderHash=ShaderHash();
    if(!g_motion->pipeline)g_motion->error=1;
    greeneye::Host()->log(g_motion->pipeline?"Town Portal: motion pass ready (the view through a portal moves with the game's anti-aliasing)"
                                            :"Town Portal: motion pass unavailable");
}
// The depth pass. The portal is drawn into the scene colour just before the game's upscaler,
// but something after that still paints by the scene's depth: with the lens drawn black, the
// night sky behind the portal showed in it wherever nothing solid stood behind, with a hard
// edge along a wall there (2026-11-10). So each endpoint that shows a view also writes its
// lens into the scene depth (reverse Z, greater-or-equal test, depth write, no colour).
// Pipeline description: +0xb0 the depth format, +0xc2 the compare (6 greater or equal),
// +0xc3 depth write; no colour formats. The pass begins with the engine's depth-only begin
// (FUN_140164610: scope, depth view, load/store/clear bytes, name). tools/depth.off (dev)
// leaves the pass out.
struct DepthPassState {void* pipeline{};uint32_t shaderHash{},error{},format{},madeFor{};uint64_t frames{};};
DepthPassState* g_depthPass{};
using DepthBeginFn=void(*)(void* scope,const ImageView* depth,const uint8_t* ops,const char* name);
DepthBeginFn g_depthBegin{};
void DepthPipeline() {
    auto& c=*g_composite;
    auto* d=g_depthPass;
    if(!d||d->error||!g_depthBegin||!d->format||!c.enabled||!c.pipeline||!c.layout||!c.vertices||!c.graphics)return;
    if(d->pipeline&&d->shaderHash==ShaderHash()&&d->madeFor==d->format)return;
    CompositeReleaseObject(d->pipeline);
    void* device=At<void*>(c.graphics,0x278);
    alignas(8) uint8_t desc[0x118]{};
    At<const void*>(desc,8)=kComposite_vert;At<uint64_t>(desc,0x10)=sizeof(kComposite_vert);
    At<const void*>(desc,0x48)=kComposite_frag;At<uint64_t>(desc,0x50)=sizeof(kComposite_frag);
    At<void*>(desc,0x58)=c.layout;At<void*>(desc,0x60)=c.vertices;
    At<uint32_t>(desc,0x68)=0x01000001;desc[0x6d]=0x1f;
    At<uint32_t>(desc,0xb0)=0xff00u|(d->format&0xffu);At<uint32_t>(desc,0xb4)=7;
    At<uint32_t>(desc,0xb8)=0x070000ff;At<uint32_t>(desc,0xbc)=0x01000000;
    At<uint32_t>(desc,0xc0)=0x01060000;
    const char* names[]={"main","tcs_main","tes_main","ms_main","main"};
    for(int i=0;i<5;++i){At<const char*>(desc,0xc8+i*16)=names[i];At<uint64_t>(desc,0xd0+i*16)=std::strlen(names[i]);}
    d->pipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
    d->shaderHash=ShaderHash();d->madeFor=d->format;
    if(!d->pipeline)d->error=1;
    char line[160]{};
    std::snprintf(line,sizeof(line),d->pipeline?"Town Portal: depth pass ready (scene depth format %u); the portal hides what the game draws behind it":"Town Portal: depth pass unavailable (scene depth format %u)",d->format);
    greeneye::Host()->log(line);
}
void MotionPipelineGuard() {
    NoteGraphicsLock();
    __try {MotionPipeline();DepthPipeline();}
    __except(TargetFault(GetExceptionInformation())) {if(g_motion)g_motion->error=2;}
}
void MotionRecord(void* scope,uint8_t* state,uint8_t* inputs,const ImageView& depth,uint64_t now) {
    static float prevView[16]{},prevProjection[16]{};
    static uint64_t prevTick=0;
    const float* view=&At<float>(state,0x1000);
    const float* projection=&At<float>(state,0x10c0);
    auto& c=*g_composite;
    const bool havePrev=prevTick&&now-prevTick<200;
    const ImageView motion=At<ImageView>(inputs,0x30);
    const auto info=InspectImage(motion),depthInfo=InspectImage(depth);
    if(havePrev&&g_motion&&g_motion->pipeline&&!g_motion->error&&g_motionCount&&std::fabs(projection[11])>1e-6f&&
       info.valid&&info.format==78&&(info.usage&8)&&info.width==depthInfo.width&&info.height==depthInfo.height){
        // World to the previous frame's clip x, y, w, without its jitter.
        float before[16];std::memcpy(before,prevProjection,sizeof(before));before[8]=before[9]=0;
        double clip[12]{};
        for(int col=0;col<4;++col){
            double v[4]{};
            for(int k=0;k<4;++k)v[k]=prevView[col*4+k];
            double out[4]{};for(int row=0;row<4;++row)for(int k=0;k<4;++k)out[row]+=double(before[k*4+row])*v[k];
            clip[col]=out[0];clip[4+col]=out[1];clip[8+col]=out[3];
        }
        const float jitter[2]={projection[8]/projection[11],projection[9]/projection[11]};
        float scale=1.f;
#ifdef TOWN_PORTAL_DEV
        // Dev: C:\Mods\Enshrouded\TownPortal\tools\motion.tune holds a scale for the motion
        // written (1 as computed, -1 reversed, 0 none; "off" skips the pass), read once a second.
        {
            static uint64_t readAt=0;static float tuned=1.f;static bool off=false,cycle=false,cycle2=false,noaim=false;
            if(now-readAt>1000){
                readAt=now;tuned=1.f;off=false;cycle=false;cycle2=false;noaim=false;
                if(FILE* f=_wfopen(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\motion.tune",L"r")){
                    char text[32]{};
                    if(std::fgets(text,sizeof(text),f)){
                        if(!std::strncmp(text,"off",3))off=true;
                        else if(!std::strncmp(text,"noaim",5))noaim=true;
                        else if(!std::strncmp(text,"cycle2",6))cycle2=true;
                        else if(!std::strncmp(text,"cycle",5))cycle=true;
                        else tuned=float(std::atof(text));
                    }
                    std::fclose(f);
                }
            }
            // "cycle": every 6 s the next of A (motion written, view follows the eye), B (no
            // motion written), C (view does not follow the eye), D (neither); main.cpp shows the letter.
            if(cycle){
                const int mode=int((now/6000)%4);
                g_liveTestMode=mode;off=mode==1||mode==3;g_liveParallax=mode>=2?0.f:1.f;
            } else if(cycle2){
                // A as it is; B the frame before the second camera's anti-aliasing; C the camera
                // not aimed and zoomed; D a frame every game frame.
                const int mode=int((now/6000)%4);
                g_liveTestMode=mode;g_liveParallax=1.f;
                g_liveRaw=mode==1;g_liveNoAim=mode==2;g_liveEveryMs=mode==3?33u:0u;
            } else {g_liveTestMode=-1;g_liveParallax=1.f;g_liveRaw=false;g_liveNoAim=noaim;g_liveEveryMs=0;}
            scale=tuned;
            if(off){std::memcpy(prevView,view,sizeof(prevView));std::memcpy(prevProjection,projection,sizeof(prevProjection));prevTick=now;return;}
        }
#endif
        const uint8_t attachment[]={1,1,0};
        Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&motion,attachment,"town portal motion");
        Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
        auto* graph=At<uint8_t*>(scope,0);
        for(uint32_t i=0;i<g_motionCount;++i){
            const auto& m=g_motionSlots[i];
            // Portal metres (right, up, normal from the endpoint's centre) to that clip space.
            const double basis[4][4]={{m.end.rx,0,m.end.rz,0},{0,1,0,0},{m.end.nx,0,m.end.nz,0},{m.end.wx,m.end.wy+1.62f,m.end.wz,1}};
            float rows[12]{};
            for(int row=0;row<3;++row)for(int col=0;col<4;++col){
                double v=0;for(int k=0;k<4;++k)v+=clip[row*4+k]*basis[col][k];
                rows[row*4+col]=float(v);
            }
            alignas(8) uint8_t bindings[3][0x28]{};
            for(int b=0;b<2;++b){bindings[b][0]=3;At<ImageView>(bindings[b],8)=b?depth:m.image;bindings[b][0x18]=7;}
            bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
            alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
            void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
            auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=g_motion->pipeline;
            command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
            auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3);
            if(!mapped)continue;
            std::memcpy(mapped,m.vertices,sizeof(m.vertices));
            for(int v=0;v<3;++v){
                float* f=mapped+v*kVertexFloats;
                f[2]=jitter[0];f[3]=jitter[1];f[4]=scale;f[5]=0;
                f[6]+=1000.f;
                std::memcpy(f+24,rows,4*sizeof(float));std::memcpy(f+28,rows+4,4*sizeof(float));std::memcpy(f+48,rows+8,4*sizeof(float));
            }
        }
        Native<void(*)(void*)>(0x6c960)(scope);
        ++g_motion->frames;
    }
    std::memcpy(prevView,view,sizeof(prevView));std::memcpy(prevProjection,projection,sizeof(prevProjection));
    prevTick=now;
}
void DepthRecord(void* scope,const ImageView& depth,uint64_t now) {
    auto& c=*g_composite;
    auto* d=g_depthPass;
    if(!d||d->error||!g_depthBegin||!(g_motionCount+g_swirlCount))return;
    static uint64_t checked=0;static bool off=false;
    if(now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/depth.off")!=INVALID_FILE_ATTRIBUTES;}
    if(off)return;
    const auto info=InspectImage(depth);
    if(!info.valid||info.format<0x7c||info.format>0x82)return;
    if(d->format!=info.format){d->format=info.format;return;} // its pipeline is made before the next frame
    if(!d->pipeline||d->madeFor!=info.format)return;
    // The scene depth is this pass's target, so each endpoint's picture stands in for it among
    // the sampled images (the shader does not read it in this pass).
    const uint8_t ops[]={1,1,0};
    g_depthBegin(scope,&depth,ops,"town portal depth");
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    auto* graph=At<uint8_t*>(scope,0);
    for(uint32_t i=0;i<g_motionCount+g_swirlCount;++i){
        const bool swirl=i>=g_motionCount;
        const float* vertices=swirl?g_swirlVertices[i-g_motionCount]:g_motionSlots[i].vertices;
        const ImageView image=swirl?g_target->color:g_motionSlots[i].image;
        alignas(8) uint8_t bindings[3][0x28]{};
        for(int b=0;b<2;++b){bindings[b][0]=3;At<ImageView>(bindings[b],8)=image;bindings[b][0x18]=7;}
        bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
        alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
        void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
        auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=d->pipeline;
        command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
        auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3);
        if(!mapped)continue;
        std::memcpy(mapped,vertices,3*kVertexFloats*sizeof(float));
        for(int v=0;v<3;++v)mapped[v*kVertexFloats+6]+=2000.f;
    }
    Native<void(*)(void*)>(0x6c960)(scope);
    ++d->frames;
}
// early: the colour alone, drawn before the game picks up the frame's bright pixels for its
// glow (the call at exe+0x13d43a, EarlyColourHook). Drawn only at the upscaler, a flame behind
// a portal had already been picked up and its glow was laid over the portal afterwards: the
// portal looked see-through (2026-11-10). The motion and depth passes stay at the upscaler,
// which is where their images are known; g_earlyColour says the colour is already in.
bool g_earlyColour{};
std::atomic<float> g_frameExposure{0.f};
uint32_t g_lateDepthFrames{};
bool InFrameDraw(void* scope,uint8_t* state,uint8_t* inputs,bool lightOnly=false,const ImageView* early=nullptr) {
    auto& c=*g_composite;
    if(!c.enabled||!c.framePipeline||c.error||!g_target||g_target->error||!g_world)return false;
    if(state!=WorldState())return false;
    const auto now=GetTickCount64();
    if(!InFrameEnabled(now))return false;
    CompositeSlot slots[kCompositeSlots]{};
    const uint32_t count=CompositeGather(slots,now);
    const bool colourDone=!early&&!lightOnly&&g_earlyColour;
    if(!early)g_earlyColour=false;
    if(!count)return false;
    // The scene colour the upscaler reads (state+0x140). Depth comes from the selector the
    // engine's transparent and lighting passes use: the state holds two depth images that
    // swap every frame, and the upscaler's own depth entry is not always the current one.
    const ImageView color=early?*early:At<ImageView>(inputs,0x10);
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
    const auto target=InspectImage(color),depthInfo=InspectImage(depth);
    if(!target.valid||target.format!=0x61||!depthInfo.valid||
       target.width!=depthInfo.width||target.height!=depthInfo.height){++c.inFrameSkip;return false;}
    if(colourDone){
        MotionRecord(scope,state,inputs,depth,now);
        DepthRecord(scope,depth,now);
        ++c.inFrameFrames;c.inFrameTick=now;c.tick=now;++c.frames;
        c.portal=slots[0].portal.portalId;c.field=slots[0].field;
        return true;
    }
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_target->color,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    for(const auto& snapshot:c.snapshots)
        if(snapshot.valid)Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&snapshot.image,7,&access);
    if(g_sharedGpu)for(const auto& v:g_sharedGpu->views)
        if(v.snapshot.valid&&v.epoch==sharing::Epoch())
            Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&v.snapshot.image,7,&access);
    if(g_gagGpu&&g_gagGpu->ticket&&!g_gagGpu->error&&g_gagGpu->place>=0)
        Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_gagGpu->image,7,&access);
    if(g_liveView.snapshot.valid)
        Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_liveView.snapshot.image,7,&access);
    if(g_liveView2.snapshot.valid)
        Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_liveView2.snapshot.image,7,&access);
    // Load and store: blend over what the scene drew.
    const uint8_t attachment[]={1,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&color,attachment,"town portal");
    // Snapshots are scaled from their exposure to this frame's (inputs +9c: 1/exposure).
    // Early there are no upscaler inputs yet: the exposure of the frame before.
    float invExposure=0;
    if(early){const float e=g_frameExposure.load();invExposure=e>0?1.f/e:0.f;}
    else invExposure=At<float>(inputs,0x9c);
    if(lightOnly){CompositeRecord(scope,state,depth,true,slots,count,now,0.f,true);return true;}
    CompositeRecord(scope,state,depth,true,slots,count,now,std::isfinite(invExposure)&&invExposure>0?invExposure:0.f);
    if(early){
        // Its depth at once too: the game's effects drawn after this are then hidden by the
        // portal when behind it and drawn over it when in front (VfxPassHook).
        DepthRecord(scope,depth,now);
        g_earlyColour=true;return true;
    }
    MotionRecord(scope,state,inputs,depth,now);
    DepthRecord(scope,depth,now);
    ++g_lateDepthFrames; // this frame's portal was not drawn before the effects
    ++c.inFrameFrames;c.inFrameTick=now;c.tick=now;++c.frames;
    if(g_liveView.snapshot.valid&&g_liveView.ready&&!lightOnly){
        const uint64_t lag=g_frameNo-g_liveFrameNo;
        g_lagMin=std::min(g_lagMin,lag);g_lagMax=std::max(g_lagMax,lag);
    }
    c.portal=slots[0].portal.portalId;c.field=slots[0].field;
    return true;
}
// A portal seen through a portal. The second camera's frame gets the portal drawn into it
// too: the end this frame is for, where it stands in the far scene, with its own last live
// picture. Two ends that see each other then show each other, one inside the next, a frame
// deeper each time (2026-11-10, asked for by the user). Drawn into the far view's scene colour
// before the frame is stored. tools/live.noloop (dev) leaves it out.
bool OwnedPortalDraw(void* scope,uint8_t* state,uint8_t* inputs) {
    auto& c=*g_composite;
    if(!c.enabled||!c.framePipeline||c.error||!g_target||g_target->error||!g_world||!g_liveNow)return false;
    static uint64_t checked=0;static bool off=false;
    const auto now=GetTickCount64();
    if(now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.noloop")!=INVALID_FILE_ATTRIBUTES;}
    if(off||!InFrameEnabled(now))return false;
    // Not while two ends take turns: the inner portal is placed with the view state's matrices,
    // which are then the other end's, and it came out as spirals and bands (2026-11-10).
    // While two ends take turns the view state's matrices are the other end's (LiveCapture), so
    // the inner portal is placed with this end's own, put into the state for the drawing and
    // taken out again. (Placed with the state's as they were it came out as spirals and bands,
    // 2026-11-10.) tools/live.noloop2 (dev) leaves the inner portal out while taking turns.
    const auto& l=*g_liveNow;
    const bool own=g_liveAlternating;
    if(own&&(!l.haveMatrices||GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.noloop2")!=INVALID_FILE_ATTRIBUTES))return false;
    struct Swap {
        uint8_t* state;bool on;float view[16],projection[16];
        Swap(uint8_t* s,bool o,const LiveView& l):state(s),on(o){
            if(!on)return;
            std::memcpy(view,state+0x1000,sizeof(view));std::memcpy(projection,state+0x10c0,sizeof(projection));
            std::memcpy(state+0x1000,l.view,sizeof(view));std::memcpy(state+0x10c0,l.projection,sizeof(projection));
        }
        ~Swap(){if(on){std::memcpy(state+0x1000,view,sizeof(view));std::memcpy(state+0x10c0,projection,sizeof(projection));}}
    } swap(state,own,l);
    if(!l.ready||!l.snapshot.valid||!l.armedPortal)return false;
    CompositeSlot slots[kCompositeSlots]{};uint32_t count=0;
    for(const PortalInstance* portal:{&g_livePortal,&g_livePortal2})
        if(!count&&portal->active&&portal->portalId==l.armedPortal){
            slots[0].portal=*portal;slots[0].field=l.armedField;
            for(uint32_t i=0;i<g_compositeSlotCount;++i)
                if(g_compositeSlots[i].portal.portalId==portal->portalId&&g_compositeSlots[i].field==l.armedField)slots[0].tint=g_compositeSlots[i].tint;
            count=1;
        }
    if(!count)return false;
    const ImageView color=g_givenColor?*g_givenColor:At<ImageView>(inputs,0x10);
    ImageView depth{};
    if(g_givenDepth)depth=*g_givenDepth;
    else Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
    const auto target=InspectImage(color),depthInfo=InspectImage(depth);
    if(!target.valid||target.format!=0x61||!depthInfo.valid||target.width!=depthInfo.width||target.height!=depthInfo.height)return false;
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_target->color,7,&access);
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    for(const auto& snapshot:c.snapshots)
        if(snapshot.valid)Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&snapshot.image,7,&access);
    if(g_liveView.snapshot.valid)
        Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_liveView.snapshot.image,7,&access);
    if(g_liveView2.snapshot.valid)
        Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_liveView2.snapshot.image,7,&access);
    const uint8_t attachment[]={1,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&color,attachment,"town portal in portal");
    const float invExposure=g_givenColor?g_givenInvExposure:At<float>(inputs,0x9c);
    const uint32_t motionKept=g_motionCount;
    g_innerDraw=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/inner.mist")==INVALID_FILE_ATTRIBUTES;
    CompositeRecord(scope,state,depth,true,slots,count,now,std::isfinite(invExposure)&&invExposure>0?invExposure:0.f);
    g_innerDraw=false;
    // Its depth too, so the stored frame holds the inner portal at its own distance: the next
    // level is found through it (without this the loop stopped one level deep, 2026-11-10).
    // Only when the far view's depth is of the kind the depth pass was made for.
    if(g_depthPass&&g_depthPass->pipeline&&g_depthPass->format==depthInfo.format&&g_depthPass->madeFor==depthInfo.format
       &&GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.nodepth")==INVALID_FILE_ATTRIBUTES)
        DepthRecord(scope,depth,now);
    {static uint64_t toldAt=0;if(now-toldAt>10000){toldAt=now;char line[200]{};
        std::snprintf(line,sizeof(line),"Town Portal: portal in portal: far depth format %u (%ux%u), depth pass made for %u (pipeline %d), drawn slots %u, frames %llu",
            depthInfo.format,depthInfo.width,depthInfo.height,g_depthPass?g_depthPass->madeFor:0u,int(g_depthPass&&g_depthPass->pipeline),g_motionCount,g_depthPass?g_depthPass->frames:0ull);
        greeneye::Host()->log(line);}}
    g_motionCount=motionKept;
    static bool told=false;
    if(!told){told=true;greeneye::Host()->log("Town Portal: live view: the far view shows the portal too (a portal seen through a portal)");}
    return true;
}
void OwnedPortalDrawGuard(void* scope,uint8_t* state,uint8_t* inputs) {
    NoteGraphicsLock();
    __try {OwnedPortalDraw(scope,state,inputs);}
    __except(TargetFault(GetExceptionInformation())) {}
}
// The portal inside the far view is drawn before the far view's own effects, like the portal
// in the player's view (VfxPassCall): drawn at the far view's upscaler it covered a flame
// standing in front of it there (the altar's flame was swallowed by the infinity view behind
// it: user, 2026-11-10). g_farInvExposure: the far view's 1/exposure of the frame before.
float g_farInvExposure{};bool g_farPortalDrawn{};
void FarPortalEarly(void* scope,uint8_t* state) {
    if(g_farPortalDrawn||!(g_farInvExposure>0)||!std::isfinite(g_farInvExposure))return;
    static uint64_t checked=0;static bool off=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/farportal.late")!=INVALID_FILE_ATTRIBUTES;}
    if(off)return;
    std::lock_guard lock(g_lock);
    if(!g_target||!g_target->view||g_target->error||!g_liveNow||!g_liveNow->armed)return;
    const ImageView colour=At<ImageView>(state,0x140);
    g_givenColor=&colour;g_givenInvExposure=g_farInvExposure;
    OwnedPortalDrawGuard(scope,state,nullptr);
    g_givenColor=nullptr;
    g_farPortalDrawn=true;
}
void RunPendingFar(void* scope) {
    PendingFar pending=g_pendingFar;
    g_pendingFar.has=false;
    if(!pending.has||!g_target||!g_target->view||g_target->error||At<uint8_t*>(g_target->view,0xa0)!=pending.state||!pending.view)return;
    g_liveNow=pending.view;
    g_givenDepth=&pending.depth;g_givenColor=&pending.color;g_givenInvExposure=pending.invExposure;
    OwnedPortalDrawGuard(scope,pending.state,nullptr);
    g_givenColor=nullptr;
    LiveCaptureGuard(scope,pending.state,pending.color,pending.exposure,pending.jittered);
    g_givenDepth=nullptr;
}
using GlowPreFnEarly=void*(*)(void*,void*,void*);
// Earlier still: before the game draws its effects (flames, sparks, spell effects: the forward
// transparent pass, FUN_140129590(scope, view state, description), three calls in the frame
// function). Drawn before the glow pre-pass only, the portal covered an effect standing IN
// FRONT of it: the flame of the player's own wand vanished where the portal was behind it
// (user, 2026-11-10). With the portal's colour and depth in before the effects, those behind it
// are hidden by its depth and those in front are drawn over it. The first of the three calls
// in a frame draws it. tools/vfxpass.off (dev) leaves it to the glow hook.
using VfxPassFn=void*(*)(void* scope,uint8_t* state,void* description);
VfxPassFn g_vfxPass[3]{};
bool InFrameDrawGuard(void* scope,uint8_t* state,uint8_t* inputs,bool lightOnly,const ImageView* early);
bool g_snapshotHoldingEarly();
void VfxPassEarly(void* scope,uint8_t* state,bool playersOwn=false) {
    greeneye::Lease lease;
    if(!lease||(!state&&!playersOwn)||!g_composite||!g_target||!g_target->ticket||!g_world)return;
    static uint64_t checked=0;static bool off=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/vfxpass.off")!=INVALID_FILE_ATTRIBUTES;}
    if(off)return;
    std::lock_guard lock(g_lock);
    if(playersOwn)state=WorldState();
    if(!state)return;
    if(state==WorldState())RefreshLivePlacement();
    if(g_earlyColour||g_snapshotHoldingEarly()||state!=WorldState()||!(g_frameExposure.load()>0))return;
    const ImageView colour=At<ImageView>(state,0x140);
    static bool told=false;
    if(InFrameDrawGuard(scope,state,nullptr,false,&colour)&&!told){told=true;greeneye::Host()->log("Town Portal: portals are drawn before the game's effects (an effect in front of a portal shows over it)");}
}
// The second camera's effects are drawn WITHOUT fog while its scene keeps fog, sky and haze:
// through the far view's fog every flame, spark and smoke puff came out as nothing (the fog
// terms the effect shader multiplies by read as zero there; cause not found). For this pass
// the view's fog pointer (view state +0xb40) is taken away, which makes the game pick its own
// no-fog effect pipelines, as for a view without fog (FUN_1401bcdb0 checks the pointer), and
// put back after. tools/fxfog.on (dev) leaves the effects fogged.
// Earlier again, before the game shades its water: the water's picture of what lies behind
// and under it is made from the scene colour (FUN_1401ea380, after the call taken here), so a
// portal drawn later was missing through the water: seen from above, its part below the
// water line; from under water, its part above (user, 2026-11-10). The call taken is the last
// one before the game's water test in the frame function (exe+0x13b08c: scope, a renderer
// object, a description), after the scene's own fog. It is not given the view, so it draws only
// in the player's own render (not while the second camera renders). tools/prewater.off (dev).
using PreWaterFn=void*(*)(void* scope,void* object,void* description);
PreWaterFn g_preWater{};
void* PreWaterHook(void* scope,void* object,void* description) {
    static uint64_t checked=0;static bool off=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/prewater.off")!=INVALID_FILE_ATTRIBUTES;}
    if(!off&&g_ownedAspectNow<=0&&g_inPlayerRender)VfxPassEarly(scope,nullptr,true);
    return g_preWater(scope,object,description);
}
// And the portal's depth before the game draws its water at all (FUN_1401f0ac0: scope, the
// water object, a description; exe+0x137ad6): the water keeps a copy of the scene's depth from
// here, and water BEHIND a portal was laid over it without the portal in that copy (a strip of
// the far lake across the portal, 2026-11-10). Depth only, where the portal stood in the frame
// before (the colour follows at PreWaterHook). tools/waterdepth.off (dev).
// (Written into the SCENE depth at this point it wrecked the lighting, which comes later and
// lit the scenery behind as if it stood at the portal: a wide blotch, 2026-11-10. So it goes
// into the water's own copy, water +8, after the call has made it.)
// The copy is a colour image (R32F, a third of the frame's size): a pipeline of its own writes
// the lens's depth into it as a colour (shader mode +3000), where the scene's depth has nothing
// nearer.
void* g_waterDepthPipeline{};uint32_t g_waterDepthHash{};
void WaterDepthRecord(void* scope,uint8_t* state,uint8_t* water) {
    auto& c=*g_composite;
    if(!c.enabled||!c.pipeline||!c.layout||!c.vertices||!c.graphics||c.error||!g_motionCount)return;
    const ImageView copy=At<ImageView>(water,8);
    const auto info=InspectImage(copy);
    if(!info.valid||info.format!=100||!(info.usage&8))return;
    if(!g_waterDepthPipeline||g_waterDepthHash!=ShaderHash()){
        CompositeReleaseObject(g_waterDepthPipeline);
        void* device=At<void*>(c.graphics,0x278);
        alignas(8) uint8_t desc[0x118]{};
        At<const void*>(desc,8)=kComposite_vert;At<uint64_t>(desc,0x10)=sizeof(kComposite_vert);
        At<const void*>(desc,0x48)=kComposite_frag;At<uint64_t>(desc,0x50)=sizeof(kComposite_frag);
        At<void*>(desc,0x58)=c.layout;At<void*>(desc,0x60)=c.vertices;
        At<uint32_t>(desc,0x68)=0x01000001;desc[0x6d]=0x1f;
        for(int i=0;i<7;++i)desc[0x79+i*8]=0xf;
        desc[0x78]=100;
        At<uint32_t>(desc,0xb0)=0xff00;At<uint32_t>(desc,0xb4)=7;
        At<uint32_t>(desc,0xb8)=0x070000ff;At<uint32_t>(desc,0xbc)=0x01000000;
        At<uint32_t>(desc,0xc0)=0x00070000;
        const char* names[]={"main","tcs_main","tes_main","ms_main","main"};
        for(int i=0;i<5;++i){At<const char*>(desc,0xc8+i*16)=names[i];At<uint64_t>(desc,0xd0+i*16)=std::strlen(names[i]);}
        g_waterDepthPipeline=DeviceMethod<void*(*)(void*,const void*)>(device,0x40)(device,desc);
        g_waterDepthHash=ShaderHash();
        greeneye::Host()->log(g_waterDepthPipeline?"Town Portal: water depth pass ready (water behind a portal stays behind it)":"Town Portal: water depth pass unavailable");
    }
    if(!g_waterDepthPipeline)return;
    ImageView depth{};
    Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&depth,state);
    uint64_t access=0x400;
    Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&depth,7,&access);
    for(uint32_t i=0;i<g_motionCount;++i)Native<void(*)(void*,const void*,uint8_t,const uint64_t*)>(0x4cf00)(scope,&g_motionSlots[i].image,7,&access);
    const uint8_t attachment[]={1,1,0};
    Native<void(*)(void*,const void*,const void*,const char*)>(0x6c340)(scope,&copy,attachment,"town portal water depth");
    Native<void(*)(void*,uint64_t)>(0x47a70)(scope,0x7ffffffff);
    auto* graph=At<uint8_t*>(scope,0);
    for(uint32_t i=0;i<g_motionCount;++i){
        const auto& m=g_motionSlots[i];
        alignas(8) uint8_t bindings[3][0x28]{};
        for(int b=0;b<2;++b){bindings[b][0]=3;At<ImageView>(bindings[b],8)=b?depth:m.image;bindings[b][0x18]=7;}
        bindings[2][0]=1;At<void*>(bindings[2],8)=At<void*>(reinterpret_cast<void*>(g_world->root),0x16e28);
        alignas(8) uint8_t set[0x20]{};set[0]=3;At<void*>(set,8)=c.bindings;At<void*>(set,0x10)=bindings;At<uint64_t>(set,0x18)=3;
        void* descriptor=Native<void*(*)(void*,void*,const void*)>(0x102a630)(At<void*>(graph,0),At<void*>(graph,0x10),set);
        auto* command=CompositeCommand(scope,5,0x10);At<void*>(command,8)=g_waterDepthPipeline;
        command=CompositeCommand(scope,7,0x38);At<void*>(command,8)=c.layout;At<void*>(command,0x10)=descriptor;At<uint16_t>(command,0x30)=1;
        auto* mapped=Native<float*(*)(void*,void*,int)>(0x148a50)(scope,c.vertices,3);
        if(!mapped)continue;
        std::memcpy(mapped,m.vertices,sizeof(m.vertices));
        for(int v=0;v<3;++v)mapped[v*kVertexFloats+6]+=3000.f;
    }
    Native<void(*)(void*)>(0x6c960)(scope);
}
void WaterDepthGuard(void* scope,uint8_t* water) {
    NoteGraphicsLock();
    __try {if(auto* state=WorldState())WaterDepthRecord(scope,state,water);}
    __except(TargetFault(GetExceptionInformation())) {}
}
PreWaterFn g_waterRaster{};
void* WaterRasterHook(void* scope,void* water,void* description) {
    void* result=g_waterRaster(scope,water,description);
    greeneye::Lease lease;
    static uint64_t checked=0;static bool off=true;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/waterdepth.on")==INVALID_FILE_ATTRIBUTES;}
    if(lease&&!off&&water&&g_ownedAspectNow<=0&&g_inPlayerRender&&g_composite&&g_target&&g_target->ticket&&!g_target->error&&g_world){
        std::lock_guard lock(g_lock);
        if(WorldState()&&!g_snapshotHoldingEarly()&&g_motionCount)WaterDepthGuard(scope,static_cast<uint8_t*>(water));
    }
    return result;
}
void** VfxFogSlot(uint8_t* state) {
    greeneye::Lease lease;
    static uint64_t checked=0;static bool keep=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;keep=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fxfog.on")!=INVALID_FILE_ATTRIBUTES;}
    if(lease&&!keep&&state&&g_ownedAspectNow>0&&g_target&&g_target->view&&At<uint8_t*>(g_target->view,0xa0)==state)return &At<void*>(state,0xb40);
    return nullptr;
}
// The portal's depth in the half-size depth (view state +0x220) the game's half-size effects
// (an altar's fire and smoke) are tested against: written just before that pass (the second of
// the three calls), after the game's depth_downsample (FUN_140073020, at the pre-water call)
// has made +0x220 from the full depth, which wiped a write made earlier; the downsample alone
// lost the portal round its outline, and the fire blended over its edge (user, 2026-11-10;
// static RE C:/temp/enshrouded-re/fxdepth-q*-out.txt). tools/halfdepth.off (dev) leaves it.
void HalfDepthBeforeEffects(void* scope,uint8_t* state) {
    if(!g_composite||!g_target||!g_world||state!=WorldState())return;
    static uint64_t checked=0;static bool off=false;
    const uint64_t now=GetTickCount64();
    if(now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/halfdepth.off")!=INVALID_FILE_ATTRIBUTES;}
    if(off||!g_earlyColour)return; // the portal was drawn before the effects this frame
    const ImageView half=At<ImageView>(state,0x220);
    if(!InspectImage(half).valid)return;
    DepthRecord(scope,half,now);
}
void HalfDepthGuard(void* scope,uint8_t* state) {
    NoteGraphicsLock();
    __try {HalfDepthBeforeEffects(scope,state);}
    __except(TargetFault(GetExceptionInformation())) {}
}
void HalfDepthLocked(void* scope,uint8_t* state) {
    greeneye::Lease lease;
    if(!lease)return;
    std::lock_guard lock(g_lock);
    HalfDepthGuard(scope,state);
}
void* VfxPassCall(int which,void* scope,uint8_t* state,void* description) {
    VfxPassEarly(scope,state);
    if(which==1)HalfDepthLocked(scope,state);
    void** fog=VfxFogSlot(state);void* saved=nullptr;
    if(fog)FarPortalEarly(scope,state);   // fog is non-null only for the second camera's own render
    if(fog){saved=*fog;*fog=nullptr;}
    void* result=nullptr;
    __try {result=g_vfxPass[which](scope,state,description);}
    __finally {if(fog)*fog=saved;}
    return result;
}
void* VfxPassHook0(void* scope,uint8_t* state,void* description) {return VfxPassCall(0,scope,state,description);}
void* VfxPassHook1(void* scope,uint8_t* state,void* description) {return VfxPassCall(1,scope,state,description);}
void* VfxPassHook2(void* scope,uint8_t* state,void* description) {return VfxPassCall(2,scope,state,description);}
extern GlowPreFnEarly g_glowPreSeen;
// From the second camera's upscaler: keep the frame for RunPendingFar; without the early hook
// it is stored at once, as before. tools/live.direct (dev) stores at once too.
void DeferFar(void* scope,uint8_t* state,uint8_t* inputs,const ImageView& frameGiven,float exposure,bool jittered) {
    // Dev: tools/live.fx shows the far view's off-screen effect picture (state +0x1b0) in the
    // portal in place of its frame: are the flames in it before the game blends it in?
    static uint64_t fxChecked=0;static unsigned fxOnly=0;
    if(const uint64_t now=GetTickCount64();now-fxChecked>1000){
        fxChecked=now;fxOnly=0;
        if(FILE* f=std::fopen("C:/Mods/Enshrouded/TownPortal/tools/live.fx","r")){if(std::fscanf(f,"%x",&fxOnly)!=1||fxOnly<0x100||fxOnly>0x220)fxOnly=0x1b0;std::fclose(f);}
        static uint64_t told=0;
        if(now-told>10000){told=now;char line[200]{};
            std::snprintf(line,sizeof(line),"Town Portal: far frame given: image id %llx; state +0x140 %llx, +0x180 %llx, +0x1b0 %llx",
                static_cast<unsigned long long>(frameGiven.id),static_cast<unsigned long long>(At<ImageView>(state,0x140).id),static_cast<unsigned long long>(At<ImageView>(state,0x180).id),static_cast<unsigned long long>(At<ImageView>(state,0x1b0).id));
            greeneye::Host()->log(line);}
    }
    const ImageView frame=fxOnly?At<ImageView>(state,fxOnly):frameGiven;
    static uint64_t checked=0;static bool direct=false;
    if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;direct=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/live.deferred")==INVALID_FILE_ATTRIBUTES; /* deferred storing blew the portal out to white (feedback through the portal-in-portal picture, 2026-11-10): off unless asked for */}
    g_farInvExposure=At<float>(inputs,0x9c);
    const bool drawnEarly=g_farPortalDrawn;g_farPortalDrawn=false;
    if(g_farShot.captureNow){ // a FarShot frame: stored as the end's parallax picture, not live
        FarShotCaptureGuard(scope,state,frame,exposure);
        return;
    }
    if(g_farShot.active)return; // the frames before it: neither live nor stored
    if(!g_glowPreSeen||direct){
        if(!drawnEarly)OwnedPortalDrawGuard(scope,state,inputs);
        LiveCaptureGuard(scope,state,frame,exposure,jittered);
        return;
    }
    PendingFar pending{};
    pending.has=true;pending.color=frame;pending.exposure=exposure;pending.invExposure=At<float>(inputs,0x9c);
    pending.jittered=jittered;pending.view=g_liveNow;pending.state=state;
    Native<ImageView*(*)(ImageView*,void*)>(0x125f10)(&pending.depth,state);
    g_liveNow->armed=false;
    g_pendingFar=pending;
}
bool InFrameDrawGuard(void* scope,uint8_t* state,uint8_t* inputs,bool lightOnly=false,const ImageView* early=nullptr) {
    NoteGraphicsLock();
    __try {return InFrameDraw(scope,state,inputs,lightOnly,early);}
    __except(TargetFault(GetExceptionInformation())) {g_composite->error=g_target->error;return false;}
}
// The game's glow pre-pass (FUN_140a5ce40: scope, the view's post object = state +0x1120, a
// description whose first entry is the scene colour, state +0x140). tools/early.off (dev)
// leaves the portals to the upscaler hook as before.
using GlowPreFn=void*(*)(void* scope,void* object,void* description);
GlowPreFn g_glowPre{};
GlowPreFnEarly g_glowPreSeen{};
bool g_snapshotHoldingEarly();
void* EarlyColourHook(void* scope,void* object,void* description) {
    greeneye::Lease lease;
    if(lease&&object&&description&&g_composite&&g_target&&g_target->ticket&&g_world){
        static uint64_t checked=0;static bool off=false;
        if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;off=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/early.off")!=INVALID_FILE_ATTRIBUTES;}
        std::lock_guard lock(g_lock);
        auto* state=WorldState();
        if(state&&At<void*>(state,0x1120)==object)RunPendingFar(scope);
        if(!off&&!g_earlyColour&&!g_snapshotHoldingEarly()&&state&&At<void*>(state,0x1120)==object&&g_frameExposure.load()>0){
            const ImageView colour=At<ImageView>(description,0);
            static bool told=false;
            if(InFrameDrawGuard(scope,state,nullptr,false,&colour)&&!told){told=true;greeneye::Host()->log("Town Portal: portals are drawn before the game's glow (a flame behind a portal no longer shines over it)");}
        }
    }
    return g_glowPre(scope,object,description);
}
#ifdef TOWN_PORTAL_DEV
// Once per reload: what the upscaler is given for the world view (images by offset, and the
// floats after them), to find its motion vectors.
void DumpUpscalerInputs(uint8_t* inputs) {
    __try {
        for(uint32_t offset=0;offset<0xa0;offset+=8){
            const auto a=InspectImage(At<ImageView>(inputs,offset));
            if(!a.valid||a.width<256||a.height<128||!a.format||a.format>200)continue;
            char line[160]{};
            std::snprintf(line,sizeof(line),"Town Portal: upscaler input +0x%x: image %ux%u format %u usage 0x%x id 0x%llx",
                          offset,a.width,a.height,a.format,a.usage,static_cast<unsigned long long>(a.image.id));
            greeneye::Host()->log(line);
        }
        char line[400]{};int n=std::snprintf(line,sizeof(line),"Town Portal: upscaler input floats +0x80:");
        for(uint32_t offset=0x80;offset<0x100&&n<380;offset+=4)n+=std::snprintf(line+n,sizeof(line)-n," %.5g",At<float>(inputs,offset));
        greeneye::Host()->log(line);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
#endif
// Dev: what the upscaler hands back (its result pointer read as an image, and the first
// image-like things at it), for the player's view and for the second camera's, every 20 s.
void DescribeUpscaled(const char* who,void* returned,uint8_t* inputs,uint8_t* upscalers=nullptr) {
    __try {
        char line[300]{};
        const auto in=InspectImage(At<ImageView>(inputs,0x10));
        int n=std::snprintf(line,sizeof(line),"Town Portal: %s upscaler: input %ux%u, result %p:",who,in.width,in.height,returned);
        if(returned)for(uint32_t offset=0;offset<0x40;offset+=8){
            const auto a=InspectImage(At<ImageView>(returned,offset));
            if(a.valid&&a.width>=128&&a.height>=64&&a.format&&a.format<200)
                n+=std::snprintf(line+n,sizeof(line)-n," +0x%x %ux%u format %u id 0x%llx;",offset,a.width,a.height,a.format,static_cast<unsigned long long>(a.image.id));
        }
        greeneye::Host()->log(line);
        // The render function takes the frame after the upscaler from the view state: +0x140, or
        // +0x180 when the upscaler is active (FUN_14011a810(state + 0xf38)); 14013d5cc.
        if(upscalers){
            auto* st=upscalers-0xf38;
            const auto plain=InspectImage(At<ImageView>(st,0x140)),up=InspectImage(At<ImageView>(st,0x180));
            std::snprintf(line,sizeof(line),"Town Portal:   %s view state: +0x140 %ux%u f%u valid %d, +0x180 %ux%u f%u valid %d, upscaler active %d",
                          who,plain.width,plain.height,plain.format,int(plain.valid),up.width,up.height,up.format,int(up.valid),
                          -1);
            greeneye::Host()->log(line);
        }
        // Images held by the upscaler object itself, and by the inputs past the known ones.
        for(int which=0;which<2;++which){
            auto* base=which?inputs:upscalers;
            if(!base)continue;
            n=std::snprintf(line,sizeof(line),"Town Portal:   %s images in its %s:",who,which?"inputs":"upscaler");
            for(uint32_t offset=which?0x70:0;offset<(which?0x200u:0x600u)&&n<260;offset+=8){
                const auto a=InspectImage(At<ImageView>(base,offset));
                if(a.valid&&a.width>=256&&a.height>=128&&a.width<=16384&&a.height<=16384&&a.format&&a.format<200)
                    n+=std::snprintf(line+n,sizeof(line)-n," +0x%x %ux%u f%u;",offset,a.width,a.height,a.format);
            }
            greeneye::Host()->log(line);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool g_snapshotHoldingEarly() {return g_snapshotHolding;}
void* UpscaleHook(void* scope,uint8_t* upscalers,uint8_t* inputs) {
    greeneye::Lease lease;
#ifdef TOWN_PORTAL_DEV
    if(lease&&g_world){
        std::lock_guard lock(g_lock);
        static bool dumped=false;
        if(auto* state=WorldState();!dumped&&state&&upscalers-0xf38==state){dumped=true;DumpUpscalerInputs(inputs);}
    }
#endif
    // The second camera's frame, for the portal's live view: taken before its own anti-aliasing
    // (g_liveRaw), which blends in earlier frames of a camera that is re-aimed every frame; the
    // game's anti-aliasing of the player's view then smooths the portal with everything else.
    // Taken after it at first (the raw frame at 30 a second smeared); with a frame every game
    // frame the user found the raw one better (A/B, 2026-11-09).
    if(lease&&g_target&&g_composite){
        bool owned=false;
        {
            std::lock_guard lock(g_lock);
            owned=(g_liveNow->armed||g_farShot.captureNow)&&g_target->view&&upscalers-0xf38==At<uint8_t*>(g_target->view,0xa0);
        }
        if(owned){
            const ImageView sceneColor=At<ImageView>(inputs,0x10);
            const float exposure=At<float>(inputs,0xa0);
            // With the game's own upscaler on this view (mode 2 of FUN_14011a870: DLSS, which
            // reads the scene colour at inputs +0x10 and writes its anti-aliased result to the
            // image at +0x40), that result is the frame to show: steady and sharp, where the
            // raw frame resampled onto the screen stayed soft next to the player's own view
            // (2026-11-09).
            const bool upscaled=upscalers[0]==2&&At<void*>(upscalers,0x10)&&upscalers[0x29]&&inputs[8];
            if(upscaled){
                void* returned=g_upscaleNext(scope,upscalers,inputs);
                std::lock_guard lock(g_lock);
                if((g_liveNow->armed||g_farShot.captureNow)&&g_target->view)DeferFar(scope,upscalers-0xf38,inputs,At<ImageView>(inputs,0x40),exposure,false);
                {static uint64_t told=0;if(const uint64_t now=GetTickCount64();now-told>20000){told=now;DescribeUpscaled("second camera's (own upscaler)",returned,inputs,upscalers);}}
                return returned;
            }
            if(g_liveRaw){
                std::lock_guard lock(g_lock);
                if((g_liveNow->armed||g_farShot.captureNow)&&g_target->view)DeferFar(scope,upscalers-0xf38,inputs,sceneColor,exposure,true);
                void* returned=g_upscaleNext(scope,upscalers,inputs);
                {static uint64_t told=0;if(const uint64_t now=GetTickCount64();now-told>20000){told=now;DescribeUpscaled("second camera's",returned,inputs,upscalers);}}
                return returned;
            }
            void* returned=g_upscaleNext(scope,upscalers,inputs);
            std::lock_guard lock(g_lock);
            {
                static uint64_t finalChecked=0;
                if(const uint64_t now=GetTickCount64();now-finalChecked>1000){finalChecked=now;g_liveFinalTest=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\live.final")!=INVALID_FILE_ATTRIBUTES;}
            }
            if((g_liveNow->armed||g_farShot.captureNow)&&g_target->view)DeferFar(scope,upscalers-0xf38,inputs,g_liveFinalTest?g_target->color:sceneColor,exposure,false);
            {static uint64_t told=0;if(const uint64_t now=GetTickCount64();now-told>20000){told=now;DescribeUpscaled("second camera's (after)",returned,inputs,upscalers);}}
            return returned;
        }
    }
    if(lease&&!g_snapshotHolding){
        // WorldState: null once the scene is destroyed (an unguarded read of the stale scene
        // here crashed the game at exe+0x13b1b2 on leaving a world, 2026-10-28).
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
        // The portals' glow on the ground, as the player saw the spot (the portals themselves stay hidden).
        if(g_target->ticket)InFrameDrawGuard(scope,upscalers-0xf38,inputs,true);
        SnapshotCaptureGuard(scope,upscalers-0xf38,sceneColor,sceneColor,exposure);
    }
    if(lease&&g_world&&g_composite&&g_sharedGpu){
        std::lock_guard lock(g_lock);
        if(auto* state=WorldState();state&&upscalers-0xf38==state)SharedGpuRecordGuard(scope);
    }
    void* returned=g_upscaleNext(scope,upscalers,inputs);
#ifdef TOWN_PORTAL_DEV
    if(lease&&g_world){
        std::lock_guard lock(g_lock);
        static uint64_t told=0;
        if(auto* state=WorldState();state&&upscalers-0xf38==state)
            if(const uint64_t now=GetTickCount64();now-told>20000){told=now;DescribeUpscaled("player's",returned,inputs,upscalers);}
    }
#endif
    return returned;
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
            {
                FixedPos at{};std::memcpy(&at,static_cast<const uint8_t*>(input)+0x38,sizeof(at));
                ToFloat(at,g_playerCamera.x,g_playerCamera.y,g_playerCamera.z);
                g_havePlayerCamera=math::Finite(g_playerCamera);
            }
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
    // Dev: tools/owned.after renders the second camera after the player's view instead of before
    // it (which of the two leaves its mark on what the game keeps once for all views?).
    static uint64_t afterChecked=0;static bool ownedAfter=false;
    if(const uint64_t tick=GetTickCount64();tick-afterChecked>1000){afterChecked=tick;ownedAfter=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/owned.after")!=INVALID_FILE_ATTRIBUTES;}
    bool ownedLater=false;
    if(lease&&g_ownedPrepared.load()&&reinterpret_cast<uint64_t>(scene)==g_world->scene&&g_renderOnce.exchange(false)) {
        if(ownedAfter)ownedLater=true;
        else {
            std::lock_guard lock(g_lock);
            g_ownedPrepared=false;
            if(g_target&&g_target->view&&g_world->ownedBound&&!g_target->error)
                RenderOwnedOnce(context,input);
        }
    }
    g_inFrameDrawn=false;
#ifdef TOWN_PORTAL_DEV
    // Dev test: tools\player.nomesher renders the player's own view with its terrain mesher out
    // of sight (the brick pass then cuts nothing away near the camera), to see how the game's
    // bricks look close up.
    uint8_t* heldState=nullptr;void* heldMesher=nullptr;
    if(lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene){
        static uint64_t checked=0;static bool on=false;
        if(const uint64_t now=GetTickCount64();now-checked>1000){checked=now;on=GetFileAttributesW(L"C:\\Mods\\Enshrouded\\TownPortal\\tools\\player.nomesher")!=INVALID_FILE_ATTRIBUTES;}
        if(on){heldState=WorldState();if(heldState){heldMesher=At<void*>(heldState,0x978);At<void*>(heldState,0x978)=nullptr;}}
    }
#endif
    // Dev: tools/fx.nowater clears the render input's byte +0x424 for this call, which sends the
    // player's own view down the effect path a view without the water feature takes (does that
    // path draw flames at all?).
    uint8_t* waterByte=nullptr;uint8_t waterSaved=0;
    if(lease){
        static uint64_t waterChecked=0;static bool waterOff=false;
        if(const uint64_t t=GetTickCount64();t-waterChecked>1000){waterChecked=t;waterOff=GetFileAttributesW(L"C:/Mods/Enshrouded/TownPortal/tools/fx.nowater")!=INVALID_FILE_ATTRIBUTES;}
        if(waterOff&&input){
            waterByte=(actualInput!=input?mappedInput.data():static_cast<uint8_t*>(input))+0x424;waterSaved=*waterByte;*waterByte=0;
            static uint64_t toldAt=0;
            if(const uint64_t t=GetTickCount64();t-toldAt>5000){toldAt=t;char line[120]{};std::snprintf(line,sizeof(line),"Town Portal: fx.nowater: input +0x424 was %u (%s input)",unsigned(waterSaved),actualInput!=input?"mapped":"own");greeneye::Host()->log(line);}
        }
    }
    const bool playersOwn=lease&&g_world&&reinterpret_cast<uint64_t>(scene)==g_world->scene;
    if(playersOwn)g_inPlayerRender=true;
    void* returned=actualInput!=input?CallWithCamera(g_next,result,scene,context,input,target,mappedInput.data()):
        g_next(result,scene,context,input,target);
    if(playersOwn)g_inPlayerRender=false;
    if(waterByte)*waterByte=waterSaved;
#ifdef TOWN_PORTAL_DEV
    if(heldState&&heldMesher)At<void*>(heldState,0x978)=heldMesher;
#endif
    if(ownedLater){
        std::lock_guard lock(g_lock);
        g_ownedPrepared=false;
        if(g_target&&g_target->view&&g_world&&g_world->ownedBound&&!g_target->error)
            RenderOwnedOnce(context,input);
    }
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
    // Layout probes must fail closed when the game's layout may have moved: every native function
    // and every layout guard has to match (portal_natives.h), on any build.
    auto* module=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if(!natives::RenderReady()) {
        host->log("Town Portal scene probe: native checks did not pass; native research disabled");
        return false;
    }
    g_registerImage=reinterpret_cast<RegisterImageFn>(natives::At(0x11a2ab0));
    g_unregisterImage=reinterpret_cast<UnregisterImageFn>(natives::At(0x11a6180));
    g_importImage=reinterpret_cast<ImportImageFn>(natives::At(0x102ceb0));
    g_freeImage=reinterpret_cast<FreeImageFn>(natives::At(0x102ed30));
    g_output=static_cast<ImageView*>(host->keep("portal-imported-output-v1",sizeof(ImageView)));
    if(!g_output)return false;
    g_bindImage=reinterpret_cast<BindImageFn>(natives::At(0x10345a0));
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
    g_composite=static_cast<CompositeState*>(host->keep("portal-gpu-composite-v8",sizeof(CompositeState)));
    if(!g_composite)return false;
    g_sharedGpu=static_cast<SharedGpuState*>(host->keep("portal-shared-snapshot-gpu-v1",sizeof(SharedGpuState)));
    if(!g_sharedGpu)return false;
    g_gagGpu=static_cast<GagGpuState*>(host->keep("portal-easter-gpu-v1",sizeof(GagGpuState)));
    if(!g_gagGpu)return false;
    g_snapshotCaptureHash=static_cast<uint32_t*>(host->keep("portal-snapshot-capture-hash-v1",sizeof(uint32_t)));
    if(!g_snapshotCaptureHash)return false;
    // 14 bytes from the function's first byte (40 53: the push has a prefix): a push, a stack step, a
    // load and a stack address; no relative operands. (A signature starting at the 53 put the hook
    // one byte in and broke the call's alignment argument, 2026-11-10.)
    if(auto* strict=host->find("40 53 48 83 EC 40 48 8B 01 4C 8D 54 24 30 4D 8B D8 66 C7 44 24 30 03 02 48 8B 58 08 41 0F B6 01",nullptr))
        g_strictAlloc=reinterpret_cast<StrictAllocFn>(host->hook(strict,reinterpret_cast<void*>(StrictAllocHook),14,nullptr,0));
    host->log(g_strictAlloc?"Town Portal: render memory requests that do not fit are given general memory":"Town Portal: render memory call not found");
    // 15 bytes: four stack saves; no relative operands.
    if(auto* pool=host->find("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 4C 89 44 24 18 57 41 56 41 57 48 83 EC 60 48 8D A9 F8 00 00 00 48 8B F9 48",nullptr))
        g_poolAlloc=reinterpret_cast<PoolAllocFn>(host->hook(pool,reinterpret_cast<void*>(PoolAllocHook),15,nullptr,0));
    host->log(g_poolAlloc?"Town Portal: render memory: every request that does not fit is given general memory":"Town Portal: render memory allocate not found");
    if(auto* site=host->find("4C 8B C8 C7 44 24 20 FF FF FF FF 45 33 C0 48 8D 55 60 49 8B CC E8 ?? ?? ?? ??",nullptr))
        g_tableFill=reinterpret_cast<TableFillFn>(host->redirectCall(site+21,reinterpret_cast<void*>(TableFillHook)));
    host->log(g_tableFill?"Town Portal: live view: effects keep their frame-to-frame table in the far view":"Town Portal: live view: effect table wipe not found");
    g_fxPrivate=static_cast<FxPrivate*>(host->keep("portal-fx-private-v1",sizeof(FxPrivate)));
    // 13 bytes: eight pushes; no relative operands. Client exe only (the dedicated server has
    // its own start-up and does not render).
    if(auto* tasks=host->find("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 C8 FB FF FF 48 81 EC 38 05 00 00",nullptr))
        g_taskSystemCreate=reinterpret_cast<TaskSystemCreateFn>(host->hook(tasks,reinterpret_cast<void*>(TaskSystemCreateHook),13,nullptr,0));
    host->log(g_taskSystemCreate?"Town Portal: task system create taken (more task queues from the next game start)":"Town Portal: task system create not found");
    LiveViewSetting(); // town-portal.ini's LiveView brought up to date (Auto) and logged at start
    // 17 bytes: a stack save and eight pushes; no relative operands.
    if(auto* atlas=host->find("48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC 98 00 00 00 48 8B 02",nullptr))
        g_fogAtlas=reinterpret_cast<FogAtlasFn>(host->hook(atlas,reinterpret_cast<void*>(FogAtlasHook),17,nullptr,0));
    host->log(g_fogAtlas?"Town Portal: fog tile pool create taken (larger pool from the next game start)":"Town Portal: fog tile pool create not found");
    if(auto* found=host->find("48 8B D1 48 8D 8D F0 84 00 00 88 85 B1 0C 00 00 E8 ?? ?? ?? ??",nullptr))
        g_glowPre=reinterpret_cast<GlowPreFn>(host->redirectCall(found+16,reinterpret_cast<void*>(EarlyColourHook)));
    g_glowPreSeen=g_glowPre;
    {
        static const char* sites[3]={"8D F0 84 00 00 0F 11 8D 60 5B 00 00 E8 ?? ?? ?? ??","85 10 69 00 00 0F 11 8D D0 68 00 00 E8 ?? ?? ?? ??","11 8D B0 4D 00 00 48 8B D3 48 8D 8D F0 84 00 00 E8 ?? ?? ?? ??"};
        static const int at[3]={12,12,16};
        void* hooks[3]={reinterpret_cast<void*>(VfxPassHook0),reinterpret_cast<void*>(VfxPassHook1),reinterpret_cast<void*>(VfxPassHook2)};
        int taken=0;
        for(int i=0;i<3;++i)if(auto* found=host->find(sites[i],nullptr)){
            g_vfxPass[i]=reinterpret_cast<VfxPassFn>(host->redirectCall(found+at[i],hooks[i]));
            if(g_vfxPass[i])++taken;
        }
        g_vfxPassAll=taken==3;
        if(auto* found=host->find("0F 10 8B 20 02 00 00 0F 11 8D 10 57 00 00 48 8B 90 70 6B 01 00 E8 ?? ?? ?? ??",nullptr))
            g_preWater=reinterpret_cast<PreWaterFn>(host->redirectCall(found+21,reinterpret_cast<void*>(PreWaterHook)));
        host->log(g_preWater?"Town Portal: portals are drawn before the game's water":"Town Portal: pre-water call not found");
        if(auto* found=host->find("48 8B 93 48 0B 00 00 4C 8D 85 80 2A 00 00 48 8D 8D F0 84 00 00 E8 ?? ?? ?? ??",nullptr))
            g_waterRaster=reinterpret_cast<PreWaterFn>(host->redirectCall(found+21,reinterpret_cast<void*>(WaterRasterHook)));
        host->log(g_waterRaster?"Town Portal: portal depth goes in before the game's water":"Town Portal: water raster call not found");
        char line[120]{};std::snprintf(line,sizeof(line),"Town Portal: effect pass calls taken: %d of 3",taken);host->log(line);
    }
    host->log(g_glowPre?"Town Portal: glow pre-pass call taken (portals drawn before it)":"Town Portal: glow pre-pass call not found; portals are drawn at the upscaler");
    if(auto* site=host->find("85 F0 84 00 00 F3 0F 11 85 7C 35 00 00 48 83 C1 08 48 8B 90 A8 09 00 00 E8 ?? ?? ?? ??",nullptr))
        g_effectTick=reinterpret_cast<EffectTickFn>(host->redirectCall(site+24,reinterpret_cast<void*>(EffectTickHook)));
    host->log(g_effectTick?"Town Portal: live view: the far view's effect tick is taken":"Town Portal: live view: effect tick call not found");
    // 16 bytes: a stack copy, a stack step, a load and a clear; no relative operands.
    if(auto* solver=host->find("48 8B C4 48 81 EC 98 00 00 00 48 8B 11 45 33 C9 44 89 4A 6C 44 39 4A 10",nullptr))
        g_mipSolver=reinterpret_cast<MipSolverFn>(host->hook(solver,reinterpret_cast<void*>(FarMipsHook),16,nullptr,0));
    host->log(g_mipSolver?"Town Portal: live view: the far side's textures are asked for at full detail":"Town Portal: live view: texture solver not found; far textures keep the game's detail");
    g_depthPass=static_cast<DepthPassState*>(host->keep("portal-depth-pass-v1",sizeof(DepthPassState)));
    g_depthBegin=reinterpret_cast<DepthBeginFn>(host->find("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC B0 01 00 00 49 8B D8 4C 89 4C 24 30 48 8B FA 48 8B F1 33 D2 48 8D 4C 24 38 41 B8 18 01 00 00 E8",nullptr));
    g_motion=static_cast<MotionState*>(host->keep("portal-motion-pass-v1",sizeof(MotionState)));
    if(!g_motion)return false;
    g_snapshotScratch=static_cast<SnapshotScratch*>(host->keep("portal-snapshot-scratch-v1",sizeof(SnapshotScratch)));
    g_liveSecond=static_cast<SnapshotScratch*>(host->keep("portal-live-second-v1",sizeof(SnapshotScratch)));
    if(!g_snapshotScratch)return false;
    g_world=static_cast<WorldTemplate*>(host->keep("portal-owned-world-v1",sizeof(WorldTemplate)));
    g_mirrors=static_cast<MirrorState*>(host->keep("portal-owned-instances-v3",sizeof(MirrorState)));
    g_objectMemory=static_cast<ObjectMemory*>(host->keep("portal-object-memory-v2",sizeof(ObjectMemory)));
    g_instanceCreate=reinterpret_cast<InstanceCreateFn>(host->find(signatures::instanceCreate,nullptr));
    g_instanceRemove=reinterpret_cast<InstanceRemoveFn>(host->find(signatures::instanceRemove,nullptr));
    g_instanceTransform=reinterpret_cast<InstanceTransformFn>(host->find(signatures::instanceTransform,nullptr));
    // Optional (animated things in the live view): the skeleton group calls, build of 2026-11-02.
    g_plantZones=reinterpret_cast<ZonesFn>(host->find("48 8B 89 90 0A 00 00 48 85 C9 0F 85 ?? ?? ?? ?? C3",nullptr));
    g_instanceFlag=reinterpret_cast<InstanceFlagFn>(host->find("48 89 5C 24 08 57 48 83 EC 20 41 0F B6 F8 48 8B D9 41 B0 01 E8 ?? ?? ?? ?? 48 85 C0 74 15 41 B0 02 40 88 B8 3C 01 00 00",nullptr));
    if(g_instanceFlag){
        auto* code=reinterpret_cast<uint8_t*>(g_instanceFlag);
        // mov rdx,rax; mov rcx,rbx; call
        if(code[0x28]==0x48&&code[0x29]==0x8b&&code[0x2a]==0xd0&&code[0x2b]==0x48&&code[0x2c]==0x8b&&code[0x2d]==0xcb&&code[0x2e]==0xe8){
            int32_t rel=0;std::memcpy(&rel,code+0x2f,4);
            g_instanceMark=reinterpret_cast<InstanceMarkFn>(code+0x33+rel);
        }
    }
    // Optional (the second camera's frame size follows the opening): the game's dynamic resolution call.
    // 14 bytes: two stack saves and three pushes, no relative operands.
    if(auto* entry=host->find("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 80 00 00 00 48 8B 41 08 0F 57 C0 48 8B 99 AC 00 00 00 48 8B E9 0F 29 74 24 70 0F 28 F1",nullptr))
        g_renderScale=reinterpret_cast<RenderScaleFn>(host->hook(entry,reinterpret_cast<void*>(RenderScaleHook),14,nullptr,0));
    host->log(g_renderScale?"Town Portal: live view: frame size follows the opening":"Town Portal: live view: render size call not found; the second camera keeps its full frame");
    g_actors=static_cast<ActorState*>(host->keep("portal-owned-actors-v2",sizeof(ActorState)));
    // Optional (effects in the live view): the effect system's start and stop, build of 2026-11-02.
    g_effects=static_cast<EffectState*>(host->keep("portal-owned-effects-v2",sizeof(EffectState)));
    g_effectCreate=reinterpret_cast<EffectCreateFn>(host->find("48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B 6A 08 48 8B FA 48 8B 52 28 49 8B F1 4D 8B D8 4C 8B F1 48 85 D2",nullptr));
    g_effectDestroy=reinterpret_cast<EffectDestroyFn>(host->find("85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 56 57 48 81 EC E8 00 00 00 48 8B 71 08 48 8B F9 0F B7 CA 48 3B 4F 58 0F 83 ?? ?? ?? ?? 48 89 58 08",nullptr));
    host->log(g_effects&&g_effectCreate&&g_effectDestroy?"Town Portal: live view: effects are twinned":"Town Portal: live view: effect calls not found; no effects in the far view");
    g_groupCreate=reinterpret_cast<GroupCreateFn>(host->find("40 55 53 56 41 57 48 8D 6C 24 C1 48 81 EC 88 00 00 00 48 8B 42 08 33 DB 48 0B 02 4C 8B FA 48 8B F1 48 89 5D D7 48 89 5D",nullptr));
    g_groupBind=reinterpret_cast<GroupBindFn>(host->find("48 89 5C 24 18 48 89 6C 24 20 56 41 54 41 57 48 83 EC 20 45 33 E4 41 8B D8 4C 8B FA 48 8B F1 41 8B EC 45 85 C0 74 74 48",nullptr));
    g_groupPose=reinterpret_cast<GroupPoseFn>(host->find("85 D2 0F 84 ?? ?? ?? ?? 48 89 74 24 20 41 56 48 83 EC 30 48 8B F1 44 0F B7 CA 48 8B 89 E8 03 00 00 45 8B D1 49 D3 EA 4D",nullptr));
    g_groupDestroy=reinterpret_cast<GroupDestroyFn>(host->find("85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 20 57 48 83 EC 20 48 8B F9 8B DA 48 8B 89 E8 03 00 00 0F B7 D3 44 8B C2 49 D3 E8 48",nullptr));
    host->log(g_actors&&g_groupCreate&&g_groupBind&&g_groupPose&&g_groupDestroy?"Town Portal: live view: animated things are mirrored":"Town Portal: live view: skeleton group calls not found; no animated things");
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
    if(reinterpret_cast<void*>(g_sceneIbl)!=natives::At(0x1122c0))return false;
    g_terrainRequest=reinterpret_cast<TerrainRequestFn>(host->find(signatures::terrainRequest,nullptr));
    if(reinterpret_cast<void*>(g_terrainRequest)!=natives::At(0x10c3580))return false;
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
    // The brick tree's creation (FUN_14004f6b0; 15 bytes: a stack save, four pushes and a
    // stack-relative lea, checked against the build of 2026-11-02). Optional: without it the
    // second camera shows the player's tree, as before.
    g_treeAllocator=static_cast<TreeAllocatorKeep*>(host->keep("portal-tree-allocator-v1",sizeof(TreeAllocatorKeep)));
    g_goneTree=static_cast<GoneTree*>(host->keep("portal-gone-tree-v1",sizeof(GoneTree)));
    if(auto* create=host->find("48 89 54 24 10 55 53 56 41 57 48 8D 6C 24 C1 48 81 EC B8 00 00 00 48 8B 01 4C 8B FA 41 B1 08",nullptr);create&&g_treeAllocator){
        g_brickTreeCreate=reinterpret_cast<BrickTreeCreateFn>(host->hook(create,reinterpret_cast<void*>(BrickTreeCreateHook),15,nullptr,0));
        host->log(g_brickTreeCreate?"Town Portal: live view: the second camera gets a brick tree of its own":"Town Portal: live view: brick tree hook not placed");
    } else host->log("Town Portal: live view: brick tree creation not found; the second camera shares the player's");
#ifdef TOWN_PORTAL_DEV
    // Dev only, by address (build of 2026-11-02): 15 bytes, two stack saves, a push and a
    // stack adjust, checked against the exe.
    {
        auto* entry=host->find("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 51 78 48 8B F9 48 85 D2 74 09 48 8B 49 70 E8",nullptr);
        if(entry)g_brickTreeDestroy=reinterpret_cast<BrickTreeDestroyFn>(host->hook(entry,reinterpret_cast<void*>(BrickTreeDestroyHook),15,nullptr,0));
        if(!g_brickTreeDestroy)g_brickTreeDestroy=reinterpret_cast<BrickTreeDestroyFn>(natives::At(0x526f0)); // unhooked: the hook just is not there
        host->log(entry?"Town Portal: dev: brick tree destroy found":"Town Portal: dev: brick tree destroy not found");
        host->log(g_brickTreeDestroy?"Town Portal: dev: brick tree destroy observed":"Town Portal: dev: brick tree destroy hook not placed");
    }
#endif
    if(!rootEntry||!createEntry||!bindEntry||!destroyEntry||!tickEntry||!projectionEntry||!projectionMatrixEntry) return false;
    g_projectionCaller=natives::At(0x12d372);
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
    // Optional: renderView's call into the upscaler (0x13d5ab -> 0x11a870). Without it
    // portals are drawn over the finished frame.
    if(host->redirectCall){
        if(auto* found=host->find("00 00 83 BF 80 02 00 00 20 0F 94 85 04 4B 00 00 E8 ?? ?? ?? ??",nullptr)){
            auto* site=found+16;int32_t delta=0;std::memcpy(&delta,site+1,4);
            if(host->generation>1||site+5+delta==natives::At(0x11a870))
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
            if(site==natives::At(snapshot::hide::callRva)&&site[0]==0xe8&&
               target==natives::At(snapshot::hide::targetRva)&&
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
    // Automatic live view (LiveAuto): a portal on screen asks for the second camera, which the
    // render thread then makes, attaches and later lets go by itself (LiveLifecycle).
    const bool automatic=LiveAuto(now);
    if(automatic)g_liveWantTick=now;
    if(now>=g_displayProbeUntil&&!automatic)return 0;
    if(!g_target->view||!g_world->ownedBound||g_target->error)return 0;
    // One second camera: of the portal ends on screen the nearest to the player's camera gets
    // the live view (the first one drawn got it before, which with both ends of a portal in
    // sight was the farther one, 2026-11-10). The end that has it keeps it unless another is
    // clearly nearer, so it does not flip between two at about the same distance.
    {
        static float bestDistance=0;static uint64_t keptPortal=0;static bool keptField=false;
        const auto& end=fieldSide?portal.fieldEnd:portal.baseEnd;
        float distance=0;
        if(g_havePlayerCamera){
            const float dx=end.wx-g_playerCamera.x,dy=end.wy+1.62f-g_playerCamera.y,dz=end.wz-g_playerCamera.z;
            distance=std::sqrt(dx*dx+dy*dy+dz*dz);
        }
        if(portal.portalId==keptPortal&&fieldSide==keptField)distance*=0.8f;
        static float secondDistance=0;
        if(g_liveSelectTick!=now){
            g_livePortal=portal;g_liveField=fieldSide;g_liveSelectTick=now;g_liveUntil=now+250;
            bestDistance=distance;keptPortal=portal.portalId;keptField=fieldSide;
            secondDistance=1e30f;
        } else if(portal.portalId==g_livePortal.portalId&&fieldSide==g_liveField){
            // asked again for the same end in this tick
        } else if(distance<bestDistance){
            g_livePortal2=g_livePortal;g_liveField2=g_liveField;secondDistance=bestDistance;g_liveSecondTick=now;
            g_livePortal=portal;g_liveField=fieldSide;g_liveUntil=now+250;
            bestDistance=distance;keptPortal=portal.portalId;keptField=fieldSide;
        } else if(distance<secondDistance){
            g_livePortal2=portal;g_liveField2=fieldSide;secondDistance=distance;g_liveSecondTick=now;
        }
    }
    if(now>=g_displayProbeUntil)return 0; // no diagnostic panel outside the probe
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
int LiveTestMode() {return g_liveTestMode.load();}
void ShowGag(const PortalInstance& portal,int place,std::shared_ptr<sharing::Image> image) {ShowGagImpl(portal,place,std::move(image));}
int GagPlace() {return GagPlaceImpl();}
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
// The player's camera and its right, for the portals' sound (portal_ambient).
bool ListenerPose(float& x,float& y,float& z,float& rightX,float& rightZ) {
    std::lock_guard lock(g_lock);
    if(!g_havePlayerCamera)return false;
    x=g_playerCamera.x;y=g_playerCamera.y;z=g_playerCamera.z;
    rightX=1.f;rightZ=0.f;
    if(auto* state=WorldState()){
        const float* view=&At<float>(state,0x1000);
        const float len=std::sqrt(view[0]*view[0]+view[8]*view[8]);
        if(std::isfinite(len)&&len>1e-3f){rightX=view[0]/len;rightZ=view[8]/len;}
    }
    return true;
}
bool LiveFarPoint(float& x,float& z) {
    std::lock_guard lock(g_lock);
    if(!g_livePortal.active||GetTickCount64()>g_liveUntil+1500)return false;
    const auto& other=g_liveField?g_livePortal.baseEnd:g_livePortal.fieldEnd;
    x=other.wx;z=other.wz;
    return true;
}
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
