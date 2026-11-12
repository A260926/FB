#include "DynamicAudioNormalizer.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <new>
#include <iostream>
static bool track=false;static unsigned long allocs=0,frees=0;
void* operator new(std::size_t n){if(track)++allocs;if(void*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p)noexcept{if(track&&p)++frees;std::free(p);}
void*operator new[](std::size_t n){return ::operator new(n);}void operator delete[](void*p)noexcept{::operator delete(p);}
void operator delete(void*p,std::size_t)noexcept{::operator delete(p);}void operator delete[](void*p,std::size_t)noexcept{::operator delete(p);}
int main(){MDynamicAudioNormalizer f(2,8000,32,31,.95,10,0,0,true,false,false);assert(f.initialize());double x[2][256],y[2][256];const double* in[2]={x[0],x[1]};double*out[2]={y[0],y[1]};long long produced=0;double energy=0;for(int b=0;b<400;b++){for(int i=0;i<256;i++){x[0][i]=.13*std::sin(.029*(b*256+i));x[1][i]=-.5*x[0][i];}int64_t n=0;track=true;bool ok=f.process(in,out,256,n);track=false;assert(ok&&n>=0&&n<=256);produced+=n;for(int i=0;i<n;i++){assert(std::isfinite(y[0][i])&&std::abs(y[1][i]+.5*y[0][i])<1e-12);energy+=y[0][i]*y[0][i];}}for(;;){int64_t n=0;track=true;bool ok=f.flushBuffer(out,256,n);track=false;assert(ok&&n>=0&&n<=256);if(!n)break;produced+=n;for(int i=0;i<n;i++)energy+=y[0][i]*y[0][i];}track=true;bool reset=f.reset();track=false;assert(reset&&produced==102400&&energy>1);std::cout<<"DAN_BASELINE_OK input=102400 produced="<<produced<<" api_allocs="<<allocs<<" api_frees="<<frees<<" energy="<<energy<<"\n";}
