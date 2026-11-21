#include "fg/FrameGraph.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

struct Stats {
  int creates=0, destroys=0, preReads=0, preWrites=0, pendingDestroy=0;
  bool pending=false;
  std::vector<int> order;
};
struct CpuTexture {
  struct Desc { Stats *stats; };
  int value=0;
  bool alive=false;
  void create(const Desc& d, void*) { assert(!alive);alive=true;value=0;++d.stats->creates; }
  void destroy(const Desc& d, void*) { assert(alive);if(d.stats->pending)++d.stats->pendingDestroy;alive=false;++d.stats->destroys; }
  void preRead(const Desc& d, uint32_t f, void*) { assert(alive && f==11);++d.stats->preReads; }
  void preWrite(const Desc& d, uint32_t f, void*) { assert(alive && f==22);++d.stats->preWrites; }
};
struct Data { FrameGraphResource texture; };
static void normal() {
  Stats s; FrameGraph fg;
  auto h=fg.addCallbackPass<Data>("geometry",[&](auto& b,auto& d) {
    d.texture=b.template create<CpuTexture>("gbuffer",{&s});d.texture=b.write(d.texture,22);
  },[&](const auto& d,auto& r,void*) {
    auto& t=r.template get<CpuTexture>(d.texture);assert(t.alive);t.value=7;s.order.push_back(1);
  }).texture;
  int result=0;
  fg.addCallbackPass<Data>("lighting",[&](auto& b,auto& d) {d.texture=b.read(h,11);d.texture=b.write(h,22);b.setSideEffect();},
    [&](const auto& d,auto& r,void*) {result=r.template get<CpuTexture>(d.texture).value*3;s.order.push_back(2);});
  fg.addCallbackPass("culled",[](auto&,auto&){},[](const auto&,auto&,void*){assert(false);});
  fg.compile();fg.execute();
  assert(result==21 && s.creates==1 && s.destroys==1 && s.preReads==1 && s.preWrites==2);
  assert((s.order==std::vector<int>{1,2}));
  std::printf("SYNC value=%d creates=%d destroys=%d hooks=%d/%d\n",result,s.creates,s.destroys,s.preReads,s.preWrites);
  Stats importedStats; FrameGraph imported;
  CpuTexture back;back.alive=true;back.value=4;
  auto backId=imported.import("backbuffer",CpuTexture::Desc{&importedStats},std::move(back));
  int importedValue=0;
  imported.addCallbackPass<Data>("present",[&](auto& b,auto& d){d.texture=b.write(backId);},
    [&](const auto& d,auto& r,void*) {auto& t=r.template get<CpuTexture>(d.texture);t.value+=5;importedValue=t.value;});
  imported.compile();imported.execute();
  assert(importedValue==9 && importedStats.creates==0 && importedStats.destroys==0);
  std::printf("IMPORTED value=%d creates=%d destroys=%d\n",importedValue,importedStats.creates,importedStats.destroys);
  std::puts("SYNCHRONOUS_NORMAL_OK");
}
static void legacyDeferredProbe() {
  Stats s; FrameGraph fg; std::function<void()> deviceComplete;
  int lateUse=0,observed=-1;
  auto h=fg.addCallbackPass<Data>("submit-gbuffer",[&](auto& b,auto& d) {
    d.texture=b.template create<CpuTexture>("deferred-gbuffer",{&s});d.texture=b.write(d.texture);
  },[&](const auto& d,auto& r,void*) {
    auto* texture=&r.template get<CpuTexture>(d.texture);s.pending=true;
    deviceComplete=[&,texture]{if(texture->alive)texture->value=7;else ++lateUse;s.pending=false;};
  }).texture;
  fg.addCallbackPass<Data>("consume-gbuffer",[&](auto& b,auto& d){d.texture=b.read(h);d.texture=b.write(h);b.setSideEffect();},
    [&](const auto& d,auto& r,void*){observed=r.template get<CpuTexture>(d.texture).value;});
  fg.compile();fg.execute();deviceComplete();
  assert(observed==0 && s.pendingDestroy==1 && lateUse==1);
  std::printf("LEGACY consumer_before_completion=%d destroyed_while_pending=%d late_use_rejected=%d\n",observed,s.pendingDestroy,lateUse);
  std::puts("CURRENT_COMPLETION_GAP_OBSERVED");
}
int main(int argc,char** argv) {
  normal();
  if(argc==1)legacyDeferredProbe();
  else assert(argc==2 && std::strcmp(argv[1],"--normal")==0);
}
