#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int LLVMFuzzerTestOneInput(const uint8_t *,size_t);
void task_cov_start(void);
unsigned task_cov_hits(void);
int main(int argc,char **argv) {
 unsigned files=0;task_cov_start();
 for(int i=1;i<argc;i++) {
  FILE *f=fopen(argv[i],"rb");if(!f)return 3;
  if(fseek(f,0,SEEK_END))return 3;long n=ftell(f);if(n<0||n>65536)return 3;rewind(f);
  uint8_t *p=malloc(n?n:1);if(!p)return 3;
  if(fread(p,1,n,f)!=(size_t)n)return 3;fclose(f);
  if(LLVMFuzzerTestOneInput(p,n))return 4;free(p);files++;
 }
 if(files!=2||!task_cov_hits())return 5;
 printf("LLVM_ENTRY_REPLAY files=%u real_guard_hits=%u\n",files,task_cov_hits());return 0;
}
