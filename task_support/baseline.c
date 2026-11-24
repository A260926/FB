#include "fixture_io.h"
int main(void){struct fixture f;int i,keys=0,anchors=0;if(fixture_read("tests/fixtures/frames.ogv",&f)||f.count!=15)return 1;
 for(i=3;i<f.count;i++){if(f.bytes[i]&&!(f.data[i][0]&64)){printf("keyframe=%d\n",i-3);keys++;}if(f.gp[i]>=0){printf("anchor frame=%d granule=%lld\n",i-3,(long long)f.gp[i]);anchors++;}}
 printf("theora_baseline packets=%d frames=%d keys=%d anchors=%d\n",f.count,f.count-3,keys,anchors);fixture_free(&f);return keys<2||anchors<1;
}
