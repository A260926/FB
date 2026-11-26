#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
#include <cmath>
int main(int c,char**v){std::ifstream a(v[1],std::ios::binary),b(v[2],std::ios::binary);std::vector<unsigned char>x((std::istreambuf_iterator<char>(a)),{}),y((std::istreambuf_iterator<char>(b)),{});size_t n=std::min(x.size(),y.size())/2;double se=0,ae=0;size_t bad=0;for(size_t i=0;i<n;i++){uint16_t u=x[2*i]|x[2*i+1]<<8,w=y[2*i]|y[2*i+1]<<8;double e=double(u)-w;se+=e*e;ae+=fabs(e);if(u!=w)bad++;}printf("n=%zu bad=%zu mae=%.1f rmse=%.1f\n",n,bad,ae/n,sqrt(se/n));}
