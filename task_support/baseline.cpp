#include "signalsmith-stretch.h"
#include <cmath>
#include <cstdio>
#include <vector>
int main(){const int n=24000;for(bool split:{false,true}){double energy=0,relation=0;signalsmith::stretch::SignalsmithStretch<float> s(42);s.configure(2,1024,256,split);s.setTransposeSemitones(7);std::vector<float>a(n),b(n),x(n),y(n);for(int i=0;i<n;i++){a[i]=.2f*std::sin(6.283185307179586*440*i/48000);b[i]=-.5f*a[i];}float *in[]={a.data(),b.data()},*out[]={x.data(),y.data()};s.process(in,n,out,n);for(int i=4096;i<n;i++){if(!std::isfinite(x[i])||!std::isfinite(y[i]))return 1;energy+=x[i]*x[i];relation=std::max(relation,std::abs(double(y[i]+.5*x[i])));}std::printf("STRETCH_BASELINE split=%d frames=%d energy=%.6f stereo_error=%.8f\n",int(split),n,energy,relation);if(energy<100||relation>2e-4)return 2;}return 0;}
