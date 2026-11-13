// Fixture, MIT: directly invokes original UV preparation and interpolation.
#include <iterator>
#define main upstream_uv_main
#include "../examples/uv_raster/main.cc"
#undef main
#include <cassert>
int main(){example::Mesh m;m.triangulated_indices={0,1,2};m.facevarying_uvs={0,0,1,0,0,1};example::SetupVerticesForUVRaster(&m);assert(m.uv_vertices.size()==9);assert(m.uv_vertices[3]==1);nanort::TriangleMesh<float> a(m.uv_vertices.data(),m.uv_face_indices.data(),12);nanort::TriangleSAHPred<float> p(m.uv_vertices.data(),m.uv_face_indices.data(),12);nanort::BVHAccel<float>b;assert(b.Build(1,a,p));nanort::TriangleIntersector<float>i(a);nanort::Ray<float>r;r.org[0]=.25;r.org[1]=.25;r.org[2]=1;r.dir[0]=r.dir[1]=0;r.dir[2]=-1;r.min_t=0;r.max_t=2;nanort::TriangleIntersection<float> h;assert(b.Traverse(r,i,&h));auto P=example::Lerp(float3(0,0,2),float3(4,0,2),float3(0,4,2),h.u,h.v);assert(P[0]==1&&P[1]==1&&P[2]==2);puts("NANORT_UPSTREAM_UV_POSITION=1,1,2");}
