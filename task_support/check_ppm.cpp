#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char**argv) {
  if(argc!=2) return 2;
  std::ifstream in(argv[1]); std::string magic;
  int w=0,h=0,m=0;in>>magic>>w>>h>>m;
  if(!in || magic!="P3" || w!=192 || h!=96 || m!=255) return 3;
  int lit=0,left=0,right=0;
  for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
    int r,g,b;in>>r>>g>>b;
    if(!in || r<0 || r>255 || g<0 || g>255 || b<0 || b>255) return 4;
    if(r||g||b){++lit;if(x<8)++left;if(x>=w-8)++right;}
  }
  std::string tail;if(in>>tail)return 5;
  if(!lit || !left || !right || lit==w*h) return 6;
  std::cout<<"PANORAMA_IMAGE_COMPLETE lit="<<lit<<" left="<<left<<" right="<<right<<"\n";
}
