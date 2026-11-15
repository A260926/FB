#define main upstream_trace_main
#include "../tinyraytracer.cpp"
#undef main
#include <iostream>
#include <iomanip>
#include <filesystem>
int main(){
    Sphere probe={{0,0,-4},1,ivory};
    auto [ok,t]=ray_sphere_intersect({0,0,0},{0,0,-1},probe);
    if(!ok || std::abs(t-3)>1e-6)return 1;
    auto [inside,exit_t]=ray_sphere_intersect({0,0,-4},{0,0,1},probe);
    if(!inside || std::abs(exit_t-1)>1e-6)return 2;
    vec3 miss=cast_ray({0,0,0},{0,1,0});
    if(std::abs(miss.x-.2f)>1e-6 || std::abs(miss.y-.7f)>1e-6 || std::abs(miss.z-.8f)>1e-6)return 3;
    constexpr int W=16,H=12;
    std::ofstream out(".task-build/baseline.ppm",std::ios::binary);out<<"P6\n16 12\n255\n";
    int finite=0,changed=0; double energy=0;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        vec3 d={float((x+.5)-W/2.),float(-(y+.5)+H/2.),float(-H/(2*std::tan(1.05/2)))};
        vec3 c=cast_ray({0,0,0},d.normalized());
        bool valid=true;for(int k=0;k<3;++k)valid=valid&&std::isfinite(c[k])&&c[k]>=0;
        if(!valid)return 4;++finite;
        if((c-miss).norm()>1e-4)++changed;
        energy+=c.x+c.y+c.z;
        float scale=std::max({1.f,c.x,c.y,c.z});
        for(int k=0;k<3;++k)out.put(char(255*c[k]/scale));
    }
    if(changed<5 || changed>=W*H || !(energy>0) || !out)return 5;
    std::cout<<"PRODUCTION legacy rays="<<W*H<<" finite="<<finite<<" changed="<<changed<<" sphere_t="<<t<<" inside_exit="<<exit_t<<" energy="<<std::setprecision(10)<<energy<<"\n";
}
