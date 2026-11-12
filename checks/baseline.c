#include "src/loaders/loader.h"
#include "src/loaders/it.h"
#include <assert.h>
#include <stdio.h>
static void put(unsigned char *b,unsigned *p,unsigned x,unsigned n) {
    for(unsigned i=0;i<n;i++,(*p)++) if((x>>i)&1)b[*p/8]|=1u<<(*p%8);
}
int main(void) {
    unsigned char bytes[1024],tmp[32768],out8[61];short out16[61];int cases=0;
    for(int word=8;word<=16;word+=8)for(int mode=0;mode<2;mode++) {
        memset(bytes,0,sizeof(bytes));unsigned pos=0;unsigned mask=word==8?255:65535;
        for(int i=0;i<61;i++) {int delta=(i%7)-3;put(bytes+2,&pos,(unsigned)delta&mask,word+1);}
        unsigned size=(pos+7)/8;bytes[0]=size&255;bytes[1]=size>>8;
        HIO_HANDLE *h=hio_open_const_mem(bytes,size+2);assert(h);
        int rc=word==8?itsex_decompress8(h,out8,61,tmp,sizeof(tmp),mode):itsex_decompress16(h,out16,61,tmp,sizeof(tmp),mode);assert(rc==0);
        unsigned d1=0,d2=0;
        for(int i=0;i<61;i++) {d1=(d1+(i%7)-3)&mask;d2=(d2+d1)&mask;unsigned actual=word==8?out8[i]:(unsigned short)out16[i];assert(actual==(mode?d2:d1));}
        assert(hio_tell(h)==(long)(size+2));hio_close(h);cases++;
    }
    printf("ITSEX_BASELINE_OK cases=%d modes=214+215 widths=8+16 samples=244\n",cases);
}
