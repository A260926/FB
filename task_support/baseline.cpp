#include <cinolib/meshes/trimesh.h>
#include <iostream>
#include <set>
#include <stdexcept>
using cinolib::Trimesh;using cinolib::vec3d;
int main(){
 const unsigned B=7;
 std::vector<vec3d> v;std::vector<unsigned> f;
 for(unsigned y=0;y<B;++y)for(unsigned x=0;x<B;++x)v.emplace_back(x,y,0.02*(x*x+2*y*y));
 for(unsigned y=0;y+1<B;++y)for(unsigned x=0;x+1<B;++x){unsigned a=x+B*y;f.insert(f.end(),{a,a+1,a+B+1,a,a+B+1,a+B});}
 Trimesh<> m(v,f);unsigned startv=m.num_verts(),startf=m.num_polys();
 for(unsigned p=0;p<m.num_polys();++p)m.poly_data(p).label=1000+p;
 std::cout<<"CINO_START vertices="<<startv<<" faces="<<startf<<" euler="<<m.Euler_characteristic()<<"\n";
 for(unsigned step=0;step<3;++step){bool done=false;
  for(unsigned e=0;e<m.num_edges();++e){if(m.edge_is_incident_to_boundary(e))continue;
   if(m.edge_collapse(e,0.35,true,true)>=0){done=true;break;}}
  if(!done)throw std::runtime_error("production collapse stalled");
  if(m.Euler_characteristic()!=1)throw std::runtime_error("Euler changed");
  for(unsigned p=0;p<m.num_polys();++p){auto ids=m.poly_verts_id(p);if(ids.size()!=3)throw std::runtime_error("not a triangle");for(unsigned i:ids)if(i>=m.num_verts())throw std::runtime_error("invalid corner");if(m.poly_area(p)<=0)throw std::runtime_error("zero area");}
  std::set<int> labels;unsigned changed=0;for(unsigned p=0;p<m.num_polys();++p){labels.insert(m.poly_data(p).label);changed+=(m.poly_data(p).label!=int(1000+p));}
  std::cout<<"CINO_STEP step="<<step+1<<" vertices="<<m.num_verts()<<" faces="<<m.num_polys()<<" missing_source_faces="<<startf-labels.size()<<" changed_dense_face_slots="<<changed<<" euler="<<m.Euler_characteristic()<<"\n";
 }
 if(m.num_verts()!=startv-3||m.num_polys()!=startf-6)throw std::runtime_error("collapse counts");
 std::cout<<"CINO_BASELINE_OK\n";
}
