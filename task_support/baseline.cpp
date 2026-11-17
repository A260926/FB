#include "opensubdiv/far/topologyDescriptor.h"
#include "opensubdiv/far/topologyRefinerFactory.h"
#include "opensubdiv/far/primvarRefiner.h"
#include "opensubdiv/far/stencilTable.h"
#include "opensubdiv/far/stencilTableFactory.h"
#include "opensubdiv/far/patchTableFactory.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace OpenSubdiv;
struct V {
 double x[3]={0,0,0};
 void Clear(){for(double& a:x)a=0;}
 void AddWithWeight(const V& v,double w){for(int i=0;i<3;++i)x[i]+=w*v.x[i];}
};
static void check(bool b,const char* m){if(!b)throw std::runtime_error(m);}
static std::unique_ptr<Far::TopologyRefiner> make(){
 static int n[]={4,4,4,4,4,4};
 static int f[]={0,3,2,1,4,5,6,7,0,1,5,4,1,2,6,5,2,3,7,6,3,0,4,7};
 Far::TopologyDescriptor d;d.numVertices=8;d.numFaces=6;d.numVertsPerFace=n;d.vertIndicesPerFace=f;
 Sdc::Options o;o.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
 Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options opts(Sdc::SCHEME_CATMARK,o);
 return std::unique_ptr<Far::TopologyRefiner>(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(d,opts));
}
int main(){
 std::vector<V> base(8);const double xyz[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1.2},{1,-1,1.3},{1,1,1.5},{-1,1,1.1}};
 for(int i=0;i<8;++i)for(int j=0;j<3;++j)base[i].x[j]=xyz[i][j];
 auto r=make();check(bool(r),"refiner");r->RefineUniform(Far::TopologyRefiner::UniformOptions(2));
 Far::PrimvarRefinerReal<double> pr(*r);std::vector<V> previous=base;
 for(int l=1;l<=2;++l){std::vector<V> next(r->GetLevel(l).GetNumVertices());pr.Interpolate(l,previous,next);previous.swap(next);}
 Far::StencilTableFactoryReal<double>::Options so;so.generateIntermediateLevels=false;so.generateOffsets=true;
 std::unique_ptr<const Far::StencilTableReal<double>> st(Far::StencilTableFactoryReal<double>::Create(*r,so));check(bool(st),"stencil");
 std::vector<V> direct(st->GetNumStencils());st->UpdateValues(base,direct);check(direct.size()==previous.size()&&direct.size()==98,"uniform row count");
 double error=0;for(size_t i=0;i<direct.size();++i)for(int j=0;j<3;++j)error=std::max(error,std::fabs(direct[i].x[j]-previous[i].x[j]));check(error<1e-12,"stencil interpolation equivalence");
 std::cout<<"SUBDIV vertices="<<direct.size()<<" stencil_error="<<error<<std::endl;
 auto a=make();Far::PatchTableFactory::Options po(5);po.SetPatchPrecision<double>();po.endCapType=Far::PatchTableFactory::Options::ENDCAP_GREGORY_BASIS;
 a->RefineAdaptive(po.GetRefineAdaptiveOptions());std::unique_ptr<const Far::PatchTable> patches(Far::PatchTableFactory::Create(*a,po));check(bool(patches),"patches");
 double u=.37,v=.41;Far::LimitStencilTableFactoryReal<double>::LocationArray loc;loc.ptexIdx=0;loc.numLocations=1;loc.s=&u;loc.t=&v;
 Far::LimitStencilTableFactoryReal<double>::LocationArrayVec locations(1,loc);
 std::unique_ptr<const Far::LimitStencilTableReal<double>> limit(Far::LimitStencilTableFactoryReal<double>::Create(*a,locations,nullptr,patches.get()));check(bool(limit)&&limit->GetNumStencils()==1,"limit");
 auto row=limit->GetLimitStencil(0);V p,du,dv;
 for(int j=0;j<row.GetSize();++j){const V& q=base[row.GetVertexIndices()[j]];p.AddWithWeight(q,row.GetWeights()[j]);du.AddWithWeight(q,row.GetDuWeights()[j]);dv.AddWithWeight(q,row.GetDvWeights()[j]);}
 double n[3]={du.x[1]*dv.x[2]-du.x[2]*dv.x[1],du.x[2]*dv.x[0]-du.x[0]*dv.x[2],du.x[0]*dv.x[1]-du.x[1]*dv.x[0]};
 double area=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);check(std::isfinite(area)&&area>1e-12,"normal area");
 for(double x:p.x)check(std::isfinite(x),"position finite");
 std::cout<<"LIMIT samples=1 position="<<p.x[0]<<","<<p.x[1]<<","<<p.x[2]<<" area="<<area<<" normal="<<n[0]/area<<","<<n[1]/area<<","<<n[2]/area<<std::endl;
 std::cout<<"FAR_BASELINE_OK\n";
}
