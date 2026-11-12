#include "Iir.h"
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <iostream>
using Chain=Iir::Custom::SOSCascade<2>;
static const double sos[2][6]={{.2,.4,.2,1,-.7,.2},{.3,-.1,.05,2,-.9,.18}};
static std::vector<double> forward(std::vector<double>x){Chain f(sos);for(auto &v:x)v=f.filter(v);return x;}
static std::vector<double> reverse_filter(std::vector<double>x){std::reverse(x.begin(),x.end());x=forward(x);std::reverse(x.begin(),x.end());return x;}
int main(){std::vector<double>x(31);for(size_t i=0;i<x.size();i++)x[i]=.1*i+sin(.7*i);auto a=reverse_filter(forward(x));auto b=forward(reverse_filter(x));double d=0;for(size_t i=0;i<x.size();i++){assert(std::isfinite(a[i]));d=std::max(d,std::abs(a[i]-b[i]));}assert(d>1e-4);std::cout<<"IIR_BASELINE_OK stages=2 samples=31 directional_difference="<<d<<"\n";}
