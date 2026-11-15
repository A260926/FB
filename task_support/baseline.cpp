#include <render/Rasterizer.hpp>
#include <object/Triangle.hpp>
#include <cmath>
#include <iostream>
using namespace SoftRasterizer;
int main() {
  auto scene=std::make_shared<Scene>("baseline",glm::vec3(0),glm::vec3(0,0,-1));
  scene->setProjectionMatrix(70,.05f,100.f);
  auto material=std::make_shared<Material>(); material->Kd={.8f,.2f,.1f};
  std::array<Vertex,3> v{Vertex({-1,-1,-2}),Vertex({1,-1,-3}),Vertex({0,1,-2})};
  auto t=std::make_shared<Triangle>(v,material);
  auto shader=std::make_shared<Shader>(); shader->type=SHADERS_TYPE::TEXTURE;
  t->bindShader2Mesh(shader); scene->addGraphicObj(t,"triangle");
  TraditionalRasterizer a(63,47),b(63,47); a.addScene(scene); b.addScene(scene); a.setSIMD(false);
  a.draw(); b.draw(); size_t covered=0;
  for(size_t i=0;i<a.depth().size();++i){
    if(std::isfinite(a.depth()[i])){++covered; if(!std::isfinite(b.depth()[i])||std::abs(a.depth()[i]-b.depth()[i])>1e-6f||glm::length(a.pixels()[i]-b.pixels()[i])>1e-5f)return 2;}
    else if(std::isfinite(b.depth()[i]))return 3;
  }
  if(covered<100||a.pixels()[23*63+31].r<.7f)return 4;
  std::cout<<"BASELINE_RENDER_OK covered="<<covered<<" scalar_simd=match\n";
}
