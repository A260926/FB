#include "libhfuzz/covmerge.h"
#include "libhfuzz/instrument.h"
#include <assert.h>
#include <sys/mman.h>
extern void __sanitizer_cov_8bit_counters_init(char*,char*);
int main(void){
 size_t n=hfCovMergeSize();assert(n>0);
 void *m=mmap(0,n,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANON,-1,0);assert(m!=MAP_FAILED);
 assert(hfCovMergeInit(m,n,1));assert(hfCovMergeAttach(m,n));
 char a[1]={0};__sanitizer_cov_8bit_counters_init(a,a+1);
 assert(hfCovMergeBind(a,a+1,1,2));uint8_t out=99;
 assert(hfCovMergeRead(1,2,0,&out) && out==0);
 assert(hfCovMergeBegin()==1);a[0]=3;instrument8BitCountersCount();
 assert(hfCovMergeRead(1,2,0,&out) && out==3);assert(a[0]==0);
 hfCovMergeDetach();assert(munmap(m,n)==0);
}
