#include <cstdio>
#include <cstdlib>
int main(int n,char**v){if(n!=3)return 1;FILE*f=fopen(v[1],"wb");if(!f)return 2;int planes=atoi(v[2]);
for(int frame=0;frame<3;++frame)for(int c=0;c<planes;++c)for(int y=0;y<64;++y)for(int x=0;x<64;++x){
unsigned a=((x*2311+y*673+frame*7013+c*17177)^(((x/4+y/8+frame)&1)?65535:0))&65535;
if(fputc(a&255,f)==EOF||fputc(a>>8,f)==EOF)return 3;}
return fclose(f);}
