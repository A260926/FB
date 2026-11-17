#include "discrete_mirage.h"
#include <cstdio>
#include <cstring>
static DMirBool start(const void*,DMirAddress node,DMirAddress,DMirTraversal* out) {
    std::memset(out,0,sizeof(*out));out->node[0]=node;out->data[0]=node;return 1;
}
static DMirBool next(const void*,DMirTraversal*,int32_t,DMirTraversal*) {return 0;}
static void cage(float* a,float z) {
    for(int k=0;k<8;k++) {a[3*k]=(k&1)?0.5f:-0.5f;a[3*k+1]=(k&2)?0.5f:-0.5f;a[3*k+2]=z+((k&4)?0.2f:0.0f);}
}
int main() {
    dmir_lookups_initialize();
    DMirGeometry geometry={start,next,nullptr,0};
    DMirFramebuffer* fb=dmir_framebuffer_make(32,24);
    DMirBatcher* b=dmir_batcher_make();DMirRenderer* r=dmir_renderer_make();
    DMirRect vp={0,0,31,23};DMirFrustum fr=DMIR_FRUSTUM_DEFAULT;
    DMirEffects fx={0,0,0,DMIR_SHAPE_RECT};float near_cage[24],far_cage[24];cage(near_cage,2);cage(far_cage,4);
    dmir_batcher_reset(b,vp,fr);
    dmir_batcher_add(b,fb,9,far_cage,&geometry,99,fx);
    dmir_batcher_add(b,fb,7,near_cage,&geometry,42,fx);
    dmir_batcher_sort(b);r->framebuffer=fb;r->batcher=b;r->rect=vp;dmir_renderer_draw(r);
    int n=0;for(int y=0;y<24;y++)for(int x=0;x<32;x++)if(fb->voxel[dmir_pixel_index(fb,x,y)].affine_id>=0)n++;
    DMirAffineInfo* ai=nullptr;uint32_t count=0;dmir_batcher_affine_get(b,&ai,&count);
    auto ref=fb->voxel[dmir_pixel_index(fb,16,12)];
    bool ok=n>0 && ref.affine_id>=0 && ai[ref.affine_id].group==7 && ref.voxel_id==42;
    std::printf("VOXEL_BASELINE visible_pixels=%d affine_count=%u center_voxel=%llu center_group=%u\n",n,count,(unsigned long long)ref.voxel_id,ref.affine_id<0?0:ai[ref.affine_id].group);
    dmir_renderer_free(r);dmir_batcher_free(b);dmir_framebuffer_free(fb);
    if(ok)std::puts("BASELINE_OK CPU cage projection and foreground occlusion");return ok?0:1;
}
