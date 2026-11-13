#include "registry.hpp"
#include <array>
#include <iostream>
int main(){try{
 task::Registry r;register_spatial_tests(r);
 const std::array<std::string,4> groups={"clipping","budget","queries","integration"};
 for(auto& e:r.entries){bool known=false;for(auto& g:groups)known|=e.group==g;if(!known)throw std::runtime_error("unknown group");}
 size_t total=0,checks=0;
 for(auto& g:groups){size_t n=0,c=0;
  for(auto& e:r.entries)if(e.group==g){task::Case tc;e.run(tc);
   if(tc.checks<2)throw std::runtime_error("case has fewer than two checks");
   ++n;c+=tc.checks;std::cout<<"CASE "<<g<<" "<<e.name<<" checks="<<tc.checks<<"\n";
  }
  if(n<2)throw std::runtime_error("group has fewer than two cases");
  total+=n;checks+=c;std::cout<<"GROUP "<<g<<" cases="<<n<<" checks="<<c<<"\n";
 }
 std::cout<<"TARGET_DONE cases="<<total<<" checks="<<checks<<"\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"TARGET_FAILURE "<<e.what()<<"\n";return 3;}}
