#include "oss-fuzz/structured.h"
#include <iostream>
#include <vector>
int main(){
 const std::vector<unsigned> required;
 const std::string a=reduceCode2(nullptr,0,"",required,0);
 const std::string b=reduceCode2(nullptr,0,"",required,0);
 if(a.empty()||a!=b||a.find("int f1()") == std::string::npos||a.find("return 0;")==std::string::npos) return 1;
 std::cout<<"structured API empty input deterministic bytes="<<a.size()<<"\n";
}
