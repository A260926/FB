#include "mesh.hpp"
int main(){try{
 Mesh m;make_fixture(m);const double before=signed_volume(m);int nf=m.fn;
 TriEdgeCollapseQuadricParameter p;p.PreserveTopology=true;
 LocalOptimization<Mesh> session(m,&p);session.Init<Collapse>();session.SetTargetSimplices(40);
 session.DoOptimization();session.Finalize<Collapse>();double after=signed_volume(m);
 if(m.fn>=nf||std::abs(before-after)<1e-4)throw std::runtime_error("fixture did not expose unconstrained QEM drift");
 std::cout<<std::setprecision(17)<<"BASELINE_QEM volume_before="<<before<<" volume_after="<<after<<" faces="<<nf<<"->"<<m.fn<<"\n";
 for(size_t i=0;i<m.vert.size();++i)if(!m.vert[i].IsD()){const auto&p=m.vert[i].cP();std::cout<<"V "<<i<<" "<<p[0]<<" "<<p[1]<<" "<<p[2]<<"\n";}
 for(size_t i=0;i<m.face.size();++i)if(!m.face[i].IsD()){const auto&f=m.face[i];std::cout<<"F "<<i;for(int j=0;j<3;++j)std::cout<<" "<<(f.cV(j)-&m.vert[0]);std::cout<<"\n";}
 std::cout<<"BASELINE_VOLUME_CONSTRAINT_ABSENT\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
