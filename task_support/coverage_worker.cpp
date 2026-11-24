// Benign text-only coverage fixture. Never compiles or executes its input.
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
int main(int argc,char**argv){
 if(argc!=2)return 2;
 std::ifstream f(argv[1]); if(!f)return 3;
 std::string s((std::istreambuf_iterator<char>(f)),{});
 std::cout<<"COVER";
 if(s.find("return")!=std::string::npos)std::cout<<" 1";
 if(s.find("var1")!=std::string::npos)std::cout<<" 2";
 if(s.find("if (")!=std::string::npos)std::cout<<" 3";
 std::cout<<"\n";
}
