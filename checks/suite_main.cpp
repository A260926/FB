#include "suite.h"
int main(int argc,char**argv){
 if(argc!=2)return 2;
 unsigned n=0,start=uf::count();
 for(auto &t:uf::tests())if(std::string(t.group)==argv[1]){
  unsigned b=uf::count();try{t.fn();}catch(const std::exception&e){std::cerr<<t.name<<": "<<e.what()<<"\n";return 1;}
  if(uf::count()==b){std::cerr<<"NO_ASSERTIONS "<<t.name<<"\n";return 1;}
  ++n; std::cout<<"CASE "<<t.name<<" PASS\n";
 }
 if(!n){std::cerr<<"NO_TESTS "<<argv[1]<<"\n";return 1;}
 std::cout<<"GROUP "<<argv[1]<<" PASS tests="<<n<<" checks="<<uf::count()-start<<"\n";
}
