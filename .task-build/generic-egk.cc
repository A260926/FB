#define main fallback_dct_unused_main5
#include "../libde265/fallback-dct.cc"
#undef main
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <cstdint>

struct Rec{int p,k,code;};
int main(){
 std::ifstream bf(".task-build/bits.txt"); std::string line; std::vector<Rec> recs;
 while(std::getline(bf,line)){
  int p,k,c; if(sscanf(line.c_str(),"REM prefix=%d k=%d code=%d",&p,&k,&c)==3) recs.push_back({p,k,c});
 }
 std::vector<int> bits;
 for(auto r:recs){
  for(int i=0;i<r.p;i++) bits.push_back(1);
  bits.push_back(0);
  int n=(r.p<=3?r.k:r.p-3+r.k);
  for(int b=n-1;b>=0;b--) bits.push_back((r.code>>b)&1);
 }
 size_t at=0;
 auto get=[&](){return bits[at++];};
 // first TU max entries: pos,base,sign
 struct E{int pos,base,sign;};
 std::vector<E> es={
 {61,2,1},{47,2,0},{45,2,1},{59,3,0},{57,2,1},{43,2,0},{41,2,1},
 {31,3,0},{29,2,1},{15,2,0},{13,2,1},{7,2,0},{6,2,1},{5,2,0},
 {8,3,1},{3,1,0},{2,1,0},{1,1,0}};
 // fixed nonmax coefficients pos,value
 int C[64]={0};
 C[63]=2; C[27]=1;C[24]=-1;C[19]=-1;C[18]=1;C[17]=1;C[10]=1;C[9]=1;
 // Note positions duplicated across scan? absolute unique yes.
 for(size_t j=0;j<es.size();j++){
  int k=recs[j].k;
  int p=0; while(get()) p++;
  uint32_t base=0; int n=k; for(int i=0;i<p;i++){base += 1u<<n; n++;}
  uint32_t suf=0; for(int i=0;i<n;i++) suf=(suf<<1)|get();
  int rem=base+suf;
  int val=es[j].base+rem; if(es[j].sign) val=-val;
  C[es[j].pos]=val;
  printf("j%02zu pos%02d p%d k%d rem%d val%d\n",j,es[j].pos,p,k,rem,val);
 }
 // sign hiding last flips
 C[1]=-C[1];
 int16_t D[64]; for(int i=0;i<64;i++) D[i]=Clip3(-32768,32767,((int64_t)C[i]*640+256)>>10);
 int32_t out[64]; transform_idct_8x8_fallback(out,D,4,15);
 unsigned char ref[128]; FILE*f=fopen("task_support/fixtures/mono16.yuv","rb"); fread(ref,1,128,f); fclose(f);
 double err=0; for(int y=0;y<8;y++){for(int x=0;x<8;x++){uint16_t v=ref[2*(y*64+x)]|ref[2*(y*64+x)+1]<<8; double t=v-32768; double e=out[y*8+x]-t;err+=e*e; if(y==0)printf("%6d/%6.0f ",out[y*8+x],t);} if(y==0)printf("\n");}
 printf("rmse %.1f consumed %zu available %zu\n",sqrt(err/64),at,bits.size());
}
