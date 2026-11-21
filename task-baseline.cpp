#include <cstdio>
#include <cstdlib>
#include "src/pathtracer.hxx"
#include "src/vertexcm.hxx"
int main(){
 Scene s; s.LoadCornellBox(Vec2i(12,12)); s.BuildSceneSphere();
 PathTracer pt(s,1234); VertexCM bpt(s,VertexCM::kBpt,0.003f,0.75f,1234), vcm(s,VertexCM::kVcm,0.003f,0.75f,1234);
 AbstractRenderer* renderers[]={&pt,&bpt,&vcm}; const char* names[]={"PT","BPT","VCM"};
 for(int k=0;k<3;++k){auto& r=*renderers[k];r.mMaxPathLength=6;for(int i=0;i<3;++i)r.RunIteration(i);Framebuffer f;r.GetFramebuffer(f);float e=f.TotalLuminance();if(!std::isfinite(e)||e<=0)return 2;std::printf("BASELINE %s iterations=3 luminance=%.9g\n",names[k],e);}
 std::puts("PRODUCTION_BASELINE_OK");
}
