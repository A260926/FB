#define main fallback_dct_unused_main6
#include "../libde265/fallback-dct.cc"
#undef main
#include <cstdio>
#include <cstdint>
#include <cmath>
int main(){
 int ps[]={63,61,47,45,59,57,43,41,31,29,15,13,7,6,5,27,24,19,18,17,10,9,8,3,2,1};
 int Dv[]={1,-1,4,-4,3,-7,7,-19,14,-16,138,-161,2006,-113,-2344,25,0,0,0,0,0,-695,0,3559,-1288,-10114};
 int16_t D[64]={0}; for(int i=0;i<26;i++)D[ps[i]]=Clip3(-32768,32767,Dv[i]);
 int32_t out[64]; transform_idct_fallback(out,8,D,4,22);
 unsigned char ref[128]; FILE*f=fopen("task_support/fixtures/mono16.yuv","rb"); if(!f)return 2; fread(ref,1,128,f); fclose(f);
 double err=0; for(int y=0;y<8;y++){for(int x=0;x<8;x++){uint16_t v=ref[2*(y*64+x)]|(ref[2*(y*64+x)+1]<<8); double t=v-32768; double e=out[y*8+x]-t;err+=e*e;}}
 printf("rmse %.1f\n",sqrt(err/64));
 for(int y=0;y<8;y++){for(int x=0;x<8;x++)printf("%6d",out[y*8+x]);printf("\n");}
}
