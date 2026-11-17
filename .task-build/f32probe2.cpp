#include <cstdio>
#include <cmath>
#include <cstdint>
static float bits(uint32_t u){ float f; __builtin_memcpy(&f,&u,4); return f; }
int main(){
   // classic contraction example: product rounds, tiny addend vanishes stepwise but survives fma
   float as[] = {bits(0x3fb504f3), 1.0f/3.0f, bits(0x3eaaaaab)};
   float bs[] = {bits(0x3f111112), 3.0f, bits(0x3f800001)};
   for (int i=0;i<3;i++){
      float a=as[i], b=bs[i];
      float p=a*b;
      // choose c as the successor of p in float, scaled so fma keeps it
      uint32_t pu; __builtin_memcpy(&pu,&p,4);
      float c=bits(pu+1);
      float step=p+c;
      float fm=fmaf(a,b,c);
      std::printf("contract%d p=%.9g c=%.9g step=%.9g fma=%.9g differ=%d\n",i,p,c,step,fm,step!=fm);
   }
   // reassociation: two unequal products whose sum changes when factoring
   float a=bits(0x4b000001); // 8388608 * 1.0000001
   float x=bits(0x3f800002); // ~1.0000002
   float y=bits(0x33d6bf95); // ~1e-7
   float left=(a*x)+(a*y);
   float right=a*(x+y);
   std::printf("reassoc left=%.9g right=%.9g differ=%d\n",left,right,left!=right);
}
