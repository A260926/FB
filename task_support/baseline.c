#include "faac.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static unsigned check_frame(const uint8_t*p,unsigned n){assert(n>=7&&p[0]==255&&(p[1]&0xf6)==0xf0);unsigned size=((p[3]&3)<<11)|(p[4]<<3)|(p[5]>>5);assert(size==n);unsigned ch=((p[2]&1)<<2)|(p[3]>>6);assert(ch==2);return n;}
int main(void){
 faac_params p;assert(faac_params_init(&p,sizeof(p))==FAAC_OK);p.sample_rate=48000;p.num_channels=2;p.object_type=FAAC_OBJ_LOW;p.input_format=FAAC_INPUT_16BIT;p.output_format=FAAC_STREAM_ADTS;p.quant_quality=100;
 faac_encoder*e=NULL;assert(faac_encoder_open(&p,&e)==FAAC_OK&&e);faac_encoder_info info={0};info.struct_size=sizeof(info);assert(faac_encoder_get_info(e,&info)==FAAC_OK&&info.frame_samples==1024);
 const uint8_t *asc;uint32_t alen;assert(faac_encoder_asc(e,&asc,&alen)==FAAC_OK&&alen==2);assert(((asc[1]>>3)&15)==2);
 uint8_t*out=malloc(info.max_output_bytes);int16_t*in=malloc(info.frame_samples*2*sizeof(*in));assert(in&&out);unsigned frames=0,bytes=0;
 for(unsigned f=0;f<6;f++){for(unsigned i=0;i<info.frame_samples;i++){in[2*i]=(int16_t)((i*97+f*113)%20000-10000);in[2*i+1]=(int16_t)((i*53+f*211)%16000-8000);}uint32_t n=0;assert(faac_encoder_encode(e,in,info.frame_samples*2,out,info.max_output_bytes,&n)==FAAC_OK);if(n){bytes+=check_frame(out,n);frames++;}}
 for(int i=0;i<20;i++){uint32_t n=0;assert(faac_encoder_encode(e,NULL,0,out,info.max_output_bytes,&n)==FAAC_OK);if(!n)break;bytes+=check_frame(out,n);frames++;}
 assert(frames>=6&&bytes>42);printf("FAAC_BASE stereo_frames=%u bytes=%u ASC_LC_48000_CH2\n",frames,bytes);free(in);free(out);assert(faac_encoder_close(&e)==FAAC_OK&&e==NULL);puts("FAAC_BASE_OK");return 0;
}
