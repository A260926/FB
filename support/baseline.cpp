#include "rtweekend.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "bvh.h"
#include <sstream>
#include <stdexcept>
#include <iomanip>
static void check(bool ok) { if (!ok) throw std::runtime_error("old path mismatch"); }
static bool near(double a,double b) { return std::fabs(a-b)<1e-11; }
int main() {
    auto matte=make_shared<lambertian>(color(.2,.4,.6));
    sphere s(point3(0,0,-2),1,matte);
    hit_record rec;
    ray r(point3(0,0,0),vec3(0,0,-2),.25);
    check(s.hit(r,interval(.001,infinity),rec));
    check(near(rec.t,.5)&&rec.front_face&&near(rec.u,.25)&&near(rec.v,.5));
    check(near(rec.normal.z(),1));
    auto q=make_shared<quad>(point3(-1,-1,-2),vec3(2,0,0),vec3(0,2,0),matte);
    rotate_y rotated(q,90); translate moved(make_shared<rotate_y>(q,90),vec3(1,0,0));
    check(moved.hit(ray(point3(0,0,0),vec3(-1,0,0)),interval(.001,infinity),rec));
    check(near(rec.p.x(),-1)&&near(rec.normal.x(),1));
    s.hit(r,interval(.001,infinity),rec);
    scatter_record out;
    metal mirror(color(.7,.8,.9),0);
    mirror.scatter(r,rec,out);
    check(out.skip_pdf&&near(out.skip_pdf_ray.direction().z(),1));
    check(near(out.skip_pdf_ray.time(),.25));
    dielectric glass(1.5); glass.scatter(r,rec,out);
    check(out.skip_pdf&&near(out.skip_pdf_ray.direction().length(),1));
    hittable_list world,lights;
    world.add(make_shared<quad>(point3(-100,-100,-1),vec3(200,0,0),vec3(0,200,0),
                               make_shared<diffuse_light>(color(.25,.5,1))));
    camera cam;cam.image_width=8;cam.aspect_ratio=1;cam.samples_per_pixel=1;
    cam.max_depth=2;cam.background=color(0,0,0);cam.defocus_angle=0;
    std::ostringstream image;auto old=std::cout.rdbuf(image.rdbuf());
    cam.render(world,lights);std::cout.rdbuf(old);
    std::istringstream input(image.str());std::string magic;int w,h,maxv;
    input>>magic>>w>>h>>maxv;check(magic=="P3"&&w==8&&h==8&&maxv==255);
    for(int n=0;n<64;n++){int red=-1,green=-1,blue=-1;input>>red>>green>>blue;
        check(red==128&&green==181&&blue==255);}
    std::string extra;check(!(input>>extra));
    std::cout<<"RTW_BASELINE_GEOMETRY_OK sphere quad transformed mirror glass\n";
    std::cout<<"RTW_BASELINE_RENDER_OK 8x8 emitted=(128,181,255)\n";
}
