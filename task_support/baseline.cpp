#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/tessellation.h>
#include <array>
#include <vector>
#include <memory>
#include <cmath>
#include <iostream>
#include <algorithm>
using namespace OpenSubdiv;
using V=std::array<double,3>;
static V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
static double norm(V a){return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);}
static double gap(V a,V b){return norm({a[0]-b[0],a[1]-b[1],a[2]-b[2]});}
int main(){
 constexpr int m=6,n=6;std::vector<double> pos;std::vector<int> faces,counts(m*n,4);
 double pi=std::acos(-1.0);
 for(int j=0;j<n;++j)for(int i=0;i<m;++i){double u=2*pi*i/m,v=2*pi*j/n;pos.insert(pos.end(),{(2+.6*std::cos(v))*std::cos(u),(2+.6*std::cos(v))*std::sin(u),.6*std::sin(v)});}
 for(int j=0;j<n;++j)for(int i=0;i<m;++i)faces.insert(faces.end(),{j*m+i,j*m+(i+1)%m,((j+1)%n)*m+(i+1)%m,((j+1)%n)*m+i});
 Far::TopologyDescriptor d;d.numVertices=m*n;d.numFaces=m*n;d.numVertsPerFace=counts.data();d.vertIndicesPerFace=faces.data();
 auto opt=Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Options(Sdc::SCHEME_CATMARK);
 std::unique_ptr<Far::TopologyRefiner> ref(Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(d,opt));if(!ref)return 1;
 Bfr::RefinerSurfaceFactory<> factory(*ref);Bfr::Surface<double> s,t;
 if(!factory.InitVertexSurface(0,&s)||!factory.InitVertexSurface(1,&t))return 2;
 std::vector<double> patch(s.GetNumPatchPoints()*3),other(t.GetNumPatchPoints()*3);
 s.PreparePatchPoints(pos.data(),3,patch.data(),3);t.PreparePatchPoints(pos.data(),3,other.data(),3);
 V p,du,dv,duu,duv,dvv;double uv[2]={.37,.41};s.Evaluate(uv,patch.data(),3,p.data(),du.data(),dv.data(),duu.data(),duv.data(),dvv.data());
 for(auto a:{p,du,dv,duu,duv,dvv})for(double x:a)if(!std::isfinite(x))return 3;
 if(norm(cross(du,dv))<1e-6||norm(duu)+norm(dvv)<1e-3)return 4;
 double eps=1e-5;V plus,minus,fd;double ua[2]={uv[0]+eps,uv[1]},ub[2]={uv[0]-eps,uv[1]};s.Evaluate(ua,patch.data(),3,plus.data());s.Evaluate(ub,patch.data(),3,minus.data());for(int k=0;k<3;++k)fd[k]=(plus[k]-minus[k])/(2*eps);
 double error=gap(fd,du);if(error>1e-7)return 5;
 double a[2]={1,.37},b[2]={0,.37};V pa,pb; s.Evaluate(a,patch.data(),3,pa.data());t.Evaluate(b,other.data(),3,pb.data());double seam=gap(pa,pb);if(seam>1e-10)return 6;
 int rates[5]={2,3,4,3,3};Bfr::Tessellation tess(s.GetParameterization(),5,rates);if(!tess.IsValid()||tess.GetFacetSize()!=3)return 7;
 std::vector<double> coords(2*tess.GetNumCoords());std::vector<int> tris(3*tess.GetNumFacets());tess.GetCoords(coords.data());tess.GetFacets(tris.data());
 for(int v:tris)if(v<0||v>=tess.GetNumCoords())return 8;
 for(int i=0;i<tess.GetNumCoords();++i){V x;s.Evaluate(&coords[2*i],patch.data(),3,x.data());for(double z:x)if(!std::isfinite(z))return 9;}
 // Prerequisite check only: discover real Bfr support and compare a
 // fresh point evaluation against independently accumulated limit weights.
 // This does not implement sessions, dirty propagation, stable IDs or deltas.
 auto edited=pos;edited[2]+=.125;
 std::vector<int> affected;int nonIncidentChanged=0;double maxStencilError=0;
 auto incident=ref->GetLevel(0).GetVertexFaces(0);
 for(int f=0;f<m*n;++f){
   Bfr::Surface<double> face;if(!factory.InitVertexSurface(f,&face))return 10;
   std::vector<int> ids(face.GetNumControlPoints());face.GetControlPointIndices(ids.data());
   bool supported=std::find(ids.begin(),ids.end(),0)!=ids.end();
   if(supported)affected.push_back(f);
   bool adjacent=false;for(int x:incident)if(x==f)adjacent=true;
   std::vector<double> oldPatch(face.GetNumPatchPoints()*3),newPatch(oldPatch.size());
   face.PreparePatchPoints(pos.data(),3,oldPatch.data(),3);
   face.PreparePatchPoints(edited.data(),3,newPatch.data(),3);
   V oldP,newP,weighted={0,0,0};face.Evaluate(uv,oldPatch.data(),3,oldP.data());face.Evaluate(uv,newPatch.data(),3,newP.data());
   std::vector<double> w(ids.size());if(face.EvaluateStencil(uv,w.data())!=(int)ids.size())return 11;
   for(size_t j=0;j<ids.size();++j)for(int k=0;k<3;++k)weighted[k]+=w[j]*edited[3*ids[j]+k];
   maxStencilError=std::max(maxStencilError,gap(weighted,newP));
   if(!supported&&gap(oldP,newP)>1e-12)return 12;
   if(supported&&!adjacent&&gap(oldP,newP)>1e-6)++nonIncidentChanged;
 }
 if(affected.empty()||affected.size()>=m*n||!nonIncidentChanged||maxStencilError>1e-10)return 13;
 // Rebuild the actual neighboring template under an outer-rate increase
 // and decrease, without replacing either face's own inner rate.
 for(int rate:{2,4,2}){
   int rr[5]={2,rate,2,2,2};Bfr::Tessellation tt(s.GetParameterization(),5,rr);
   if(!tt.IsValid())return 14;
   std::vector<double> cc(tt.GetNumCoords()*2);std::vector<int> ff(tt.GetNumFacets()*3);
   tt.GetCoords(cc.data());tt.GetFacets(ff.data());
   for(int ix:ff)if(ix<0||ix>=tt.GetNumCoords())return 15;
   int sites=0;for(int j=0;j<tt.GetNumCoords();++j){
     if(std::abs(cc[2*j]-1)<1e-12)++sites;
     V x;s.Evaluate(&cc[2*j],patch.data(),3,x.data());for(double z:x)if(!std::isfinite(z))return 16;
   }
   if(sites!=rate+1)return 17;
 }
 std::cout<<"SUPPORT_LOCALITY faces="<<m*n<<" affected="<<affected.size()<<" nonincident_changed="<<nonIncidentChanged<<" stencil_error="<<maxStencilError<<"\nRATE_TRANSITION_OK 2->4->2\n";
 std::cout<<"BASE_TORUS faces="<<ref->GetLevel(0).GetNumFaces()<<" samples="<<tess.GetNumCoords()<<" triangles="<<tess.GetNumFacets()<<" du_error="<<error<<" seam_error="<<seam<<" curvature_jet="<<norm(duu)+norm(dvv)<<"\nPRODUCTION_JET_OK\n";
}
