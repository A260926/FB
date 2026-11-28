#include <fstream>
#include <string>
#include <iterator>
int main(int argc,char**argv){if(argc<2)return 2;for(int a=1;a<argc;++a){std::ifstream f(argv[a],std::ios::binary);std::string magic;int w=0,h=0,max=0;if(!(f>>magic>>w>>h>>max)||magic!="P6"||w<2||h<2||w>4096||h>4096||max!=255)return 3;char c;f.get(c);if(c!='\n')return 4;std::string bytes((std::istreambuf_iterator<char>(f)),{});if(bytes.size()!=size_t(w)*h*3)return 5;}return 0;}
