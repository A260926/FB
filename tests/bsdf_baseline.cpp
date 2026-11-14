#include <glm/glm.hpp>
#include "openpbr.h"
#include <cstdio>
#include <cmath>
static bool run(float roughness, float metalness, float transmission) {
    OpenPBR_ResolvedInputs inputs=openpbr_make_default_resolved_inputs();
    inputs.base_color=vec3(0.8f,0.3f,0.1f);
    inputs.specular_roughness=roughness;
    inputs.base_metalness=metalness;
    inputs.transmission_weight=transmission;
    inputs.transmission_dispersion_scale=0;
    vec3 view=normalize(vec3(0.75f,0.0f,0.66f));
    OpenPBR_PreparedBsdf p=openpbr_prepare(inputs,vec3(1.0f),OpenPBR_BaseRgbWavelengths_nm,OpenPBR_VacuumIor,view);
    vec3 direction; OpenPBR_DiffuseSpecular weight; float pdf; OpenPBR_BsdfLobeType type;
    openpbr_sample(p,vec3(0.85f,0.65f,0.95f),direction,weight,pdf,type);
    vec3 w=openpbr_get_sum_of_diffuse_specular(weight);
    if (!(pdf>0) || !std::isfinite(pdf)) return false;
    for(int i=0;i<3;++i) if(!std::isfinite(direction[i]) || !std::isfinite(w[i])) return false;
    float query=openpbr_pdf(p,direction);
    auto value=openpbr_eval(p,direction);vec3 sum=openpbr_get_sum_of_diffuse_specular(value);
    if(!(query>0) || !std::isfinite(query) || !std::isfinite(sum.x)) return false;
    std::printf("BSDF rough=%.3f metal=%.1f trans=%.1f type=%u pdf=%.9g query=%.9g\n",roughness,metalness,transmission,unsigned(type),pdf,query);
    std::printf("DIRECTION %.9g %.9g %.9g WEIGHT %.9g %.9g %.9g\n",direction.x,direction.y,direction.z,w.x,w.y,w.z);
    if(roughness==0) {
        if(!(type&OpenPBR_BsdfLobeTypeGlossy) || (type&OpenPBR_BsdfLobeTypeSpecular)) return false;
        std::puts("ZERO_ROUGHNESS_CURRENTLY_GLOSSY");
    }
    return true;
}
int main(int argc,char**){if(!run(0.4f,0,0)) return 1; if(argc>1 && (!run(0,1,0)||!run(0,0,1))) return 1;std::puts("BASELINE_COMPLETE");}
