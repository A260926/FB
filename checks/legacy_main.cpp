#include "legacy/gtest/gtest.h"
#include <iostream>
#include "../tests/T_NurbsSurface.cpp"
#include "../tests/T_AdvancedSurface.cpp"
int main(){
 int n=0;
 try {for(auto& c:legacy_cases()){c.run(); ++n; std::cout<<"PASS "<<c.name<<"\n";}}
 catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
 if(n!=6) return 1;
 std::cout<<"LNLIB_LEGACY_OK cases="<<n<<"\n";
}
