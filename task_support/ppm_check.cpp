#include <fstream>
#include <string>
int main(int n,char**a){if(n!=2)return 1;std::ifstream f(a[1]);std::string magic;int w,h,m;if(!(f>>magic>>w>>h>>m)||magic!="P3"||w<32||h<32||m!=255)return 2;for(int i=0,v;i<w*h*3;i++){if(!(f>>v)||v<0||v>255)return 3;}std::string extra;if(f>>extra)return 4;return 0;}
