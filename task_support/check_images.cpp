#include <fstream>
#include <string>
#include <iterator>
#include <limits>
static bool token(std::istream& f,std::string& out){for(;;){f>>std::ws;if(f.peek()!='#')break;f.ignore(std::numeric_limits<std::streamsize>::max(),'\n');}return bool(f>>out);}
int main(int argc,char**argv){if(argc<2)return 2;for(int a=1;a<argc;++a){std::ifstream f(argv[a],std::ios::binary);std::string m,x,y,z;if(!token(f,m)||!token(f,x)||!token(f,y)||!token(f,z)||m!="P6")return 3;int w=0,h=0,max=0;try{w=std::stoi(x);h=std::stoi(y);max=std::stoi(z);}catch(...){return 3;}if(w<2||h<2||w>4096||h>4096||max!=255)return 4;char c;f.get(c);if(c!='\n')return 5;std::string bytes((std::istreambuf_iterator<char>(f)),{});if(bytes.size()!=size_t(w)*h*3)return 6;}return 0;}
