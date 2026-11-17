#include "object/ObjectList.h"
#include "object/Sphere.h"
#include "bvh/Node.h"
#include "light/PointLight.h"
#include <cmath>
#include <iostream>

int main() {
    Material near_mat, far_mat, excluded_mat;
    auto near = std::make_shared<Sphere>(Point3(0,0,-3),1,&near_mat);
    auto far = std::make_shared<Sphere>(Point3(0,0,-7),1,&far_mat);
    auto outside = std::make_shared<Sphere>(Point3(4,0,-3),1,&excluded_mat,true);
    ObjectList world;
    world.add(far); world.add(near); world.add(outside); world.build();
    Ray ray(Point3(0,0,0),Vector3(0,0,-1));
    IntersectionRecord a,b,c;
    world.use_bvh=UseBVH::ENABLE;
    bool hit_a=world.intersect(ray,Interval(0.001,100),a);
    world.use_bvh=UseBVH::DISABLE;
    bool hit_b=world.intersect(ray,Interval(0.001,100),b);
    world.use_bvh=UseBVH::ENABLE;
    bool hit_c=world.intersect(Ray(Point3(4,0,0),Vector3(0,0,-1)),Interval(0.001,100),c);
    std::cout << "BASELINE_CLOSEST bvh=" << a.t << " brute=" << b.t
              << " excluded=" << c.t << '\n';
    if(!hit_a || !hit_b || !hit_c || a.t!=2 || b.t!=2 || c.t!=2 ||
       a.mat_ptr!=&near_mat || b.mat_ptr!=&near_mat || c.mat_ptr!=&excluded_mat) return 1;
    PointLight light(Point3(0,0,0),Radiance(1,1,1));
    double shadow=light.shadow_factor(Point3(0,0,-10),world);
    double clear=light.shadow_factor(Point3(10,0,0),world);
    std::cout << "BASELINE_SHADOW blocked=" << shadow << " clear=" << clear << '\n';
    if(std::abs(shadow-0.1)>1e-12 || clear!=1) return 2;
    std::cout << "BASELINE_OK production_bvh_brute_excluded_shadow\n";
}
