#include "oss-fuzz/type2.h"
#include <iostream>
#include <string>
int main(){
 const std::string s=generateCode2(nullptr,0);
 unsigned vars=0; for(unsigned i=1;i<=5;++i) vars+=s.find("int var"+std::to_string(i)+" =")!=std::string::npos;
 bool function=s.find("int f1()")!=std::string::npos, ret=s.find("return 0;")!=std::string::npos;
 std::cout<<"generator vars="<<vars<<" f1="<<function<<" return_zero="<<ret<<" bytes="<<s.size()<<"\n";
 return vars==5&&function&&ret?0:1;
}
