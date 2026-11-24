#include <vector>
#include <fstream>
#include <cmath>
#include <string>
#include <cstdio>
static std::vector<float>read(const std::string&p,size_t n){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f||f.tellg()!=std::streamoff(n*sizeof(float)))throw 1;f.seekg(0);std::vector<float>x(n);if(!f.read(reinterpret_cast<char*>(x.data()),n*sizeof(float)))throw 2;return x;}
int main(int argc,char**argv){try{if(argc!=2)return 2;std::string d=argv[1];auto r=read(d+"/reference-mix.f32",30000);std::vector<float>y;
for(size_t start:{size_t(0),size_t(8000),size_t(16000),size_t(24000)}){auto v=read(d+"/p"+std::to_string(start)+".f32",start==24000?6000:8000);y.insert(y.end(),v.begin(),v.end());}
double e=0;for(size_t i=0;i<r.size();++i){if(!std::isfinite(r[i])||!std::isfinite(y[i])||std::fabs(y[i]-r[i])>2e-5)return 3;e+=y[i]*y[i];}if(e<1)return 4;std::printf("PARTITION_PROCESSES_OK frames=%zu energy=%.6f\n",y.size(),e);return 0;}catch(...){return 5;}}
