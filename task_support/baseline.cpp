#include <GEL/Geometry/HGrid.h>
#include <GEL/HMesh/polygonize.h>
#include <cstdio>
#include <cstdlib>
static void require(bool b){if(!b)std::abort();}
int main(){
 using namespace CGLA;using namespace Geometry;
 HGrid<float> sparse(Vec3i(9,10,11),-1.f);require(sparse.get_cell(0).is_coalesced());
 sparse.store(Vec3i(8,9,10),2.f);require(sparse[Vec3i(8,9,10)]==2.f);require(sparse[Vec3i(0)]==-1.f);
 require(!sparse.get_cell(sparse.get_top_dims()[0]*sparse.get_top_dims()[1]*sparse.get_top_dims()[2]-1).is_coalesced());
 sparse.clear();require(sparse[Vec3i(8,9,10)]==-1.f);require(sparse.get_cell(7).is_coalesced());
 RGrid<float> g(Vec3i(7),-1.f);g.store(Vec3i(3),1.f);XForm xf(Vec3i(7));
 for(bool dual:{false,true}){
  auto m=HMesh::volume_polygonize(xf,g,0.f,false,true,dual);
  require(m.no_vertices()>0 && m.no_faces()>0);require(HMesh::valid(m));
  std::printf("DENSE_SURFACE_OK dual=%d vertices=%zu faces=%zu\n",int(dual),m.no_vertices(),m.no_faces());
 }
 std::puts("HGRID_STORE_CLEAR_OK partial_dims=9,10,11");
}
