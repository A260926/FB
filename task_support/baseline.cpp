#include "fixture.h"
#include <cstdio>
#include <algorithm>
int main() {
 KaxTrackEntry track; KaxCluster cluster; fill(track,cluster);
 auto first=render(cluster); auto second=render(cluster);
 if(first!=second || first.size()<305 || released!=0) return 1;
 if(std::count(first.begin(),first.end(),0x42)<300) return 2;
 std::printf("RESIDENT_RENDER bytes=%zu repeat_equal=1 released=0\n",first.size());
 cluster.ReleaseFrames();
 if(released!=2) return 3;
 std::puts("RELEASE_FRAMES callbacks=2; no backing-store API in pinned source");
 return 0;
}
