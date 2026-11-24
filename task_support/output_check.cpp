#include "ts_check.h"
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc,char** argv){
 if(argc!=2)return 30;std::ifstream in(argv[1],std::ios::binary);
 if(!in)return 31;std::vector<unsigned char> d((std::istreambuf_iterator<char>(in)),{});
 return inspect_simple(d.data(),d.size(),true);
}
