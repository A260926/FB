#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include "marrow.h"
#include "tools/authoring/mrw_authoring.h"
#include <string.h>
#include <math.h>
typedef struct task_fixture {
    uint8_t *bytes; size_t size; mrw_blob blob;
    mrw_skeleton_view skel; mrw_clip_view a,b;
} task_fixture;
static int task_fixture_init(task_fixture *f) {
    const uint16_t parent[2]={0xffff,0};
    const char *names[2]={"root","tip"};
    float rest[20]={0,0,0,1,0,0,0,1,1,1, 0,0,0,1,1,0,0,1,1,1};
    const float ib[24]={1,0,0,0,0,1,0,0,0,0,1,0, 1,0,0,-1,0,1,0,0,0,0,1,0};
    float a[40],b[40];
    mrw_skel s={2,parent,rest,ib,names};
    mrw_clip clips[2];
    memset(f,0,sizeof *f);
    memset(clips,0,sizeof clips);
    for(unsigned j=0;j<2;++j) for(unsigned k=0;k<2;++k) {
        float *pa=a+(j*2+k)*10,*pb=b+(j*2+k)*10;
        memcpy(pa,rest+j*10,40); memcpy(pb,rest+j*10,40);
        if(j==0) {pa[4]=2.0f*k;pb[4]=4.0f+2.0f*k;}
        else {pb[2]=sqrtf(0.5f);pb[3]=sqrtf(0.5f);}
    }
    clips[0].fps=clips[1].fps=1;
    clips[0].sample_count=clips[1].sample_count=2;
    clips[0].flags=MRW_CLIP_LOOPING; clips[0].samples=a;
    clips[1].samples=b; clips[1].codec=1;
    if(mrw_authoring_build(&s,clips,2,0,&f->bytes,&f->size)!=MRW_OK) return 1;
    if(mrw_blob_open(f->bytes,f->size,&f->blob)!=MRW_OK) return 2;
    if(mrw_skeleton_view_at(&f->blob,0,&f->skel)!=MRW_OK) return 3;
    if(mrw_clip_view_at(&f->blob,1,&f->a)!=MRW_OK) return 4;
    if(mrw_clip_view_at(&f->blob,2,&f->b)!=MRW_OK) return 5;
    return 0;
}
#endif
