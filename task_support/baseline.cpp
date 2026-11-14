#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/primvarRefiner.h>
#include <opensubdiv/far/stencilTableFactory.h>
#include <memory>
#include <vector>
#include <cmath>
#include <cstdio>
using namespace OpenSubdiv;
struct Value {
 double p[3];
 void Clear(){for(int k=0;k<3;++k)p[k]=0;}
 void AddWithWeight(Value const& v,double w){for(int k=0;k<3;++k)p[k]+=v.p[k]*w;}
};
std::vector<Value> evaluate(float sharp,int method) {
 int counts[]={4,4,4,4,4,4};
 int faces[]={0,3,2,1,4,5,6,7,0,1,5,4,1,2,6,5,2,3,7,6,3,0,4,7};
 int creases[]={0,1,1,2}; float weights[]={sharp,1.4f};
 Far::TopologyDescriptor d; d.numVertices=8; d.numFaces=6;
 d.numVertsPerFace=counts;d.vertIndicesPerFace=faces;
 d.numCreases=2;d.creaseVertexIndexPairs=creases;d.creaseWeights=weights;
 Sdc::Options opt;opt.SetVtxBoundaryInterpolation(Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);
 opt.SetCreasingMethod(method?Sdc::Options::CREASE_CHAIKIN:Sdc::Options::CREASE_UNIFORM);
 Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options o(Sdc::SCHEME_CATMARK,opt);
 std::unique_ptr<Far::TopologyRefiner> r(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(d,o));
 if(!r)std::abort();
 Far::TopologyRefiner::UniformOptions u(3);u.fullTopologyInLastLevel=true;r->RefineUniform(u);
 std::vector<Value> base(8);for(int i=0;i<8;++i){base[i].p[0]=double(i*i%7);base[i].p[1]=double(i%3);base[i].p[2]=double(i*i*i%11);}
 std::vector<Value> last=base;
 Far::PrimvarRefinerReal<double> prim(*r);
 for(int l=1;l<=3;++l){std::vector<Value> next(r->GetLevel(l).GetNumVertices());prim.Interpolate(l,last,next);last.swap(next);}
 Far::StencilTableFactoryReal<double>::Options so;so.generateIntermediateLevels=false;so.generateOffsets=true;
 std::unique_ptr<const Far::StencilTableReal<double>> table(Far::StencilTableFactoryReal<double>::Create(*r,so));
 if(!table || table->GetNumStencils()!=int(last.size()))std::abort();
 std::vector<Value> via(last.size());table->UpdateValues(base,via);
 double error=0;for(size_t i=0;i<last.size();++i)for(int k=0;k<3;++k)error=std::fmax(error,std::fabs(last[i].p[k]-via[i].p[k]));
 if(error>1e-10)std::abort();
 std::printf("method=%s vertices=%zu stencil_error=%.3g\n",method?"chaikin":"uniform",last.size(),error);
 return last;
}
int main(){
 for(int method=0;method<2;++method){auto a=evaluate(0,method),b=evaluate(2.25f,method);double delta=0;for(size_t i=0;i<a.size();++i)for(int k=0;k<3;++k)delta=std::fmax(delta,std::fabs(a[i].p[k]-b[i].p[k]));if(!(delta>0.01))return 1;std::printf("crease_geometry_delta=%.9g\n",delta);}
 std::puts("BASELINE_OPENSUBDIV_OK");
}
