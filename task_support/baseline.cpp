#include <discrete_mirage.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

static void need(bool v, const char* why) {
    if (!v) { std::fprintf(stderr,"BASELINE_FAIL %s\n",why); std::exit(2); }
}
static DMirBool start(const void*, DMirAddress, DMirAddress, DMirTraversal* t) {
    t->mask[0]=255; t->node[0]=0; t->data[0]=1; return 1;
}
static DMirBool next(const void*, DMirTraversal*, int32_t, DMirTraversal* t) {
    for (int i=0;i<8;++i) { t->mask[i]=0; t->node[i]=i+1; t->data[i]=100+i; }
    return 1;
}
static void cage(float* a, float z) {
    for(int i=0;i<8;++i) { a[i*3]=(i&1)?.7f:-.7f;
        a[i*3+1]=(i&2)?.7f:-.7f; a[i*3+2]=z+((i&4)?.8f:0); }
}
int main() {
    dmir_lookups_initialize();
    DMirFramebuffer* f=dmir_framebuffer_make(64,48);
    DMirBatcher* b=dmir_batcher_make();
    DMirRenderer* r=dmir_renderer_make();
    need(f&&b&&r,"allocation");
    DMirGeometry g={start,next,nullptr,1};
    DMirEffects e={1,0,0,DMIR_SHAPE_RECT};
    DMirRect viewport={0,0,63,47};
    uint64_t checksum=0;
    for(int mode=0;mode<2;++mode) {
        dmir_framebuffer_clear(f);
        DMirFrustum fr=DMIR_FRUSTUM_DEFAULT;
        fr.min_depth=.1f; fr.max_depth=20; fr.perspective=float(mode);
        dmir_batcher_reset(b,viewport,fr);
        float farc[24],nearc[24]; cage(farc,4); cage(nearc,2);
        dmir_batcher_add(b,f,9,farc,&g,0,e);
        dmir_batcher_add(b,f,3,nearc,&g,0,e);
        dmir_batcher_sort(b);
        r->framebuffer=f; r->batcher=b; r->rect=viewport;
        dmir_renderer_draw(r);
        DMirAffineInfo* info=nullptr; uint32_t count=0;
        dmir_batcher_affine_get(b,&info,&count);
        unsigned hits=0,nearhits=0;
        for(int y=0;y<48;++y) for(int x=0;x<64;++x) {
            int i=dmir_pixel_index(f,x,y);
            if(f->depth[i]==DMIR_MAX_DEPTH) continue;
            ++hits; need(f->depth[i]>=0,"valid depth");
            int id=f->voxel[i].affine_id;
            need(id>=0&&uint32_t(id)<count,"affine reference");
            need(f->voxel[i].voxel_id==1||(f->voxel[i].voxel_id>=100&&f->voxel[i].voxel_id<108),"native LOD reference");
            if(info[id].group==3) ++nearhits;
            checksum=checksum*131+uint64_t(f->depth[i])+f->voxel[i].voxel_id;
        }
        need(hits>20&&nearhits==hits,"near instance wins overlapping projections");
        need(f->stats[DMIR_NODES_ORTHO]>0,"native orthographic subtree used");
        std::printf("RENDER mode=%s hits=%u near=%u ortho=%u\n",mode?"perspective":"orthographic",hits,nearhits,f->stats[DMIR_NODES_ORTHO]);
    }
    dmir_framebuffer_clear(f);
    for(int y=0;y<48;++y) for(int x=0;x<64;++x)
        need(f->depth[dmir_pixel_index(f,x,y)]==DMIR_MAX_DEPTH,"clear");
    dmir_framebuffer_resize(f,31,19); dmir_framebuffer_clear(f);
    need(f->size_x==31&&f->size_y==19,"resize");
    dmir_renderer_free(r); dmir_batcher_free(b); dmir_framebuffer_free(f);
    std::printf("CPU_RENDER_BASELINE_OK checksum=%llu\n",(unsigned long long)checksum);
}
