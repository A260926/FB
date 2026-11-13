#include "opensubdiv/far/topologyDescriptor.h"
#include "opensubdiv/far/topologyRefinerFactory.h"
#include "opensubdiv/far/primvarRefiner.h"
#include "opensubdiv/sdc/crease.h"
#include <vector>
#include <memory>
#include <cmath>
#include <cstdio>
using namespace OpenSubdiv;
struct Value{double v=0;void Clear(){v=0;}void AddWithWeight(const Value&a,double w){v+=w*a.v;}};
int main(){int counts[]={4,4,4,4,4,4},ids[]={0,1,3,2,2,3,5,4,4,5,7,6,6,7,1,0,1,7,5,3,6,0,2,4},edges[]={0,1,1,3};float weights[]={1.5f,2.25f};Far::TopologyDescriptor d;d.numVertices=8;d.numFaces=6;d.numVertsPerFace=counts;d.vertIndicesPerFace=ids;d.numCreases=2;d.creaseVertexIndexPairs=edges;d.creaseWeights=weights;
 Sdc::Options so;so.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);std::unique_ptr<Far::TopologyRefiner> r(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(d,{Sdc::SCHEME_CATMARK,so}));if(!r)return 1;r->RefineUniform(Far::TopologyRefiner::UniformOptions(2));Far::PrimvarRefinerReal<double> p(*r);std::vector<Value>a(8),b;for(auto&v:a)v.v=1;
 for(int level=1;level<=2;++level){b.resize(r->GetLevel(level).GetNumVertices());auto*src=a.data();auto*dst=b.data();p.Interpolate(level,src,dst);for(auto&v:b)if(fabs(v.v-1)>1e-12)return 2;a.swap(b);}
 float ps[]={.3f,.7f,0},cs[]={0,0,0};Sdc::Crease c(so);if(fabs(c.ComputeFractionalWeightAtVertex(0,0,3,ps,cs)-.5)>1e-7)return 3;if(a.size()!=98)return 4;std::puts("OPENSUBDIV_BASELINE level2=98 constant=preserved mixed-transition=0.5");}
