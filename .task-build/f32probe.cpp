#include <cstdio>
#include <cmath>
int main(){
   // contraction: (a*b)+c stepwise vs fma
   float cand[][3] = {
      {1.0f/3.0f, 3.0f, 1.0f},
      {1.0f/7.0f, 7.0f, 1.0f},
      {16777216.0f, 1.0000001f, 1.0f},
      {1.0f/3.0f, 3.0f, 1e-8f},
      {0.1f, 0.2f, 0.3f},
      {1.0f/3.0f, 3.0f, -1e-8f},
   };
   for (auto& t : cand) {
      float step = (t[0]*t[1]) + t[2];
      float fm = fmaf(t[0], t[1], t[2]);
      std::printf("contract a=%.9g b=%.9g c=%.9g step=%.9g fma=%.9g differ=%d\n",
                  t[0],t[1],t[2], step, fm, step!=fm);
   }
   // reassociation: (a*b)+(a*c) vs a*(b+c)
   float r[][3] = {
      {1e7f, 1.0f, 1e-7f},
      {1e8f, 1.0f/3.0f, 1.0f/3.0f},
      {3.1415927f, 1e6f, 1.0f/3.0f},
      {1.0f/3.0f, 1e7f, 1e-7f},
   };
   for (auto& t : r) {
      float left = (t[0]*t[1]) + (t[0]*t[2]);
      float right = t[0]*(t[1]+t[2]);
      std::printf("reassoc a=%.9g b=%.9g c=%.9g left=%.9g right=%.9g differ=%d\n",
                  t[0],t[1],t[2], left, right, left!=right);
   }
}
