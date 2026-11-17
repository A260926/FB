#include <cstdio>
#include <cmath>
#include <cstdint>
static float bits(uint32_t u){ float f; __builtin_memcpy(&f,&u,4); return f; }
int main(){
   // catastrophic-cancellation style: (a+b)+c vs a+(b+c)
   struct T { float a,b,c; };
   T ts[] = {
      {1.0f, bits(0x3e800001), -bits(0x3e800000)},
      {bits(0x4b189680), 1.0f, -1.0f},
      {1e8f, 1.0f/3.0f, -bits(0x3eaaaaab)},
      {bits(0x447a0000), bits(0x3b088889), -bits(0x3b088888)},
      {12345.678f, 0.0001f, -0.0001f},
   };
   for (auto& t : ts) {
      float left=(t.a+t.b)+t.c;
      float right=t.a+(t.b+t.c);
      std::printf("a=%.9g b=%.9g c=%.9g left=%.9g right=%.9g differ=%d\n",
                  t.a,t.b,t.c,left,right,left!=right);
   }
}
