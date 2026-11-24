#include <cmath>
#include <fstream>
#include <iostream>
int main(int argc,char**argv){if(argc!=2)return 2;std::ifstream f(argv[1],std::ios::binary);float x;size_t n=0;double energy=0;
 while(f.read(reinterpret_cast<char*>(&x),sizeof x)){if(!std::isfinite(x))return 3;energy+=std::abs(x);++n;}
 if(!f.eof()||f.gcount()!=0||n<512||n%2||energy==0)return 4;
 std::cout<<"DUCKING_DEMO_FRESH_OK frames="<<n/2<<"\n";
}
