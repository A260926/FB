#include "rtweekend.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "bvh.h"
#include "constant_medium.h"
#include <sstream>
#include <cassert>
int main() {
    auto diffuse=make_shared<lambertian>(color(.6,.4,.2));
    auto body=make_shared<sphere>(point3(0,0,-2),.5,diffuse);
    hit_record h;
    assert(body->hit(ray(point3(0,0,0),vec3(0,0,-2),.25),interval(.001,infinity),h));
    assert(std::fabs(h.t-.75)<1e-12 && h.front_face);
    auto boundary=make_shared<sphere>(point3(0,0,0),1,diffuse);
    constant_medium fog(boundary,.5,color(.8,.6,.4));
    std::srand(314159);
    int count=0;
    for(int i=0;i<4096;++i) {
        hit_record m;
        if(fog.hit(ray(point3(0,0,-2),vec3(0,0,2),.7),interval(.001,infinity),m)) {
            ++count; assert(m.t>=.5 && m.t<=1.5 && m.mat);
        }
    }
    double ratio=count/4096.;
    assert(std::fabs(ratio-(1-std::exp(-1)))<.04);
    hittable_list world, lights;
    world.add(body);
    auto lamp=make_shared<quad>(point3(-1,1,-3),vec3(2,0,0),vec3(0,0,2),
                               make_shared<diffuse_light>(color(3,3,3)));
    world.add(lamp);lights.add(lamp);
    bvh_node accel(world);
    camera cam;cam.image_width=8;cam.samples_per_pixel=4;cam.max_depth=4;
    cam.background=color(.1,.15,.2);
    cam.lookfrom=point3(0,0,0);cam.lookat=point3(0,0,-2);
    std::ostringstream out;auto old=std::cout.rdbuf(out.rdbuf());
    std::srand(1234);cam.render(accel,lights);std::cout.rdbuf(old);
    std::istringstream image(out.str());std::string magic;int w,hh,max;
    image>>magic>>w>>hh>>max;assert(magic=="P3" && w==8 && hh==8 && max==255);
    int n=0,v,total=0;
    while(image>>v){assert(v>=0 && v<=255);++n;total+=v;}
    assert(n==192 && total>0);
    std::cout<<"GEOMETRY nonunit_t="<<h.t<<"\n";
    std::cout<<"MEDIUM scalar_hits="<<count<<"/4096 expected_probability="<<1-std::exp(-1)<<"\n";
    std::cout<<"CAMERA P3 8 8 channels="<<n<<" sum="<<total<<"\nBASELINE_COMPLETE\n";
}
