#include <cinolib/meshes/trimesh.h>
#include <cinolib/tangential_smoothing.h>
#include <cstdio>
#include <cmath>
static double volume(const cinolib::Trimesh<> &m){double v=0;for(unsigned i=0;i<m.num_polys();i++){auto a=m.poly_vert(i,0),b=m.poly_vert(i,1),c=m.poly_vert(i,2);v+=a.dot(b.cross(c))/6;}return v;}
int main(){
 std::vector<cinolib::vec3d> p={cinolib::vec3d(1.4,0,0),cinolib::vec3d(-1,0,0),cinolib::vec3d(0,1,0),cinolib::vec3d(0,-1.1,0),cinolib::vec3d(.2,.1,1.3),cinolib::vec3d(0,0,-1)};
 std::vector<unsigned> f={0,2,4,2,1,4,1,3,4,3,0,4,2,0,5,1,2,5,3,1,5,0,3,5};
 cinolib::Trimesh<> m(p,f);double before=volume(m);
 cinolib::tangential_smoothing(m);double after=volume(m);
 printf("octahedron vertices=%u faces=%u volume_before=%.9f volume_after=%.9f\n",m.num_verts(),m.num_polys(),before,after);
 if(m.num_polys()!=8||!std::isfinite(after)||std::abs(before-after)<1e-4)return 1;
 puts("CINOLIB_BASELINE_OK tangential-smoothing-executed");
}
