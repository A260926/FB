#ifndef TASK_TUFTED_MESH_HPP
#define TASK_TUFTED_MESH_HPP
#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/update/topology.h>
class TVertex; class TFace;
struct TTypes:vcg::UsedTypes<vcg::Use<TVertex>::AsVertexType,vcg::Use<TFace>::AsFaceType>{};
class TVertex:public vcg::Vertex<TTypes,vcg::vertex::Coord3d,vcg::vertex::BitFlags>{};
class TFace:public vcg::Face<TTypes,vcg::face::VertexRef,vcg::face::FFAdj,vcg::face::BitFlags>{};
class TMesh:public vcg::tri::TriMesh<std::vector<TVertex>,std::vector<TFace>>{};
inline void task_mesh(TMesh& m,const std::vector<vcg::Point3d>& p,const std::vector<std::array<int,3>>& f){
 vcg::tri::Allocator<TMesh>::AddVertices(m,p.size());
 vcg::tri::Allocator<TMesh>::AddFaces(m,f.size());
 for(size_t i=0;i<p.size();++i)m.vert[i].P()=p[i];
 for(size_t i=0;i<f.size();++i)for(int j=0;j<3;++j)m.face[i].V(j)=&m.vert[f[i][j]];
 vcg::tri::UpdateTopology<TMesh>::FaceFace(m);
}
#endif
