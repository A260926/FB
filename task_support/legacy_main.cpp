#include <gtest/gtest.h>
#include <Unittests/unittests_split_copy.cc>
#include <iostream>
int main(){try {if(legacy::entries().size()!=2)return 1;for(auto&e:legacy::entries()){int n=legacy::checks();e.run();if(legacy::checks()==n)return 2;std::cout<<"LEGACY_PASS "<<e.name<<" checks="<<legacy::checks()-n<<"\n";}std::cout<<"LEGACY_COMPLETE cases=2 checks="<<legacy::checks()<<"\n";}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 3;}}
