#include "matroska/KaxFrameStore.h"
#include "fixture.h"
#include <cstdio>
int main() {
 KaxTrackEntry track; KaxCluster cluster; fill(track,cluster);
 auto expected=render(cluster);
 auto io=std::make_shared<MemIOCallback>();
 auto store=std::make_shared<KaxFrameStore>(io);
 if(!cluster.SpoolFrames(store) || released!=2) return 10;
 const auto size=io->GetDataBufferSize();
 if(size<305 || !cluster.SpoolFrames(store) || io->GetDataBufferSize()!=size) return 11;
 store.reset(); io.reset();
 if(render(cluster)!=expected) return 12;
 std::puts("SPOOL_CONSUMER_OK released=2 equal=1");
}
