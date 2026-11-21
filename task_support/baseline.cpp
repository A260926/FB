#include <cinolib/meshes/trimesh.h>
#include <cinolib/tangent_space.h>
#include <cinolib/vector_field.h>
#include <cinolib/homotopy_basis.h>
#include <cmath>
#include <iostream>
#include <cstdio>
int main(){
 using namespace cinolib;
 const double pi=std::acos(-1.0);const unsigned N=7,M=5;
 std::vector<vec3d> p;std::vector<uint> f;
 for(unsigned i=0;i<N;++i)for(unsigned j=0;j<M;++j){double u=2*pi*i/N,v=2*pi*j/M;p.emplace_back((3+std::cos(v))*std::cos(u),(3+std::cos(v))*std::sin(u),std::sin(v));}
 for(unsigned i=0;i<N;++i)for(unsigned j=0;j<M;++j){uint a=i*M+j,b=((i+1)%N)*M+j,c=((i+1)%N)*M+(j+1)%M,d=i*M+(j+1)%M;f.insert(f.end(),{a,b,c,a,c,d});}
 Trimesh<> m(p,f);
 if(m.Euler_characteristic()!=0||m.genus()!=1)return 1;
 for(uint e=0;e<m.num_edges();++e)if(m.edge_is_boundary(e)||!m.edge_is_manifold(e))return 2;
 HomotopyBasisData data;data.root=0;data.globally_shortest=false;data.detach_loops=false;
 homotopy_basis(m,data);
 if(data.loops.size()!=2)return 3;
 unsigned generators=0;for(uint e=0;e<m.num_edges();++e)generators+=!data.tree[e]&&!data.cotree[e];
 if(generators!=2)return 4;
 std::cout<<"CINO_TORUS V="<<m.num_verts()<<" E="<<m.num_edges()<<" F="<<m.num_polys()<<" generators="<<generators<<"\n";
 vec3d u,v;
 const vec3d a=m.poly_vert(0,0),b=m.poly_vert(0,1),c=m.poly_vert(0,2);
 if(!tangent_frame(a,b,c,u,v))return 5;
 vec3d n=(b-a).cross(c-a);n.normalize();double handed=u.cross(v).dot(n);
 if(std::abs(std::abs(handed)-1)>1e-10)return 6;
 std::cout<<"EXISTING_TANGENT_HANDEDNESS="<<handed<<"\n";
 VectorField field(std::vector<vec3d>{u,v});
 const char* filename=".task-build/baseline.field";
 std::remove(filename);field.serialize(filename);VectorField restored;restored.deserialize(filename);
 if(restored.rows()!=6||(restored-field).norm()>1e-12)return 7;
 std::cout<<"VECTOR_FIELD_ROUNDTRIP="<<restored.rows()/3<<"\nCINO_FIELD_BASELINE_OK\n";
}
