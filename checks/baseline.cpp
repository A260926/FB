#include <chowdsp_wdf/chowdsp_wdf.h>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace chowdsp::wdft;
int main(){ResistiveVoltageSourceT<double> src(1000);CapacitorT<double> c(1.e-7,48000);auto s=makeSeries<double>(src,c);auto inv=makeInverter<double>(s);DiodePairT<double,decltype(inv)> d(inv,1.e-9);double energy=0;for(int n=0;n<4096;n++){src.setVoltage(1.5*std::sin(.071*n));d.incident(inv.reflected());inv.incident(d.reflected());double y=voltage<double>(c);assert(std::isfinite(y));energy+=y*y;}assert(energy>1);std::cout<<"WDF_BASELINE_OK samples=4096 energy="<<energy<<"\n";}
