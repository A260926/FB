#include "GoTools/geometry/SplineSurface.h"
#include "GoTools/lrsplines2D/LRSplineSurface.h"
#include "GoTools/viewlib/vol_and_lr/LRSurfaceTesselator.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <iomanip>
using namespace Go;
int main(){
 double k[]={0,0,0,1,1,1}; std::vector<double> c;
 for(int j=0;j<3;++j)for(int i=0;i<3;++i){double w=1+0.7*i+0.3*j; c.insert(c.end(),{w*i/2,w*j/2,w*(0.2*i*j+0.1*i*i),w});}
 SplineSurface ref(3,3,3,3,k,k,c.data(),3,true); LRSplineSurface lr(&ref,1e-10);
 Point a(3),b(3);ref.point(a,.37,.43);lr.point(b,.37,.43);
 std::cout<<std::setprecision(15)<<"initial_position_error="<<a.dist(b)<<"\n";
 if(a.dist(b)>1e-10)return 2;
 auto d=lr(.37,.43,1,1);std::vector<Point> r(6);ref.point(r,.37,.43,2);
 std::cout<<"initial_mixed_derivative_error="<<d.dist(r[4])<<"\n";
 lr.refine(XFIXED,.5,0,1);lr.refine(YFIXED,.5,0,1);lr.refine(XFIXED,.25,0,.5);
 lr.point(b,.37,.43);std::cout<<"refined_position_error="<<a.dist(b)<<" basis="<<lr.numBasisFunctions()<<" elements="<<lr.numElements()<<"\n";
 LRSurfaceTesselator t(lr);t.changeRes(9,7);auto m=t.getMesh();
 if(m->numTriangles()<=0 || m->numVertices()<=0)return 3;
 double maxerr=0;
 for(int i=0;i<m->numVertices();++i){double* uv=m->paramArray()+2*i;ref.point(a,uv[0],uv[1]);Point q(m->vertexArray()+3*i,m->vertexArray()+3*i+3);maxerr=std::max(maxerr,a.dist(q));if(!std::isfinite(maxerr))return 4;}
 std::cout<<"mesh_vertices="<<m->numVertices()<<" triangles="<<m->numTriangles()<<" reference_error="<<maxerr<<"\nLR_CPU_BASELINE_OK\n";
}
