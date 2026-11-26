#define main fallback_dct_unused_main3
#include "../libde265/fallback-dct.cc"
#undef main
#include <cstdio>
int main(){
 int16_t C[64]={0};
 auto set=[&](int p,int v){C[p]=v;};
 set(63,2);set(61,-2);set(47,6);set(45,-7);set(59,4);set(57,-11);set(43,11);set(41,-31);set(31,23);set(29,-26);set(15,220);set(13,-258);set(7,2083);set(6,-8);set(5,-33);set(27,1);set(24,-1);set(19,-1);set(18,1);set(17,1);set(10,1);set(9,-1);set(8,-7);set(3,1);set(2,2);set(1,-8);
 // dequant as library
 int16_t D[64]; for(int i=0;i<64;i++) D[i]=Clip3(-32768,32767,((int64_t)C[i]*640+256)>>10);
 int32_t out[64]; transform_idct_8x8_fallback(out,D,4,15);
 for(int y=0;y<8;y++){for(int x=0;x<8;x++) printf("%6d ",out[y*8+x]); printf("\n");}
}
