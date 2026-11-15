#include "MeshDecimationTools.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstdint>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"baseline failure: %s:%d %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
int main(){
  constexpr uint32_t N=17;
  std::vector<float> v;std::vector<uint32_t> indices;
  for(uint32_t y=0;y<N;y++)for(uint32_t x=0;x<N;x++){
    float u=float(x)/(N-1),w=float(y)/(N-1);
    v.insert(v.end(),{u,w,0.02f*u*w,u,w});
  }
  for(uint32_t y=0;y<N-1;y++)for(uint32_t x=0;x<N-1;x++){
    uint32_t a=y*N+x,b=a+1,c=a+N,d=c+1;
    indices.insert(indices.end(),{a,b,d,a,d,c});
  }
  MdtTriangleGeometryDesc g={indices.data(),v.data(),uint32_t(indices.size()),N*N};
  MdtTriangleMeshDesc mesh={};mesh.geometry_descs=&g;mesh.geometry_desc_count=1;
  mesh.vertex_stride_bytes=5*sizeof(float);mesh.geometric_weight=0.5f;
  MdtLevelOfDetailTargetDesc levels[2]={{256,FLT_MAX},{128,FLT_MAX}};
  MdtDiscreteLodBuildInputs di={};di.mesh=mesh;di.level_of_detail_descs=levels;di.level_of_detail_count=2;
  MdtDiscreteLodBuildResult dr={};MdtBuildDiscreteLod(&di,&dr,nullptr);
  REQUIRE(dr.level_of_detail_count==2 && dr.geometry_desc_count==2 && dr.index_count>0 && dr.vertex_count>0);
  uint32_t face_counts[2]={};
  for(uint32_t l=0;l<2;l++){
    const auto& r=dr.level_of_detail_descs[l];REQUIRE(r.end_geometry_index-r.begin_geometry_index==1);
    const auto& d=dr.geometry_descs[r.begin_geometry_index];REQUIRE(d.end_indices_index<=dr.index_count);
    face_counts[l]=(d.end_indices_index-d.begin_indices_index)/3;
    REQUIRE(face_counts[l]>0 && face_counts[l]<=levels[l].target_face_count);
  }
  REQUIRE(face_counts[1]<=face_counts[0] && face_counts[0]<indices.size()/3);
  for(uint32_t i=0;i<dr.index_count;i++)REQUIRE(dr.indices[i]<dr.vertex_count);
  for(uint32_t i=0;i<dr.vertex_count*5;i++)REQUIRE(std::isfinite(dr.vertices[i]));
  std::printf("MDT_DLOD source_faces=%zu levels=%u faces0=%u faces1=%u vertices=%u\n",indices.size()/3,dr.level_of_detail_count,face_counts[0],face_counts[1],dr.vertex_count);
  MdtFreeDiscreteLodBuildResult(&dr,nullptr);
  MdtContinuousLodBuildInputs ci={};ci.mesh=mesh;ci.meshlet_target_triangle_count=16;ci.meshlet_target_vertex_count=24;
  MdtContinuousLodBuildResult cr={};MdtBuildContinuousLod(&ci,&cr,nullptr);
  REQUIRE(cr.level_count>=2 && cr.meshlet_count>0 && cr.meshlet_group_count>0 && cr.vertex_count>0);
  for(uint32_t i=0;i<cr.meshlet_vertex_index_count;i++)REQUIRE(cr.meshlet_vertex_indices[i]<cr.vertex_count);
  for(uint32_t i=0;i<cr.meshlet_count;i++){
    const auto&m=cr.meshlets[i];
    REQUIRE(m.begin_vertex_indices_index<m.end_vertex_indices_index && m.end_vertex_indices_index<=cr.meshlet_vertex_index_count);
    REQUIRE(m.begin_meshlet_triangles_index<m.end_meshlet_triangles_index && m.end_meshlet_triangles_index<=cr.meshlet_triangle_count);
    uint32_t nv=m.end_vertex_indices_index-m.begin_vertex_indices_index;
    REQUIRE(nv<=24 && m.end_meshlet_triangles_index-m.begin_meshlet_triangles_index<=16);
    for(uint32_t j=m.begin_meshlet_triangles_index;j<m.end_meshlet_triangles_index;j++){
      auto t=cr.meshlet_triangles[j];REQUIRE(t.i0<nv && t.i1<nv && t.i2<nv);
    }
  }
  for(uint32_t i=0;i<cr.vertex_count*5;i++)REQUIRE(std::isfinite(cr.vertices[i]));
  std::printf("MDT_CLOD levels=%u meshlets=%u groups=%u vertices=%u triangles=%u\n",cr.level_count,cr.meshlet_count,cr.meshlet_group_count,cr.vertex_count,cr.meshlet_triangle_count);
  MdtFreeContinuousLodBuildResult(&cr,nullptr);
  std::puts("MDT_BASELINE_COMPLETE");
  return 0;
}
