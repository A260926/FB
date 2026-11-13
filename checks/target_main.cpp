#include <iostream>
#include <exception>
namespace volume_tests { void constrained_optimum(); void singular_constraint(); void live_collapse(); void shell_invariance(); void rejection(); void compatibility(); }
int main(){try{
#define RUN(x) volume_tests::x(); std::cout<<"PASS " #x "\n";
 RUN(constrained_optimum) RUN(singular_constraint) RUN(live_collapse) RUN(shell_invariance) RUN(rejection) RUN(compatibility)

}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
