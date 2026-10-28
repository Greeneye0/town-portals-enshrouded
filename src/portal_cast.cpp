#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "portal_cast.h"
#include "portal_cast_hooks.h"
#include "portal_snapshot_timing.h"
#include "portal_cast_data.h"
#include "portal_shrug_data.h"
#include <iterator>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace portal::casting {
namespace {
template<class T>T get(const void* p,size_t n){T v{};std::memcpy(&v,static_cast<const uint8_t*>(p)+n,sizeof(v));return v;}
using EvaluateFn=uint64_t(*)(uint32_t,void**);
using ComponentFn=void*(*)(void*,void*,uint32_t,uint32_t);
using StackFn=void(*)(void*,void*);
EvaluateFn g_next=nullptr;
ComponentFn g_component=nullptr;
StackFn g_stack=nullptr;
std::atomic<void*> g_graph{nullptr};
std::atomic<uint64_t> g_started{0},g_cancelled{0},g_requests{0},g_applied{0},g_matched{0},g_calls{0},g_faults{0};
std::atomic<uint32_t> g_jointCount{0},g_handle{0};
uint64_t g_lastBind=0;
// ClientPlayerInput (reflection hash low32 0x9621e4c3), filled each frame by
// 0x2437c0 from the local player's input builder at 0x3ab840 (evidence:
// C:\temp\enshrouded-re\out\movement-lock\evidence.txt). The server receives
// this data, so zeroing it here also stops movement on a dedicated server.
using InputFn=uint64_t(*)(uint8_t*,void*,void*,void*,void*,void*,void*,uint64_t,void*,void*,uint64_t,void*);
InputFn g_inputNext=nullptr;
constexpr size_t kMovementStick=0x2b8,kStickMode=0x2c0,kDigitalInput=0x3a8;
constexpr uint64_t kJump=1ull<<32;
// Evade, Jump, HeadDive, Glider, Sprint.
constexpr uint64_t kLockedInput=(1ull<<31)|kJump|(1ull<<33)|(1ull<<37)|(1ull<<53);
std::atomic<uint64_t> g_lockUntil{0};
std::atomic<uint64_t> g_inputCalls{0},g_blockedFrames{0},g_jumpAborts{0},g_inputFaults{0};
std::atomic<bool> g_jumpAbort{false},g_hardLock{false};
uint64_t g_heldInput=0,g_previousInput=0;
void* g_boundClient=nullptr;
// The clips this blends over the player's pose: the 3 s portal channel and the shrug when
// there is no room for a portal. Rotations: frames x joints x quaternion.
struct Clip {uint32_t fps,frames;float duration;const uint16_t* joints;size_t jointCount;const float* rotations;int pelvis;float drop;};
const Clip kChannel{data::kFps,data::kFrames,data::kDuration,data::kJoints,std::size(data::kJoints),&data::kRotations[0][0][0],data::kPelvisJoint,data::kPelvisDrop};
const Clip kShrug{shrug::kFps,shrug::kFrames,shrug::kDuration,shrug::kJoints,std::size(shrug::kJoints),&shrug::kRotations[0][0][0],-1,0.0f};
std::atomic<const Clip*> g_clip{&kChannel};
uint64_t ClipMs(const Clip& c){return uint64_t(c.duration*1000)+1;}
void Mix(const float* a,const float* b,float t,float* out){
 float dot=0;for(int i=0;i<4;++i)dot+=a[i]*b[i];
 float sign=dot<0?-1.0f:1.0f;dot=std::min(1.0f,std::fabs(dot));
 float aa=1-t,bb=t;
 if(dot<.9995f){float angle=std::acos(dot),s=std::sin(angle);aa=std::sin((1-t)*angle)/s;bb=std::sin(t*angle)/s;}
 float norm=0;for(int i=0;i<4;++i){out[i]=a[i]*aa+b[i]*bb*sign;norm+=out[i]*out[i];}
 norm=norm>1e-10f?1.0f/std::sqrt(norm):1;for(int i=0;i<4;++i)out[i]*=norm;
}
void Apply(void** context,uint64_t start){
 __try {
  auto* graph=static_cast<uint8_t*>(*context);
  if(!graph||graph!=g_graph.load(std::memory_order_acquire))return;
  ++g_matched;
  auto now=GetTickCount64();if(now<start)return;
  const Clip& clip=*g_clip.load(std::memory_order_acquire);
  float seconds=float(now-start)*.001f;
  if(seconds>clip.duration)return;
  auto* stack=graph+0x5e0;
  if(get<uint64_t>(stack,0x30)==0)return;
  void* pose[7]{};g_stack(pose,stack);
  auto joints=reinterpret_cast<uintptr_t>(pose[1]);g_jointCount=static_cast<uint32_t>(joints);
  if(joints<102||joints>256||!pose[0])return;
  float pos=std::min(float(clip.frames-1),seconds*clip.fps);
  auto frame=static_cast<uint32_t>(pos);auto next=std::min(frame+1,clip.frames-1);
  // Raise for 0.25 s, hold the clip, release over the last 0.25 s.
  float weight=std::min({1.0f,seconds/.25f,(clip.duration-seconds)/.25f});
  if(auto cancel=g_cancelled.load()){float release=1-float(now-std::min(now,cancel))/250.0f;if(release<=0)return;weight=std::min(weight,release);}
  weight=weight*weight*(3-2*weight);
  for(size_t j=0;j<clip.jointCount;++j){
   auto* dest=static_cast<float*>(pose[0])+clip.joints[j]*4;
   const float* a=clip.rotations+(size_t(frame)*clip.jointCount+j)*4;
   const float* b=clip.rotations+(size_t(next)*clip.jointCount+j)*4;
   float animation[4],blended[4];Mix(a,b,pos-frame,animation);
   Mix(dest,animation,weight,blended);std::memcpy(dest,blended,sizeof(blended));
  }
  // Low stance: the second pose array holds joint translations (float4 per joint).
  if(pose[2]&&clip.pelvis>=0&&uint32_t(clip.pelvis)<joints)static_cast<float*>(pose[2])[clip.pelvis*4+1]-=clip.drop*weight;
  ++g_applied;
 } __except(EXCEPTION_EXECUTE_HANDLER){++g_faults;g_started=0;}
}
uint64_t EvaluateHook(uint32_t node,void** context){
 greeneye::Lease lease;
 auto result=g_next(node,context);
 if(lease){
  ++g_calls;auto start=g_started.load(std::memory_order_acquire);
  auto now=GetTickCount64();auto cancel=g_cancelled.load();
  if(start&&(now-start>ClipMs(*g_clip.load())||(cancel&&now-std::min(now,cancel)>250)))g_started.compare_exchange_strong(start,0);
  else if(result&&start)Apply(context,start);
 }
 return result;
}
void LockInput(uint8_t* input){
 __try {
  uint64_t bits=get<uint64_t>(input,kDigitalInput);
  bool pressedJump=(bits&kJump)&&!(g_previousInput&kJump);
  g_previousInput=bits;
  if(GetTickCount64()<g_lockUntil.load()){
   if(pressedJump&&!g_hardLock){g_lockUntil=0;g_jumpAbort=true;g_heldInput=0;++g_jumpAborts;return;}
   ++g_blockedFrames;
   g_heldInput=kLockedInput;
   float zero[2]{};std::memcpy(input+kMovementStick,zero,sizeof(zero));input[kStickMode]=0;
  }
  // After the lock, keep a button that was held through it suppressed until
  // it is released, so it does not register as a fresh press.
  g_heldInput&=bits;
  if(g_heldInput){bits&=~g_heldInput;std::memcpy(input+kDigitalInput,&bits,sizeof(bits));}
 } __except(EXCEPTION_EXECUTE_HANDLER){g_lockUntil=0;g_heldInput=0;++g_inputFaults;}
}
uint64_t InputHook(uint8_t* input,void* a2,void* a3,void* a4,void* a5,void* a6,void* a7,uint64_t mode,void* a9,void* a10,uint64_t a11,void* a12){
 greeneye::Lease lease;
 auto result=g_inputNext(input,a2,a3,a4,a5,a6,a7,mode,a9,a10,a11,a12);
 if(lease&&input){++g_inputCalls;LockInput(input);}
 return result;
}
bool BindSafe(void* client){
 __try {
  auto* world=get<uint8_t*>(client,0x47d4e0);auto* storage=get<void*>(client,0x1d0);
  uint32_t local=get<uint32_t>(client,0x466b44);if(!world||!storage||!local)return false;
  local=(local&0x3f)+1;
  void* ref[3]{};g_component(ref,storage,0xb4c3563e,local);
  if(!ref[0])return false;
  uint32_t handle=get<uint32_t>(ref[0],0);uint32_t index=handle&0xffffff;
  auto* system=get<uint8_t*>(world,0x618);if(!system||!handle||index>=get<uint16_t>(system,0x78))return false;
  auto* generations=get<uint8_t*>(system,0x50);auto* pool=get<uint8_t*>(system,0x38);
  if(!generations||!pool||(handle>>24)-1!=uint32_t(generations[index])%255)return false;
  auto* graph=pool+size_t(index)*0xfd0;
  // The verified humanoid hierarchy has 102 joints, padded to a SIMD boundary.
  if(get<uint64_t>(graph,0x638)!=102)return false;
  g_graph.store(graph,std::memory_order_release);g_handle=handle;return true;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
bool Init(const GreeneyeHost* host){
 auto* exe=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
 auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(exe+reinterpret_cast<IMAGE_DOS_HEADER*>(exe)->e_lfanew);
 if(!greeneye::HostHasHooks(host)||!host->redirectCall||!host->read||nt->FileHeader.TimeDateStamp!=0x6ad77f0c||nt->OptionalHeader.SizeOfImage!=0x2a1e000)return false;
 auto* site=exe+0x92458a;
 if(!hooks::CheckCall(host->read,site,exe+0x91c260))return false;
 g_component=reinterpret_cast<ComponentFn>(exe+0x9a75d0);
 g_stack=reinterpret_cast<StackFn>(exe+0x92cde0);
 g_next=reinterpret_cast<EvaluateFn>(host->redirectCall(site,reinterpret_cast<void*>(EvaluateHook)));
 auto* input=exe+hooks::inputCallRva;
 if(hooks::CheckInputCall(host->read,input,exe+hooks::inputTargetRva))
  g_inputNext=reinterpret_cast<InputFn>(host->redirectCall(input,reinterpret_cast<void*>(InputHook)));
 if(host->log)host->log(g_inputNext?"Town Portal: chained movement lock ready; Jump cancels channel":"Town Portal: movement lock unavailable; portal casting disabled");
 if(host->log)host->log(g_next?"Town Portal: channel pose adapter loaded":"Town Portal: cast pose adapter requires a client restart");
 return g_next!=nullptr;
}
void Bind(void* client){
 auto now=GetTickCount64();if(client==g_boundClient&&now-g_lastBind<250)return;
 g_lastBind=now;g_boundClient=client;
 if(!g_next||!client||!BindSafe(client)){g_graph=nullptr;g_handle=0;}
}
bool SnapshotActorStates(void* client,uint64_t& states){
 states=0;
 if(!g_component||!client)return false;
 __try {
  auto* world=get<uint8_t*>(client,0x47d4e0);auto* storage=get<void*>(client,0x1d0);
  uint32_t local=get<uint32_t>(client,0x466b44);if(!world||!storage||!local)return false;
  local=(local&0x3f)+1;
  void* ref[3]{};g_component(ref,storage,0x3b912d39,local);
  if(!ref[0])return false;
  // Runtime ClientActor descriptor 1419fa400, size c38, namespace 141976b68
  // (same as AnimationGraphComponent2); 1419af880 is the ds descriptor. Native state query
  // 14021aaa0 merges pending adds/removals when bit 0 at +1b1 is set.
  states=snapshot::EffectiveActorStates(get<uint64_t>(ref[0],0xbf8),
      get<uint64_t>(ref[0],0xbd0),get<uint64_t>(ref[0],0xbd8),
      (get<uint8_t>(ref[0],0x1b1)&1)!=0);
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Start(void* client){
 Bind(client);if(!g_next||!g_graph.load())return false;
 g_clip.store(&kChannel,std::memory_order_release);
 g_cancelled=0;g_started.store(GetTickCount64(),std::memory_order_release);++g_requests;return true;
}
bool Shrug(void* client){
 Bind(client);if(!g_next||!g_graph.load())return false;
 // Never over a channel still playing.
 auto start=g_started.load();auto now=GetTickCount64();
 if(start&&g_clip.load()==&kChannel&&now-start<ClipMs(kChannel))return false;
 g_clip.store(&kShrug,std::memory_order_release);
 g_cancelled=0;g_started.store(now,std::memory_order_release);++g_requests;return true;
}
void Cancel(){if(g_started.load())g_cancelled=GetTickCount64();}
bool MovementLockReady(){return g_inputNext!=nullptr&&g_inputFaults.load()==0;}
void LockMovement(uint64_t until,bool hard){g_jumpAbort=false;g_hardLock=hard;g_lockUntil=until;}
void UnlockMovement(){g_lockUntil=0;g_hardLock=false;}
bool TakeJumpAbort(){return g_jumpAbort.exchange(false);}
void Stop(){g_started=0;g_cancelled=0;g_lockUntil=0;g_graph=nullptr;g_handle=0;g_boundClient=nullptr;}
uint64_t DurationMs(){return ClipMs(kChannel);}
void AppendStatus(char* json,uint32_t capacity){
 auto n=std::strlen(json);if(n<2||capacity<n+1024)return;
 auto start=g_started.load();auto now=GetTickCount64();
 std::snprintf(json+n-1,capacity-n+1,",\"cast\":{\"source\":\"generated 3 s lunge channel (tools/generate-portal-channel.py)\",\"hookReady\":%s,\"bound\":%s,\"graphHandle\":%u,\"graph\":\"0x%llx\",\"requests\":%llu,\"active\":%s,\"durationMs\":%llu,\"poseWrites\":%llu,\"matched\":%llu,\"evaluations\":%llu,\"joints\":%u,\"faults\":%llu}}",g_next?"true":"false",g_graph.load()?"true":"false",g_handle.load(),static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_graph.load())),g_requests.load(),start&&now>=start&&now-start<DurationMs()?"true":"false",DurationMs(),g_applied.load(),g_matched.load(),g_calls.load(),g_jointCount.load(),g_faults.load());
 n=std::strlen(json);auto lockUntil=g_lockUntil.load();
 std::snprintf(json+n-1,capacity-n+1,",\"castInput\":{\"hookReady\":%s,\"locked\":%s,\"remainingMs\":%llu,\"hard\":%s,\"calls\":%llu,\"blockedFrames\":%llu,\"jumpAborts\":%llu,\"faults\":%llu}}",MovementLockReady()?"true":"false",now<lockUntil?"true":"false",lockUntil-std::min(now,lockUntil),g_hardLock.load()?"true":"false",g_inputCalls.load(),g_blockedFrames.load(),g_jumpAborts.load(),g_inputFaults.load());
}
}
