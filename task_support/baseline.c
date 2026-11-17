#include "src/libttaR.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *aligned(void *p){return (void*)(((uintptr_t)p+15u)&~(uintptr_t)15u);}
int main(void){
  unsigned cases=0;
  for(unsigned sb=1;sb<=3;sb++)for(unsigned ch=1;ch<=3;ch++){
    size_t n=96*ch, margin=libttaR_ttabuf_safety_margin((enum LibTTAr_SampleBytes)sb,ch);
    size_t cap=2*margin+n*16+32, psz=libttaR_codecstate_priv_size(ch);
    int32_t *in=malloc(n*sizeof(*in)),*out=calloc(n,sizeof(*out));
    uint8_t *buf=calloc(cap,1);void *eb=calloc(psz+16,1),*db=calloc(psz+16,1);
    if(!in||!out||!buf||!eb||!db||!margin||!psz)return 2;
    for(size_t i=0;i<n;i++){
      int32_t v=(int32_t)((i*7919u+(i%ch)*997u)%32749u)-16374;
      in[i]=sb==1 ? v%121 : sb==2 ? v : v*257;
    }
    struct LibTTAr_CodecState_User eu=LIBTTAr_CODECSTATE_USER_INIT,du=LIBTTAr_CODECSTATE_USER_INIT;
    struct LibTTAr_EncMisc em={cap,n,n,n,(enum LibTTAr_SampleBytes)sb,ch};
    int er=libttaR_tta_encode(buf,in,aligned(eb),&eu,&em);
    if(er!=LIBTTAr_ERV_OK_DONE||eu.ni32_total!=n||!eu.nbytes_tta_total)return 3;
    size_t len=eu.nbytes_tta_total;
    struct LibTTAr_DecMisc dm={n,len+margin,n,len,n,len,(enum LibTTAr_SampleBytes)sb,ch};
    int dr=libttaR_tta_decode(out,buf,aligned(db),&du,&dm);
    if(dr!=LIBTTAr_DRV_OK_DONE||du.ni32_total!=n||du.nbytes_tta_total!=len)return 4;
    if(memcmp(in,out,n*sizeof(*in))||eu.crc!=du.crc||eu.crc!=libttaR_crc32(buf,len))return 5;
    printf("TTAR_BASELINE bytes=%u channels=%u frames=96 payload=%zu crc=%08x margin=%zu\n",sb,ch,len,eu.crc,margin);
    cases++;free(in);free(out);free(buf);free(eb);free(db);
  }
  printf("TTAR_BASELINE_OK cases=%u\n",cases);return cases==9?0:6;
}
