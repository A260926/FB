#include "rtweekend.h"
#include "bvh.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include <sstream>
#include <string>
#include <fstream>
#include <cstdlib>

int main() {
 auto matte=make_shared<lambertian>(color(.6,.4,.2));
 auto glow=make_shared<diffuse_light>(color(5,4,3));
 auto ball=make_shared<sphere>(point3(0,0,-3),1,matte);
 auto lamp=make_shared<quad>(point3(-2,3,-5),vec3(4,0,0),vec3(0,0,4),glow);
 hittable_list flat;
 flat.add(ball);flat.add(lamp);
 bvh_node tree(flat);
 hit_record a,b;
 const ray r(point3(0,0,0),vec3(0,0,-2),.25);
 if(!flat.hit(r,interval(.001,100),a) || !tree.hit(r,interval(.001,100),b)) return 2;
 if(std::fabs(a.t-1)>1e-12 || std::fabs(a.t-b.t)>1e-12 || a.mat!=b.mat) return 3;
 if(tree.hit(r,interval(.001,.9),b)) return 4;
 hittable_list lights(lamp);
 const auto configure=[](){camera c;c.aspect_ratio=1;c.image_width=8;c.samples_per_pixel=4;c.max_depth=4;c.background=color(.15,.1,.05);c.vfov=40;c.lookfrom=point3(0,1,2);c.lookat=point3(0,0,-3);return c;};
 std::ostringstream image1,image2;
 auto* saved=std::cout.rdbuf(image1.rdbuf());
 std::srand(1729);auto cam1=configure();cam1.render(tree,lights);
 std::cout.rdbuf(image2.rdbuf());
 std::srand(1729);auto cam2=configure();cam2.render(flat,lights);
 std::cout.rdbuf(saved);
 if(image1.str()!=image2.str()) return 5;
 std::istringstream in(image1.str());std::string magic;int w,h,m;
 if(!(in>>magic>>w>>h>>m)||magic!="P3"||w!=8||h!=8||m!=255) return 6;
 int count=0,nonblack=0,red,green,blue;
 while(in>>red>>green>>blue){if(red<0||red>255||green<0||green>255||blue<0||blue>255)return 7;++count;if(red||green||blue)++nonblack;}
 if(count!=64||nonblack==0)return 8;
 std::ofstream out(".task-build/baseline.ppm");out<<image1.str();if(!out)return 9;
 std::cout<<"BASELINE_BVH t="<<a.t<<" clipped=1 material=1\n";
 std::cout<<"BASELINE_RENDER pixels="<<count<<" nonblack="<<nonblack<<" flat_equal=1\n";
 std::cout<<"BASELINE_OK native_bvh camera_mis ppm\n";
}
