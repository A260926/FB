#include "trimesh.h"
#include <cassert>
#include <set>
#include <cstdio>
using namespace trimesh;
static triangle_t tri(long a,long b,long c){triangle_t t;t.v[0]=a;t.v[1]=b;t.v[2]=c;return t;}
static void build(trimesh_t& m,unsigned n,std::vector<triangle_t>& t){std::vector<edge_t> e;unordered_edges_from_triangles(t.size(),&t[0],e);m.build(n,t.size(),&t[0],e.size(),&e[0]);}
int main(){
 std::vector<triangle_t> t;t.push_back(tri(0,1,2));t.push_back(tri(2,1,3));
 trimesh_t m;build(m,4,t);
 assert(m.boundary_vertices().size()==4 && m.boundary_edges().size()==4);
 assert(m.vertex_valence(0)==2 && m.vertex_valence(1)==3);
 assert(m.vertex_face_neighbors(1).size()==2);
 long h=m.directed_edge2he_index(1,2);assert(h>=0);
 const trimesh_t::halfedge_t& a=m.halfedge(h);const trimesh_t::halfedge_t& b=m.halfedge(a.opposite_he);
 assert(a.face==0 && b.face==1 && b.opposite_he==h);
 assert(m.he_index2directed_edge(a.next_he)==std::make_pair(2L,0L));
 assert(m.directed_edge2he_index(0,3)==-1);
 puts("HALFEDGE_DISK_ADJACENCY_OK");
 t.clear();t.push_back(tri(0,2,1));t.push_back(tri(0,1,3));t.push_back(tri(1,2,3));t.push_back(tri(2,0,3));
 build(m,4,t);assert(m.boundary_edges().empty() && m.boundary_vertices().empty());
 for(long v=0;v<4;v++){assert(m.vertex_valence(v)==3 && !m.vertex_is_boundary(v));assert(m.vertex_face_neighbors(v).size()==3);}
 for(long h0=0;h0<12;h0++){const auto& q=m.halfedge(h0);assert(m.halfedge(q.opposite_he).opposite_he==h0);long nx=m.halfedge(m.halfedge(q.next_he).next_he).next_he;assert(nx==h0);}
 puts("HALFEDGE_CLOSED_SHELL_OK");
 m.clear();build(m,4,t);assert(m.vertex_valence(3)==3);
 puts("HALFEDGE_CLEAR_REBUILD_OK");return 0;
}
