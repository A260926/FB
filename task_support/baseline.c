#include "marrow.h"
#include "tests/mrw_build.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
static const float I[12]={1,0,0,0,0,1,0,0,0,0,1,0};
int main(void){
    uint16_t parents[2]={0xffff,0};const char *names[2]={"root","arm"};
    float rest[20]={0,0,0,1,0,0,0,1,1,1, 0,0,0,1,1,0,0,1,1,1};
    float ib[24];memcpy(ib,I,sizeof I);memcpy(ib+12,I,sizeof I);ib[15]=-1;
    float sa[40],sb[40];
    for(int j=0;j<2;j++)for(int k=0;k<2;k++){
        memcpy(sa+(j*2+k)*10,rest+j*10,10*sizeof(float));
        memcpy(sb+(j*2+k)*10,rest+j*10,10*sizeof(float));
    }
    for(int k=0;k<2;k++){sb[k*10+2]=.707106781f;sb[k*10+3]=.707106781f;}
    mrw_skel sk={2,parents,rest,ib,names};mrw_clip clips[2]={{1,2,0,sa,NULL,0},{1,2,0,sb,NULL,0}};
    uint8_t *data=NULL;size_t bytes=mrw_build(&sk,clips,2,NULL,&data);mrw_blob blob;
    if(mrw_blob_open(data,bytes,&blob)!=MRW_OK)return 1;
    mrw_skeleton_view sv;mrw_clip_view ca,cb;
    if(mrw_blob_skeleton(&blob,&sv)!=MRW_OK||mrw_clip_view_at(&blob,1,&ca)!=MRW_OK||mrw_clip_view_at(&blob,2,&cb)!=MRW_OK)return 2;
    mrw_trs a[2],b[2],out[2];float mask[2]={1,0},model[24],palette[24];
    if(mrw_clip_sample_local(&ca,.25f,a,2)||mrw_clip_sample_local(&cb,.75f,b,2)||mrw_pose_blend(a,b,1,mask,out,2,2)||mrw_local_to_model(&sv,out,model,2)||mrw_model_to_palette(&sv,model,palette,2))return 3;
    // The old local-space API intentionally remains unchanged by the requested feature.
    float vx=2*palette[12]+palette[15],vy=2*palette[16]+palette[19];
    if(fabsf(vx)>1e-5f||fabsf(vy-2)>1e-5f)return 4;
    mrw_mem_req sr,pr;if(mrw_batch_blend_clips_to_palette_requirements(2,9,MRW_PALETTE_F32,&sr,&pr))return 5;
    void *scratch=mrw_alloc64(sr.size);float *pal=(float*)mrw_alloc64(pr.size);
    float ta[9],tb[9],w[9];for(int i=0;i<9;i++){ta[i]=.25f;tb[i]=.75f;w[i]=1;}
    mrw_dispatch d;mrw_dispatch_scalar(&d);
    if(mrw_batch_blend_clips_to_palette(&d,&sv,&ca,&cb,ta,tb,w,mask,9,pal,pr.size,scratch,sr.size))return 6;
    for(int i=0;i<9;i++)for(int k=0;k<24;k++)if(fabsf(pal[i*24+k]-palette[k])>1e-5f)return 7;
    printf("BASELINE_LOCAL joints=%u instances=9 skinned_vertex=%.6f,%.6f scratch=%zu\n",sv.joint_count,vx,vy,sr.size);
    mrw_free(scratch);mrw_free((uint8_t*)pal);mrw_free(data);return 0;
}
