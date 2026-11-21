#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int token(FILE *f,char *s){int c;do{c=fgetc(f);if(c=='#'){while(c!='\n'&&c!=EOF)c=fgetc(f);}}while(c==' '||c=='\n'||c=='\r'||c=='\t');if(c==EOF)return 0;int n=0;do{if(n<63)s[n++]=(char)c;c=fgetc(f);}while(c!=EOF&&c!=' '&&c!='\n'&&c!='\r'&&c!='\t');s[n]=0;return 1;}
int main(int argc,char**argv){char s[64];if(argc!=2)return 1;FILE*f=fopen(argv[1],"rb");if(!f)return 2;if(!token(f,s)||strcmp(s,"P6"))return 3;if(!token(f,s))return 4;long w=strtol(s,0,10);if(!token(f,s))return 4;long h=strtol(s,0,10);if(w<1||h<1||w>4096||h>4096)return 5;if(!token(f,s)||strcmp(s,"255"))return 6;unsigned long sum=0;for(long i=0;i<w*h*3;i++){int c=fgetc(f);if(c==EOF)return 7;sum+=(unsigned)c;}if(fgetc(f)!=EOF||!sum)return 8;fclose(f);printf("PPM_OK width=%ld height=%ld bytes=%ld sum=%lu\n",w,h,w*h*3,sum);return 0;}
