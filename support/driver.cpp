#include "contract.hpp"
#include <set>
int main(int argc,char** argv) {
  const bool old=argc==2 && std::string(argv[1])=="upstream";
  const std::vector<std::string> groups=old ? std::vector<std::string>{"upstream"} : std::vector<std::string>{"geometry","ownership","session"};
  std::set<std::string> names;
  for (const auto& c:uf::cases()) {
    if (!names.insert(c.name).second) { std::cerr<<"duplicate case\n"; return 2; }
    bool known=false; for (const auto& g:groups) if(c.group==g) known=true;
    if (!known) { std::cerr<<"unknown group\n"; return 2; }
  }
  unsigned long long total=0;
  try {
    for(const auto& g:groups) {
      unsigned long long count=0, sum=0;
      for(const auto& c:uf::cases()) if(c.group==g) {
        std::cout<<"BEGIN "<<g<<" "<<c.name<<std::endl;
        auto before=uf::checks();
        c.run();
        auto used=uf::checks()-before;
        if(!used) throw std::runtime_error("case returned without content assertions");
        std::cout<<"CASE "<<g<<" "<<c.name<<" "<<used<<std::endl;
        ++count; sum+=used; ++total;
      }
      std::cout<<"GROUP "<<g<<" "<<count<<" "<<sum<<std::endl;
    }
    std::cout<<"TARGET_COMPLETE cases="<<total<<" checks="<<uf::checks()<<std::endl;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
  return 0;
}
