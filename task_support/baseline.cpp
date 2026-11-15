#define main upstream_render_main
#include "../main.cpp"
#undef main
#include <filesystem>
#include <iostream>
#include <cmath>
extern mat<4,4> Viewport;
struct Solid : IShader {
    bool reject;
    explicit Solid(bool r) : reject(r) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {reject,{{3,17,211,255},3}}; }
};
int main(){
    std::filesystem::create_directories(".task-build/assets");
    std::string p=".task-build/assets/mesh";
    std::ofstream(p+".obj") << "v -0.7 -0.7 0\nv 0.7 -0.7 0\nv -0.7 0.7 0\nvt 0 1\nvt 1 1\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\n";
    TGAImage dif(2,2,TGAImage::RGB,{{20,80,160,255},3});
    TGAImage nm(2,2,TGAImage::RGB,{{255,128,128,255},3});
    TGAImage sp(2,2,TGAImage::GRAYSCALE,{{0,0,0,0},1});
    if(!dif.write_tga_file(p+"_diffuse.tga") || !nm.write_tga_file(p+"_nm_tangent.tga") || !sp.write_tga_file(p+"_spec.tga"))return 1;
    Model m(p+".obj");
    if(m.nfaces()!=1 || m.nverts()!=3 || m.diffuse().get(0,0)[2]!=160 || m.uv(0,0).y!=0)return 2;
    lookat({0,0,2},{0,0,0},{0,1,0});init_perspective(2);init_viewport(0,0,16,16);init_zbuffer(16,16);
    TGAImage fb(16,16,TGAImage::RGB);PhongShader shader({0,0,1},m);
    Triangle tri={shader.vertex(0,0),shader.vertex(0,1),shader.vertex(0,2)};
    rasterize(tri,shader,fb);
    int count=0;for(double d:zbuffer)if(d>-999)++count;
    if(count<40 || fb.get(4,4)[2]==0 || std::abs(zbuffer[4+4*16])>1e-12)return 3;
    auto before=zbuffer;Triangle near={{-.7,-.7,.5,1},{.7,-.7,.5,1},{-.7,.7,.5,1}};
    rasterize(near,Solid(true),fb);if(before!=zbuffer)return 4;
    rasterize(near,Solid(false),fb);if(std::abs(zbuffer[4+4*16]-.5)>1e-12 || fb.get(4,4)[2]!=211)return 5;
    std::cout<<"PRODUCTION Model+Phong+rasterize coverage="<<count<<" depth=0.5 discard=preserved\n";
    return !fb.write_tga_file(".task-build/baseline.tga");
}
