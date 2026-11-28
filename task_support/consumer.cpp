#include "RenderHelp.h"
#include "tone_map.h"
#include <cmath>
int main(){
 RenderHelp r(3,1);r.BeginHDR(Vec3f(2,2,2));tone::Frame f=r.EndHDR();
 if(f.width!=3||f.height!=1||f.rgb.size()!=3||f.rgb[1][0]!=2)return 31;
 tone::Options o;auto v=tone::compress(f,o);
 if(v.linear.rgb.size()!=3||v.log_luminance.size()!=3||v.attenuation.size()!=3)return 32;
 if(std::abs(v.linear.rgb[1][0]-o.key/(1+o.key))>1e-8||v.relative_residual>o.tolerance)return 33;
 return 0;
}
