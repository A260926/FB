#include "check.hpp"
#include <iostream>
void test_overlap(Check&);void test_links(Check&);void test_protocol(Check&);
int main(){int failed=0;void(*f[])(Check&)={test_overlap,test_links,test_protocol};const char* names[]={"overlap","links","protocol"};for(int i=0;i<3;++i){Check c;try{f[i](c);if(c.n<3)throw std::runtime_error("group has fewer than 3 assertions");std::cout<<names[i]<<": "<<c.n<<" checks\n";}catch(const std::exception&e){std::cerr<<names[i]<<": "<<e.what()<<"\n";++failed;}}return failed?1:0;}
