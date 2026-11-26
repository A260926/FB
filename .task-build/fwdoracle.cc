#define main fallback_dct_unused_main7
#include "../libde265/fallback-dct.cc"
#undef main
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

void fwd8(const int32_t* r,int BD,int outC[64]){
 int n=8, fact=4; int shift1=Log2(n)+6+BD-15, shift2=Log2(n)+6;
 int rnd1=1<<(shift1-1),rnd2=1<<(shift2-1);
 int g[64];
 for(int c=0;c<n;c++) for(int i=0;i<n;i++){int64_t s=0;for(int j=0;j<n;j++)s+=mat_dct[fact*i][j]*r[c+j*n]; g[c+i*n]=(int)((s+rnd1)>>shift1);}
 for(int y=0;y<n;y++) for(int i=0;i<n;i++){int64_t s=0;for(int j=0;j<n;j++)s+=mat_dct[fact*i][j]*g[y*n+j]; outC[y*n+i]=(int)((s+rnd2)>>shift2);}
}
int main(){
 // current known C and inverse residual
 int16_t C[64]={0}; int ps[]={63,61,47,45,59,57,43,41,31,29,15,13,7,6,5,27,24,19,18,17,10,9,8,3,2,1}; int vs[]={2,-2,6,-7,4,-11,11,-31,23,-26,220,-258,2083,-8,-33,1,-1,-1,1,1,-1,1,-7,1,2,-8}; for(int i=0;i<26;i++)C[ps[i]]=vs[i];
 int16_t D[64]; for(int i=0;i<64;i++)D[i]=Clip3(-32768,32767,((int64_t)C[i]*640+256)>>10);
 int32_t cur[64]; transform_idct_fallback(cur,8,D,4,22);
 int CC[64]; fwd8(cur,16,CC);
 printf("recovered current vs known\n"); for(int y=0;y<8;y++){for(int x=0;x<8;x++){int p=y*8+x; if(C[p]||CC[p])printf("p%02d k%5d f%5d ",p,C[p],CC[p]);}printf("\n");}
 unsigned char ref[128]; FILE*f=fopen("task_support/fixtures/mono16.yuv","rb");fread(ref,1,128,f);fclose(f);
 int32_t rr[64]; for(int i=0;i<64;i++){uint16_t v=ref[2*i]|ref[2*i+1]<<8;rr[i]=(int32_t)v-32768;}
 int RC[64]; fwd8(rr,16,RC);
 printf("reference forward coeff\n"); for(int y=0;y<8;y++){for(int x=0;x<8;x++)printf("%6d",RC[y*8+x]);printf("\n");}
}
