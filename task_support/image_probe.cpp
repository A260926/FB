#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <string>
int main(int argc,char**argv){try{if(argc!=2)throw std::runtime_error("argument");std::ifstream in(argv[1],std::ios::binary);std::string magic;int w,h;double scale;in>>magic>>w>>h>>scale;in.get();if(magic!="PF"||w!=8||h!=8||scale!=-1)throw std::runtime_error("header");std::vector<float> f(8*8*3);in.read((char*)f.data(),f.size()*sizeof(float));if(in.gcount()!=int(f.size()*sizeof(float)))throw std::runtime_error("payload");int fg=0,bg=0;for(int i=0;i<64;i++){float r=f[i*3],g=f[i*3+1],b=f[i*3+2];if(!std::isfinite(r)||!std::isfinite(g)||!std::isfinite(b))throw std::runtime_error("nonfinite");if(r>0){if(std::abs(r-.64)>1e-6||std::abs(g-.25)>1e-6||std::abs(b-.09)>1e-6)throw std::runtime_error("emission");fg++;}else{if(g!=0||b!=0)throw std::runtime_error("background");bg++;}}if(!fg||!bg)throw std::runtime_error("geometry coverage");std::cout<<"BASELINE render=8x8 finite=64 emission=0.64,0.25,0.09 foreground="<<fg<<" background="<<bg<<"\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
