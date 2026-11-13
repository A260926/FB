#include "checks.hpp"
#include <iostream>
int main(int argc,char** argv) {
  if(argc!=2) return 2;
  try {
    run_join_tests(argv[1]);
    if(join_checks::count<8) throw std::runtime_error("expected at least 8 real checks");
    std::cout<<"group="<<argv[1]<<" checks="<<join_checks::count<<" PASS\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
