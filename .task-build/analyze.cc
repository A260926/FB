#define main fallback_dct_unused_main
#include "../libde265/fallback-dct.cc"
#undef main

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  std::ifstream f(argv[1], std::ios::binary);
  unsigned char bytes[128];
  f.read((char*)bytes, sizeof(bytes));
  double r[8][8];
  for (int y=0;y<8;y++) for(int x=0;x<8;x++) {
    uint16_t v = bytes[2*(y*64+x)] | (bytes[2*(y*64+x)+1]<<8);
    r[y][x] = (double)v - 32768.0;
  }
  double A[8][8];
  int fact=4;
  for (int j=0;j<8;j++) for(int i=0;i<8;i++) A[j][i]=mat_dct[fact*j][i];

  // Solve kron(A^T, A^T?) equation A^T d A = 2^18 r.
  int N=64;
  std::vector<std::vector<double>> M(N, std::vector<double>(N+1,0.0));
  auto idx=[](int y,int x){return y*8+x;};
  for (int y=0;y<8;y++) for(int x=0;x<8;x++) {
    int row=idx(y,x);
    M[row][N]=std::ldexp(r[y][x],18);
    for(int a=0;a<8;a++) for(int b=0;b<8;b++) {
      // sum_j,i A[j,y]? (A^T)[y,j]=A[j,y]; d[j,i]; A[i,x]
      M[row][idx(a,b)] = A[a][y] * A[b][x];
    }
  }
  for(int col=0;col<N;col++){
    int piv=col;
    for(int row=col+1;row<N;row++) if(std::abs(M[row][col])>std::abs(M[piv][col])) piv=row;
    std::swap(M[piv],M[col]);
    double div=M[col][col];
    for(int j=col;j<=N;j++) M[col][j]/=div;
    for(int row=0;row<N;row++) if(row!=col){
      double fac=M[row][col];
      for(int j=col;j<=N;j++) M[row][j]-=fac*M[col][j];
    }
  }
  printf("estimated C (EP shift2 fact640)\n");
  for(int y=0;y<8;y++){
    for(int x=0;x<8;x++){
      double d=M[idx(y,x)][N];
      double c=d*4.0/640.0;
      printf("%8.1f ",c);
    }
    printf("\n");
  }
  printf("rounded integer C\n");
  for(int y=0;y<8;y++){
    for(int x=0;x<8;x++){
      double d=M[idx(y,x)][N];
      long c=std::lround(d*4.0/640.0);
      printf("%4ld ",c);
    }
    printf("\n");
  }
}
