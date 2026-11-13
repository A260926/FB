#ifdef __APPLE__
#include <sys/param.h>
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
#include <stdlib.h>
static unsigned sei_count,sps_count,vui;
static unsigned char rbsp[4096];static int pos,nbits;
static unsigned bit(void){if(pos>=nbits)exit(20);unsigned v=(rbsp[pos>>3]>>(7-(pos&7)))&1;pos++;return v;}
static unsigned bits(int n){unsigned v=0;while(n--)v=v*2+bit();return v;}
static unsigned ue(void){int n=0;while(!bit()){if(++n>30)exit(21);}return ((1u<<n)-1)+bits(n);}
static void cb(const unsigned char*p,int n,void*t){(void)t;if((p[0]&31)==6)sei_count++;if((p[0]&31)!=7)return;
 sps_count++;int z=0,k=0;for(int i=1;i<n;i++){if(z==2&&p[i]==3){z=0;continue;}rbsp[k++]=p[i];z=p[i]==0?z+1:0;}
 pos=0;nbits=k*8;bits(24);ue();ue();if(ue()!=2)exit(22);ue();bit();ue();ue();if(!bit())exit(23);bit();if(bit()){ue();ue();ue();ue();}vui=bit();}
int main(void){H264E_create_param_t p={0};p.width=p.height=64;p.gop=4;p.const_input_flag=1;p.vbv_size_bytes=8192;
#if H264E_SVC_API
 p.num_layers=1;
#endif
 int a,b;if(H264E_sizeof(&p,&a,&b))return 2;H264E_persist_t*e=malloc(a);H264E_scratch_t*s=malloc(b);unsigned char*y=malloc(64*64*3/2);if(!e||!s||!y||H264E_init(e,&p))return 3;
 H264E_io_yuv_t in={{y,y+4096,y+5120},{64,32,32}};H264E_run_param_t r={0};r.qp_min=r.qp_max=30;r.desired_frame_bytes=1000;r.nalu_callback=cb;
 int total=0;for(int f=0;f<3;f++){memset(y,128,6144);for(int i=0;i<4096;i++)y[i]=(unsigned char)((i+i/64+f*7)&255);unsigned char*out;int n;if(H264E_encode(e,s,&r,&in,&out,&n)||!out||n<=0)return 4;total+=n;}
 printf("encoded_frames=3 bytes=%d SPS=%u VUI=%u SEI=%u\n",total,sps_count,vui,sei_count);free(y);free(s);free(e);return !(sps_count==1&&vui==0&&sei_count==0&&total>0);}
