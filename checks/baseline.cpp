#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <memory>
#include <cmath>
#include <iostream>
using namespace OpenSubdiv;
int main(){
 int sizes[]={4,4,4,4,4,4};
 int faces[]={0,3,2,1, 4,5,6,7, 0,1,5,4, 1,2,6,5, 2,3,7,6, 3,0,4,7};
 double x[]={-1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1, -1,-1,1, 1,-1,1, 1,1,1, -1,1,1};
 Far::TopologyDescriptor d;d.numVertices=8;d.numFaces=6;d.numVertsPerFace=sizes;d.vertIndicesPerFace=faces;
 using Factory=Far::TopologyRefinerFactory<Far::TopologyDescriptor>;
 std::unique_ptr<Far::TopologyRefiner> ref(Factory::Create(d,Factory::Options(Sdc::SCHEME_CATMARK)));
 if(!ref)return 1;
 Bfr::RefinerSurfaceFactory<> factory(*ref);Bfr::Surface<double> surface;
 if(!factory.InitVertexSurface(0,&surface))return 2;
 int n=surface.GetNumControlPoints();std::vector<int> ids(n);surface.GetControlPointIndices(ids.data());
 std::vector<double> p(n),u(n),v(n),patch(surface.GetNumPatchPoints()*3);
 double uv[]={.37,.42},point[3],du[3],dv[3];
 surface.PreparePatchPoints(x,3,patch.data(),3);surface.Evaluate(uv,patch.data(),3,point,du,dv);
 if(surface.EvaluateStencil(uv,p.data(),u.data(),v.data())!=n)return 3;
 double error=0;
 for(int k=0;k<3;k++){double q=0;for(int i=0;i<n;i++)q+=p[i]*x[ids[i]*3+k];error+=std::abs(q-point[k]);}
 double cross[3]={du[1]*dv[2]-du[2]*dv[1],du[2]*dv[0]-du[0]*dv[2],du[0]*dv[1]-du[1]*dv[0]};
 double area=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
 if(!std::isfinite(area)||area<1e-8||error>1e-11)return 4;
 std::cout<<"OPENSUBDIV_BASELINE_OK control_points="<<n<<" stencil_error="<<error<<"\n";
}
