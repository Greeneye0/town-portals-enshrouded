#include "../src/portal_cast.cpp"
#include <array>
#include <cassert>
#include <vector>
namespace {
std::array<float,102*4> rotations{},translations{};
void* expectedStack{};
void Stack(void* output,void* stack){
 assert(stack==expectedStack);
 auto** pose=static_cast<void**>(output);
 pose[0]=rotations.data();pose[1]=reinterpret_cast<void*>(102);
 pose[2]=translations.data();pose[3]=reinterpret_cast<void*>(102);
}
void* Component(void* output,void*,uint32_t hash,uint32_t entity){
 assert(hash==0xb4c3563e&&entity==1);
 static uint32_t handle=0x02000000;
 *static_cast<void**>(output)=&handle;return output;
}
}
int main(){
 using namespace portal::casting;
 std::array<uint8_t,0xfd0> graph{},other{};
 // Actual binding path: the UI world's graph-system field moved +0x10.
 std::vector<uint8_t> client(0x47d4e8),world(0x638),system(0x80);
 uint8_t generation=1;void* worldPointer=world.data();void* systemPointer=system.data();
 void* poolPointer=graph.data();void* generations=&generation;int storage=0;void* storagePointer=&storage;
 uint16_t capacity=1;uint32_t local=64;uint64_t joints=102;
 std::memcpy(client.data()+0x47d4e0,&worldPointer,8);std::memcpy(client.data()+0x1d0,&storagePointer,8);
 std::memcpy(client.data()+0x466b44,&local,4);std::memcpy(world.data()+0x628,&systemPointer,8);
 std::memcpy(system.data()+0x38,&poolPointer,8);std::memcpy(system.data()+0x50,&generations,8);
 std::memcpy(system.data()+0x78,&capacity,2);std::memcpy(graph.data()+0x638,&joints,8);
 g_component=Component;assert(BindSafe(client.data())&&g_graph==graph.data()&&g_handle==0x02000000);
 std::memset(world.data()+0x628,0,8);assert(!BindSafe(client.data()));
 uint64_t depth=1;std::memcpy(graph.data()+0x610,&depth,sizeof(depth));
 for(size_t j=0;j<102;++j)rotations[j*4+3]=1;
 expectedStack=graph.data()+0x5e0;g_stack=Stack;g_graph=graph.data();
 void* context=graph.data();auto now=GetTickCount64();assert(now>1500);
 Apply(&context,now-1500);
 assert(g_applied==1&&g_faults==0&&g_jointCount==102);
 bool changed=false;
 for(auto j:data::kJoints){
  auto* q=rotations.data()+j*4;
  float norm=0;for(int i=0;i<4;++i)norm+=q[i]*q[i];assert(std::fabs(norm-1)<.001f);
  changed|=std::fabs(q[0])+std::fabs(q[1])+std::fabs(q[2])>.001f;
 }
 assert(changed);auto saved=rotations;
 context=other.data();Apply(&context,now-1500);assert(g_applied==1&&rotations==saved);
 context=graph.data();Apply(&context,now-DurationMs()-100);assert(g_applied==1);
 puts("PASS: native player graph binding at world+628; channel pose changes humanoid joints, preserves normalized rotations, excludes other actors and expired casts");
}
