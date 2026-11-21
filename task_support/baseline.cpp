#include "source/integrator/path-tracer/path-tracer.hpp"
#include "source/material/material.hpp"
#include "source/surface/surface.hpp"
#include "source/ray/interaction.hpp"
#include "source/sampling/sampler.hpp"
#include <cmath>
#include <iostream>
static bool near(double a,double b){return std::abs(a-b)<1e-10;}
int main(){
 auto j=nlohmann::json::parse(R"({"num_render_threads":1,"materials":{"default":{"reflectance":[1,1,1]},"lamp":{"reflectance":[0,0,0],"emittance":[0.6,0.4,0.2]}},"surfaces":[{"type":"sphere","radius":1,"position":[0,0,-3],"material":"lamp"},{"type":"triangle","vertices":[[3,-1,-2],[5,-1,-2],[4,1,-2]],"material":"default"}]})");
 PathTracer pt(j);Ray ray({0,0,0},{0,0,-1},1);
 auto h=pt.scene.intersect(ray);if(!h||!near(h.t,2))return 1;
 Sampler::initiate(1234);Sampler::setIndex(0);
 auto c=pt.sampleRay(ray);const double area=4*std::acos(-1.0);
 if(!near(c.x,.6/area)||!near(c.y,.4/area)||!near(c.z,.2/area))return 2;
 auto sky=pt.sampleRay(Ray({0,0,0},{0,1,0},1));
 if(!near(sky.x,.25)||!near(sky.y,.5)||!near(sky.z,.75))return 3;
 Ray tr({4,0,0},{0,0,-1},1);auto ti=pt.scene.intersect(tr);if(!ti||!near(ti.t,2))return 4;
 Interaction interaction(ti,tr,1);glm::dvec3 value;double pdf;
 Sampler::initiate(5678);Sampler::setIndex(0);Sampler::shuffle();
 if(!interaction.sampleBSDF(value,pdf,tr)||pdf<=0||!near(value.x/pdf,1))return 5;
 std::cout<<"MCRT_BASELINE hit_t="<<h.t<<" emission="<<c.x<<","<<c.y<<","<<c.z<<" sky="<<sky.x<<","<<sky.y<<","<<sky.z<<" diffuse_weight="<<value.x/pdf<<"\n";
}
