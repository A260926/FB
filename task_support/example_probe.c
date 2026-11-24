#include <stdio.h>
#include <string.h>
int main(int argc,char**argv){int i;unsigned char h[13];if(argc!=5)return 2;
 for(i=1;i<5;i++){FILE*f=fopen(argv[i],"rb");if(!f)return 3;
 if(fread(h,1,13,f)!=13){fclose(f);return 4;}if(fclose(f))return 5;
 if(memcmp(h,"FLV",3)||h[3]!=1||h[8]!=9||h[9]||h[10]||h[11]||h[12])return 6;}
 puts("EXAMPLE_FRESH_FLV_PASS outputs=4");return 0;}
