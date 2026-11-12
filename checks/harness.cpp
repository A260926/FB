#include "cases.hpp"
#include <iostream>
#include <set>
int main(){std::set<std::string> required={"decoded_fragments","parser_roles","error_completion","bounded_storage"};int n=0;
try{for(auto &t:cases()){int before=assertions();t.fn();if(assertions()==before)throw std::runtime_error("empty case");required.erase(t.name);++n;std::cout<<"PASS "<<t.name<<"\n";}if(!required.empty()||n==0)throw std::runtime_error("missing groups");}
catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
std::cout<<"TARGET_COMPLETE cases="<<n<<" assertions="<<assertions()<<"\n";}
