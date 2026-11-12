#include "src/loaders/loader.h"
#include "src/loaders/it.h"
#include "src/md5.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    const char *paths[2]={"test-dev/data/it-sample-8bit.raw","test-dev/data/it-sample-16bit.raw"};
    const char *expected[2]={"299c9144ae2349b90b430aafde8d799a","1e2395653f9bd7838006572d8fcdb646"};
    union { short words[5000]; unsigned char bytes[10000]; } out;
    unsigned char tmp[32768],digest[16],canonical[10000];char hex[33];
    for(int b=0;b<2;b++) {
        HIO_HANDLE *h=hio_open(paths[b],"rb"); assert(h);int n=b?4646:4879;
        int r=b?itsex_decompress16(h,out.words,n,tmp,sizeof(tmp),0):itsex_decompress8(h,out.bytes,n,tmp,sizeof(tmp),0); assert(!r);hio_close(h);
        if(b)for(int i=0;i<n;i++){canonical[2*i]=(unsigned short)out.words[i]&255;canonical[2*i+1]=(unsigned short)out.words[i]>>8;}
        else memcpy(canonical,out.bytes,n);
        MD5_CTX ctx;MD5Init(&ctx);MD5Update(&ctx,canonical,n*(b+1));MD5Final(digest,&ctx);
        for(int i=0;i<16;i++)sprintf(hex+2*i,"%02x",digest[i]);
        assert(!strcmp(hex,expected[b]));printf("ITSEX_OLD_SAMPLE_%d_OK md5=%s\n",b?16:8,hex);
    }
}
