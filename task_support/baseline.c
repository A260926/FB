#include "task_support/fixture.h"
#include <assert.h>
#include <stdio.h>
static void check_palette(const float *p) {
    double z=0.25*sqrt(0.5),w=0.75+0.25*sqrt(0.5);
    double norm=z*z+w*w,c=(w*w-z*z)/norm,s=2*w*z/norm;
    assert(fabs(p[3]-2)<0.0001);
    /* tip model = T(3,0,0)*R; inverse bind = T(-1,0,0). */
    double x=p[12]*2+p[15],y=p[16]*2+p[19];
    assert(fabs(x-(3+c))<0.0002 && fabs(y-s)<0.0002);
}
int main(void) {
    task_fixture f; mrw_mem_req sr,orr; mrw_dispatch ds,dd;
    float ta[9],tb[9],weights[9];
    assert(!task_fixture_init(&f));
    assert(mrw_batch_blend_clips_to_palette_requirements(2,9,MRW_PALETTE_F32,&sr,&orr)==MRW_OK);
    float *out=mrw_authoring_alloc(orr.size);
    void *scratch=mrw_authoring_alloc(sr.size);
    assert(out && scratch);
    for(int i=0;i<9;++i) ta[i]=tb[i]=0.5f,weights[i]=0.25f;
    assert(mrw_dispatch_scalar(&ds)==MRW_OK);
    assert(mrw_batch_blend_clips_to_palette(&ds,&f.skel,&f.a,&f.b,ta,tb,weights,0,9,out,orr.size,scratch,sr.size)==MRW_OK);
    for(int i=0;i<9;++i) check_palette(out+i*24);
    puts("MARROW_BASELINE scalar_instances=9 palette_verified=1");
    assert(mrw_dispatch_detect(&dd)==MRW_OK);
    assert(mrw_batch_blend_clips_to_palette(&dd,&f.skel,&f.a,&f.b,ta,tb,weights,0,9,out,orr.size,scratch,sr.size)==MRW_OK);
    for(int i=0;i<9;++i) check_palette(out+i*24);
    printf("MARROW_BASELINE detected_backend=%u palette_verified=1\n",(unsigned)dd.backend);
    {
        mrw_trs la[2],lb[2],lo[2]; float model[24],p[24];
        assert(mrw_clip_sample_local(&f.a,.5f,la,2)==MRW_OK);
        assert(mrw_clip_sample_local(&f.b,.5f,lb,2)==MRW_OK);
        assert(mrw_pose_blend(la,lb,.25f,0,lo,2,2)==MRW_OK);
        assert(mrw_local_to_model(&f.skel,lo,model,2)==MRW_OK);
        assert(mrw_model_to_palette(&f.skel,model,p,2)==MRW_OK);
        check_palette(p);
    }
    puts("CLIP_SAMPLING codec0_codec1=1 blend_and_skin=1");
    mrw_authoring_free(out);mrw_authoring_free(scratch);mrw_authoring_free(f.bytes);
    return 0;
}
