#define main fallback_dct_unused_main4
#include "../libde265/fallback-dct.cc"
#undef main
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

int main(int argc,char**argv){
 unsigned char ref[24576]; FILE*f=fopen(argc>1?argv[1]:"task_support/fixtures/mono16.yuv","rb"); fread(ref,1,sizeof(ref),f); fclose(f);
 double r[8][8]; for(int y=0;y<8;y++)for(int x=0;x<8;x++){uint16_t v=ref[2*(y*64+x)]|(ref[2*(y*64+x)+1]<<8);r[y][x]=(double)v-32768;}
 double A[8][8]; int fact=4; for(int j=0;j<8;j++)for(int i=0;i<8;i++)A[j][i]=mat_dct[fact*j][i];
 int pos[]={63,61,47,45,59,57,43,41,31,29,15,13,7,6,5,27,24,19,18,17,10,9,8,3,2,1};
 int m=64,n=26;
 for(int S : {11,18}){
  std::vector<std::vector<double>> N(n,std::vector<double>(n+1,0));
  for(int eq=0;eq<m;eq++){
   int oy=eq/8,ox=eq%8;
   std::vector<double> col(n);
   for(int z=0;z<n;z++){int p=pos[z],dy=p/8,dx=p%8; double s=0;
     // coefficient D[dy,dx], V output i,c: A[dy][i] at i=oy? Then H output oy,ox: for j=oy A[j][ox] and g[oy,dx].
     // V g[i=oy,c=dx] contribution D[dy,dx]: A[dy][oy]. H out[oy,ox] uses g[oy,j=dx] A[dx][ox].
     s=A[dy][oy]*A[dx][ox]; col[z]=s; }
   double rhs=std::ldexp(r[oy][ox],S);
   for(int i=0;i<n;i++){ N[i][n]+=col[i]*rhs; for(int j=0;j<n;j++) N[i][j]+=col[i]*col[j]; }
  }
  for(int col=0;col<n;col++){int piv=col;for(int row=col+1;row<n;row++)if(fabs(N[row][col])>fabs(N[piv][col]))piv=row;std::swap(N[piv],N[col]);double d=N[col][col];for(int j=col;j<=n;j++)N[col][j]/=d;for(int row=0;row<n;row++)if(row!=col){double q=N[row][col];for(int j=col;j<=n;j++)N[row][j]-=q*N[col][j];}}
  double sse=0; for(int eq=0;eq<m;eq++){int oy=eq/8,ox=eq%8;double pred=0;for(int z=0;z<n;z++){int p=pos[z],dy=p/8,dx=p%8;pred+=N[z][n]*A[dy][oy]*A[dx][ox];} double t=std::ldexp(r[oy][ox],S); double e=pred-t;sse+=e*e;} printf("linear rmse transformed %.1f residual %.1f\n",sqrt(sse/m),sqrt(sse/m)/(1<<S));
  printf("S=%d estimated C via EP d=80C / normal d=.625C\n",S);
  for(int z=0;z<n;z++){double D=N[z][n]; double Cep=D/160.0, Cnormal=D*0.8; printf("p%02d D=%10.0f Cep=%8.1f Cn=%8.1f\n",pos[z],D,Cep,Cnormal);}
 }
}
