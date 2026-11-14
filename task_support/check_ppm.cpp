#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char**argv) {
  if(argc!=2) return 2;
  std::ifstream in(argv[1]); std::string magic;
  int w=0,h=0,m=0; in>>magic>>w>>h>>m;
  if(!in || magic!="P3" || w!=96 || h!=64 || m!=255) return 3;
  int lo=255,hi=0,partial=0;
  for(int i=0;i<w*h;i++) {
    int r,g,b; in>>r>>g>>b;
    if(!in || r<0 || r>255 || g!=r || b!=r) return 4;
    if(r<lo) lo=r; if(r>hi) hi=r; if(r>25 && r<255) ++partial;
  }
  std::string tail; if(in>>tail) return 5;
  if(hi<=lo || !partial) return 6;
  std::cout<<"PPM_COMPLETE 96x64 partial="<<partial<<"\n";
}
