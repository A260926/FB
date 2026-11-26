#define main fallback_dct_unused_main2
#include "../libde265/fallback-dct.cc"
#undef main
#include <cstdio>
#include <cstdint>
#include <cmath>

int main(){
 int C[64]={0};
 auto set=[&](int p,int v){C[p]=v;};
 set(63,2);set(61,-2);set(47,6);set(45,-7);set(59,4);set(57,-11);set(43,11);set(41,-31);set(31,23);set(29,-26);set(15,220);set(13,-258);set(7,2083);set(6,-8);set(5,-33);set(27,1);set(24,-1);set(19,-1);set(18,1);set(17,1);set(10,1);set(9,-1);set(8,-7);set(3,1);set(2,2);set(1,-8);
 unsigned char ref[128]; FILE*f=fopen("task_support/fixtures/mono16.yuv","rb"); fread(ref,1,128,f); fclose(f);
 double A[8][8]; int fact=4; for(int j=0;j<8;j++)for(int i=0;i<8;i++)A[j][i]=mat_dct[fact*j][i];
 for(int gainShift=-4;gainShift<=12;gainShift++){
  double err=0;
  for(int y=0;y<8;y++)for(int x=0;x<8;x++){
   double tmp[8][8];
   for(int i=0;i<8;i++)for(int c=0;c<8;c++){double s=0;for(int j=0;j<8;j++)s+=A[j][i]*(C[j*8+c]*640.0/1024.0);tmp[i][c]=(s+64)/128.0;}
   double s=0; for(int j=0;j<8;j++)s+=tmp[y][j]*A[j][x];
   double out=(s+8)/16.0;
   for(int sh=0;sh<gainShift;sh++) out*=2.0;
   for(int sh=0;sh<-gainShift;sh++) out/=2.0;
   uint16_t v=ref[2*(y*64+x)]|(ref[2*(y*64+x)+1]<<8); double target=(double)v-32768;
   double e=out-target; err+=e*e;
  }
  printf("gain2^%+d rmse %.1f\n",gainShift,sqrt(err/64));
 }
}
