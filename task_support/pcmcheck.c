
#include <stdio.h>
#include <stdlib.h>
int main(int n,char**v){
 if(n!=2)return 2;FILE*f=fopen(v[1],"rb");if(!f)return 3;
 long samples=0;int nz=0,diff=0,last=0;
 for(;;){int a=fgetc(f);if(a==EOF)break;int b=fgetc(f);if(b==EOF)return 4;int x=a+(b<<8);if(x>=32768)x-=65536;
 if(x>16000||x<-16000)return 5;if(x)nz=1;
 if(samples%2&&x!=last)diff=1;last=x;samples++;}
 if(ferror(f)||fclose(f)||samples<4||samples%2||!nz||!diff)return 6;
 printf("LIMITED_PCM_OK frames=%ld\n",samples/2);return 0;
}
