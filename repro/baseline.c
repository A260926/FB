#include "openaptx.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t hash(const unsigned char *p,size_t n) {
  uint64_t h=UINT64_C(14695981039346656037);
  while(n--) h=(h^*p++)*UINT64_C(1099511628211);
  return h;
}
int main(void) {
  unsigned char pcm[12288],encoded[4096],decoded[20000];
  size_t i,n,w,tail,used;int hd,nonzero;
  for(i=0;i<sizeof pcm;i++) pcm[i]=(unsigned char)((i*7919u+0xabcdefu)>>7);
  for(hd=0;hd<2;hd++) {
    struct aptx_context *e=aptx_init(hd),*d=aptx_init(hd);
    assert(e&&d);
    used=aptx_encode(e,pcm,sizeof pcm,encoded,sizeof encoded,&n);
    assert(used==sizeof pcm && n==(size_t)(hd?3072:2048));
    assert(aptx_encode_finish(e,encoded+n,sizeof encoded-n,&tail));
    n+=tail;
    assert(n==(size_t)(hd?3210:2140));
    used=aptx_decode(d,encoded,n,decoded,sizeof decoded,&w);
    assert(used==n && w==12300);
    nonzero=0;for(i=0;i<w;i++) nonzero |= decoded[i];assert(nonzero);
    printf("OPENAPTX_BASELINE_OK hd=%d bytes=%zu pcm=%zu stream=%016llx decoded=%016llx\n",hd,n,w,(unsigned long long)hash(encoded,n),(unsigned long long)hash(decoded,w));
    assert(hash(encoded,n)==(hd?UINT64_C(0xb1b5e274af244790):UINT64_C(0xea6b2766d4543ef8)));
    assert(hash(decoded,w)==(hd?UINT64_C(0xf0aab92e83f6e33c):UINT64_C(0x2804d56b549324c9)));
    aptx_finish(e);aptx_finish(d);
  }
  return 0;
}
