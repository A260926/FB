#include <cstdio>
#include <cmath>
#include <cstdint>
static float bits(uint32_t u){ float f; __builtin_memcpy(&f,&u,4); return f; }
int main(){
   // reassociation: (a*b)+(a*c) vs a*(b+c), values where rounding differs
   struct T { float a,b,c; };
   T ts[] = {
      {1e7f, bits(0x3f800001), bits(0x33d6bf95)}, // 1.0000001 + ~1e-7
      {1e6f, bits(0x3f000001), bits(0x3b088889)},
      {bits(0x4b189680), 0.1f, 0.2f}, // large a
      {3.14159265f, bits(0x49742401), bits(0x3b088889)},
      {bits(0x447a0001), 1.0f/3.0f, bits(0x3eaaaaab)},
   };
   for (auto& t : ts) {
      float left=(t.a*t.b)+(t.a*t.c);
      float right=t.a*(t.b+t.c);
      std::printf("a=%.9g b=%.9g c=%.9g left=%.9g right=%.9g differ=%d\n",
                  t.a,t.b,t.c,left,right,left!=right);
   }
}
