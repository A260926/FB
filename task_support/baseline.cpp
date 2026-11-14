#include <OpenMesh/Core/Mesh/PolyMesh_ArrayKernelT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Subdivider/Uniform/CatmullClarkT.hh>
#include <OpenMesh/Tools/Subdivider/Uniform/LoopT.hh>
#include <iostream>
#include <stdexcept>
#include <vector>
template<class M, class S> void exercise(const char* name, int n) {
 M m; m.request_halfedge_texcoords2D();
 std::vector<typename M::VertexHandle> vs;
 for(int i=0;i<n;i++) vs.push_back(m.add_vertex(typename M::Point(i==1||i==2, i>=2,0)));
 auto f=m.add_face(vs); if(!f.is_valid()) throw std::runtime_error("face");
 auto h=m.halfedge_handle(f); int old_to=m.to_vertex_handle(h).idx();
 for(auto it=m.halfedges_begin();it!=m.halfedges_end();++it) m.set_texcoord2D(*it, typename M::TexCoord2D(10+it->idx(),20+it->idx()));
 auto uv=m.texcoord2D(h);
 S s; if(!s.attach(m)||!s(1)) throw std::runtime_error("subdivide"); s.detach();
 if(m.n_faces()!=size_t(n==4?4:4)||m.n_vertices()!=size_t(n==4?9:6)) throw std::runtime_error("topology");
 std::cout<<name<<" vertices="<<m.n_vertices()<<" faces="<<m.n_faces()<<" old_halfedge_to="<<old_to<<" now_to="<<m.to_vertex_handle(h).idx()<<" uv_before="<<uv<<" uv_after="<<m.texcoord2D(h)<<"\n";
}
int main(){try{
 exercise<OpenMesh::PolyMesh_ArrayKernelT<>,OpenMesh::Subdivider::Uniform::CatmullClarkT<OpenMesh::PolyMesh_ArrayKernelT<> > >("CATMULL",4);
 exercise<OpenMesh::TriMesh_ArrayKernelT<>,OpenMesh::Subdivider::Uniform::LoopT<OpenMesh::TriMesh_ArrayKernelT<> > >("LOOP",3);
 std::cout<<"OPENMESH_BASELINE_OK\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
