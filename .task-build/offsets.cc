#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
int main(){std::ifstream f(".task-build/bits.txt");std::string l;struct R{int p,k,c;};std::vector<R>r;while(getline(f,l)){R x;if(sscanf(l.c_str(),"REM prefix=%d k=%d code=%d",&x.p,&x.k,&x.c)==3)r.push_back(x);} size_t o=0;for(size_t i=0;i<20;i++){int n=r[i].p<=3?r[i].k:r[i].p-3+r[i].k; size_t len=r[i].p+1+n; printf("%2zu off=%3zu len=%2zu p=%d k=%d code=%d\n",i+1,o,len,r[i].p,r[i].k,r[i].c);o+=len;} printf("off16=%zu\n",o);}
