#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
static float bits(uint32_t u){ float f; memcpy(&f,&u,4); return f; }
int main(){
   // search for (a*b)+(a*c) != a*(b+c)
   uint32_t seeds[] = {0x3f800000,0x3f000000,0x40000000,0x4b189680,0x447a0000,0x3e800000,0x3b088889,0x33d6bf95,0x49742400,0x3f800001,0x3f000001,0x3eaaaaab};
   int found=0;
   for (uint32_t au : seeds) {
      float a=bits(au);
      for (uint32_t bu : seeds) {
         float b=bits(bu);
         for (uint32_t cu : seeds) {
            float c=bits(cu);
            float l=(a*b)+(a*c), r=a*(b+c);
            if (l!=r && found<5) {
               std::printf("a=%.9g b=%.9g c=%.9g step=%.9g factored=%.9g\n",a,b,c,l,r);
               found++;
            }
         }
      }
   }
   if (!found) std::puts("none in seed set");
}
