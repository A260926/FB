#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void check(int ok,const char *s){if(!ok){fprintf(stderr,"FAIL %s\n",s);exit(1);}}
int main(int argc,char **argv){
 FILE *f; int32_t y[2048]; size_t n; unsigned i;
 check(argc==3,"arguments");
 if(!strcmp(argv[1],"generate")){
  f=fopen(argv[2],"wb");check(f!=0,"open input");
  for(i=0;i<2048;i++)y[i]=i%2?-2000*65536:1000*65536;
  check(fwrite(y,sizeof(*y),2048,f)==2048,"write input");fclose(f);return 0;
 }
 f=fopen(argv[2],"rb");check(f!=0,"open result");n=fread(y,sizeof(*y),2048,f);check(feof(f),"finite result");fclose(f);
 check(n==1984,"two splice frame count");
 for(i=0;i<n;i++)check(y[i]==(i%2?-2000*65536:1000*65536),"constant channels preserved");
 printf("SOX_BASELINE_OK frames=%lu channels=2 splices=2\n",(unsigned long)n/2);return 0;
}
