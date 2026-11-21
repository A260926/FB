#define main original_cube_example_main
#include "examples/CubeOfSpheres.cpp"
#undef main
#include <iostream>
int main() {
  std::vector<Sphere<double>> objects;
  for (int i=0;i<64;++i) objects.emplace_back(Vector3<double>{4.0*i,0,0},1.0);
  FastBVH::DefaultBuilder<double> builder;
  auto tree=builder(objects,SphereBoxConverter<double>{});
  SphereIntersector<double> intersector;
  FastBVH::Traverser<double,Sphere<double>,decltype(intersector)> tracer(tree,intersector);
  int matched=0;
  for (int i=0;i<64;++i) {
    Ray<double> ray({4.0*i,0,-8},{0,0,1});
    auto got=tracer.traverse(ray);
    FastBVH::Intersection<double,Sphere<double>> ref;
    for (const auto& object:objects) ref=FastBVH::closest(ref,intersector(object,ray));
    if (!got || !ref || std::abs(got.t-ref.t)>1e-12 || std::abs(got.t-7)>1e-12 || got.object!=ref.object || std::abs(got.normal.z+1)>1e-12) return 2;
    ++matched;
  }
  std::cout<<"legacy_spheres=64 matched="<<matched<<" nodes="<<tree.getNodes().size()<<"\n";
  return matched==64?0:3;
}
