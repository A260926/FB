#include <cstdint>
#include "SunSky.hpp"
#include <cstdio>
#include <cmath>
int main() {
    SSLib::SkyHosek sky;
    sky.Update(Vec3f(0.6f,0.0f,0.8f),3.0f,Vec3f(0.7f,0.1f,0.25f),0.15f);
    SSLib::SkyTable table; table.FindThetaGammaTables(sky);
    SSLib::SkyBRDF brdf; brdf.FindBRDFTables(table,sky);
    Vec3f d=norm(Vec3f(0.2f,0.3f,0.9f));
    Vec3f raw=sky.SkyRGB(d), tab=table.SkyRGB(sky,d), conv=brdf.ConvolvedSkyRGB(sky,d,1.0f);
    for(int i=0;i<3;++i) if(!std::isfinite(raw[i]) || !std::isfinite(tab[i]) || !std::isfinite(conv[i]) || raw[i]<=0) return 1;
    std::printf("HOSEK_RAW %.6f %.6f %.6f\n",raw.x,raw.y,raw.z);
    std::printf("HOSEK_TABLE %.6f %.6f %.6f\n",tab.x,tab.y,tab.z);
    std::printf("LEGACY_BRDF %.6f %.6f %.6f\n",conv.x,conv.y,conv.z);
    std::puts("BASELINE_COMPLETE");
}
