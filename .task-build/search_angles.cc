#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <random>
using namespace std;
int main(){
 mt19937 rng(12345);
 uniform_real_distribution<double> U(0,6.283185307179586);
 for(int trial=0;trial<2000000;trial++){
  double f[3],b[3];
  for(int i=0;i<3;i++){f[i]=U(rng);b[i]=U(rng);}
  double C[3][3];
  for(int r=0;r<3;r++)for(int c=0;c<3;c++)C[r][c]=1.0-cos(f[r]-b[c]);
  // greedy row order
  int used[3]={0,0,0};double g=0;
  for(int r=0;r<3;r++){int bc=-1;double bv=1e9;for(int c=0;c<3;c++)if(!used[c]&&C[r][c]<bv-1e-15){bv=C[r][c];bc=c;}used[bc]=1;g+=bv;}
  // optimal
  int p[3]={0,1,2};double opt=1e9;
  do{double s=0;for(int r=0;r<3;r++)s+=C[r][p[r]];opt=min(opt,s);}while(next_permutation(p,p+3));
  if(opt < g-0.3){
   printf("trial %d greedy=%.6f opt=%.6f\n",trial,g,opt);
   for(int i=0;i<3;i++)printf("f%d angle=%.9f b%d=%.9f\n",i,f[i],i,b[i]);
   for(int r=0;r<3;r++){printf("C%d:",r);for(int c=0;c<3;c++)printf(" %.6f",C[r][c]);printf("\n");}
   return 0;
  }
 }
 printf("none\n");
}
