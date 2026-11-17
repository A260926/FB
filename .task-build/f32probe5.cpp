#include <cstdio>
#include <cmath>
#include <cstdint>
static float bits(uint32_t u){ float f; __builtin_memcpy(&f,&u,4); return f; }
int main(){
   // use slightly mismatched magnitudes so one grouping loses a term
   struct T { float a,b,c; };
   T ts[] = {
      {bits(0x4b189680), bits(0x3f800001), bits(0xbf000000)}, // 1e7 + 1.0000002 - 0.5
      {16777216.0f, 1.0f, -0.99999994f},
      {bits(0x4b000000), bits(0x3f000001), bits(0x3e800000)}, // 8388608 + .50000006 + .25
      {67108864.0f, 1.5f, -1.25f},
      {33554432.0f, bits(0x3f800001), bits(0xbf800000)},
   };
   for (auto& t : ts) {
      float left=(t.a+t.b)+t.c;
      float right=t.a+(t.b+t.c);
      std::printf("a=%.9g b=%.9g c=%.9g left=%.9g right=%.9g differ=%d\n",
                  t.a,t.b,t.c,left,right,left!=right);
   }
}
