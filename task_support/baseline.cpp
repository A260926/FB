#include <GEL/HMesh/skeleton_to_FEQ.h>
#include <GEL/HMesh/subdivision.h>
#include <GEL/HMesh/smooth.h>
#include <GEL/HMesh/Manifold.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
using namespace CGLA;
using namespace HMesh;
static void check(bool p){if(!p)throw std::runtime_error("production baseline assertion");}
int main(){try{
 std::vector<Vec3d> points={{0,0,0},{2,0,0},{2,1,0},{0,1,0},{0,0,1.5},{2,0,1.5},{2,1,1.5},{0,1,1.5}};
 std::vector<size_t> faces={0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
 Manifold source; build_manifold(source,points,faces,3);check(valid(source));
 Manifold target=source;
 loop_split(target);loop_split(target);
 for(auto v:target.vertices())target.pos(v)+=Vec3d(.15,.1,-.05);
 non_rigid_registration(source,target);
 double displacement=0.;for(auto v:source.vertices()){auto p=source.pos(v);for(int k=0;k<3;++k)check(std::isfinite(p[k]));displacement+=length(p-points[v.get_index()]);}
 check(displacement>0. && valid(source));
 std::printf("LEGACY_REGISTRATION vertices=%zu target_vertices=%zu displacement_sum=%.9g\n",source.no_vertices(),target.no_vertices(),displacement);
 laplacian_smooth(source,0.05f,1);check(valid(source));
 double norm=0.;for(auto v:source.vertices()){for(int k=0;k<3;++k)check(std::isfinite(source.pos(v)[k]));norm+=sqr_length(source.pos(v));}
 std::printf("LEGACY_SMOOTH faces=%zu position_norm2=%.9g\n",source.no_faces(),norm);
 std::puts("BASELINE_COMPLETE");return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
