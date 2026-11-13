#include <iostream>
#include <exception>
namespace uv_tests { void energy_gradient(); void distortion_reduction(); void no_flip(); void boundary_holes(); void sparse_components(); void rejection(); }
int main(){try{
#define RUN(x) uv_tests::x(); std::cout<<"PASS " #x "\n";
 RUN(energy_gradient) RUN(distortion_reduction) RUN(no_flip) RUN(boundary_holes) RUN(sparse_components) RUN(rejection)

}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
