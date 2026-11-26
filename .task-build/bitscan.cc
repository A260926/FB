#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
int main(){std::ifstream f(".task-build/bits.txt");std::string l;struct R{int p,k,c;};std::vector<R>r;while(getline(f,l)){R x;if(sscanf(l.c_str(),"REM prefix=%d k=%d code=%d",&x.p,&x.k,&x.c)==3)r.push_back(x);}
std::vector<int> b; std::vector<size_t> off; for(auto x:r){off.push_back(b.size());for(int i=0;i<x.p;i++)b.push_back(1);b.push_back(0);int n=x.p<=3?x.k:x.p-3+x.k;for(int i=n-1;i>=0;i--)b.push_back((x.c>>i)&1);}
size_t start=off[15]; printf("start %zu\n",start); for(size_t i=start;i<start+100&&i<b.size();i++){printf("%d",b[i]); if((i-start+1)%10==0)printf(" ");} printf("\nindex:\n"); for(int i=0;i<100;i++){if(i%10==0)printf("%-10d",i);} printf("\n");}
