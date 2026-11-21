#define main task_original_main
#include "../tinyraytracer.cpp"
#undef main
#include <iomanip>
#include <iostream>
int main() {
    vec3 bg=cast_ray({0,0,0},{0,1,0});
    if(std::abs(bg.x-.2f)>1e-6||std::abs(bg.y-.7f)>1e-6||std::abs(bg.z-.8f)>1e-6) return 2;
    auto [hit,t]=ray_sphere_intersect({0,0,0},vec3{-3,0,-16}.normalized(),spheres[0]);
    if(!hit||std::abs(t-(std::sqrt(265.f)-2.f))>1e-4) return 3;
    const vec3 directions[]={{0,0,-1},{-3,0,-16},{-1,-1.5f,-12},{7,5,-18},{1.5f,-.5f,-18}};
    double checksum=0;
    std::cout<<std::setprecision(9);
    for(int i=0;i<5;++i) {
        vec3 rgb=cast_ray({0,0,0},directions[i].normalized());
        if(!std::isfinite(rgb.x)||!std::isfinite(rgb.y)||!std::isfinite(rgb.z)) return 4;
        checksum+=(i+1)*(rgb.x+2*rgb.y+3*rgb.z);
        std::cout<<"SURFACE_RAY "<<i<<" rgb="<<rgb.x<<","<<rgb.y<<","<<rgb.z<<"\n";
    }
    if(checksum<=1) return 5;
    std::cout<<"SURFACE_BASELINE rays=5 sphere_t="<<t<<" checksum="<<checksum<<"\n";
    return 0;
}
