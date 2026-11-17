#include "demos.h"
#include <cmath>
#include <cstdio>
simdfloat iTime(0.25f);
vec3 iMouse(0.0f);
int main() {
    float xs[4]={0,200,400,799}, ys[4]={225,225,225,225};
    vec4 pixel = New::mainImage(vec2(simd_load_float(xs),simd_load_float(ys)));
    float rr[SIMDWIDTH],gg[SIMDWIDTH],bb[SIMDWIDTH],aa[SIMDWIDTH];
    simd_store_float(rr,pixel.x); simd_store_float(gg,pixel.y);
    simd_store_float(bb,pixel.z); simd_store_float(aa,pixel.w);
    for (int i=0;i<SIMDWIDTH;++i) {
        float r=0.5f+0.5f*std::cos(0.25f+xs[i]/800.0f);
        float g=0.5f+0.5f*std::cos(0.25f+ys[i]/450.0f+2.0f);
        float b=0.5f+0.5f*std::cos(0.25f+xs[i]/800.0f+4.0f);
        if (std::abs(rr[i]-r)>0.000002f || std::abs(gg[i]-g)>0.000002f ||
            std::abs(bb[i]-b)>0.000002f || aa[i]!=1.0f) return 2;
        std::printf("PIXEL %d %.6f %.6f %.6f %.1f\n",i,rr[i],gg[i],bb[i],aa[i]);
    }
    std::printf("BASELINE_SHADER_OK lanes=%d\n",SIMDWIDTH);
}
