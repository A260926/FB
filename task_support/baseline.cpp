#include <tinynurbs/tinynurbs.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
using tinynurbs::RationalSurface3d;
static void need(bool b) { if(!b) throw std::runtime_error("baseline assertion"); }
static RationalSurface3d strip(double x0,double ua,double ub,double va,double vb) {
    RationalSurface3d s; s.degree_u=2;s.degree_v=2;
    s.knots_u={ua,ua,ua,ub,ub,ub}; s.knots_v={va,va,va,vb,vb,vb};
    s.control_points.resize(3,3);s.weights.resize(3,3);
    const double w[]={.8,1.3,1.};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){
        s.control_points(i,j)=glm::dvec3(x0+.5*i,.5*j,0);
        s.weights(i,j)=w[j];
    }
    return s;
}
int main() {
    auto a=strip(0,-3,5,10,12),b=strip(1,20,24,-2,4);
    const double t=.37;
    auto pa=tinynurbs::surfacePoint(a,5.,10.+2*t);
    auto pb=tinynurbs::surfacePoint(b,20.,-2.+6*t);
    auto da=tinynurbs::surfaceDerivatives(a,1,5.,10.+2*t);
    auto db=tinynurbs::surfaceDerivatives(b,1,20.,-2.+6*t);
    const double c0=glm::length(pa-pb);
    const double c1=glm::length(-8.*da(1,0)+4.*db(1,0));
    need(c0<1e-12 && c1<1e-12);
    auto refined=tinynurbs::surfaceKnotInsertV(a,10.8,1);
    double refine_error=glm::length(tinynurbs::surfacePoint(refined,1.,10.7)-tinynurbs::surfacePoint(a,1.,10.7));
    need(refine_error<1e-12);
    auto split=tinynurbs::surfaceSplitU(a,1.);
    need(glm::length(tinynurbs::surfacePoint(std::get<0>(split),0.,10.7)-tinynurbs::surfacePoint(a,0.,10.7))<1e-12);
    for(int j=0;j<3;++j)b.control_points(1,j).z+=.2;
    auto local=tinynurbs::surfaceDerivatives(b,1,20.,-2.+6*t);
    double local_c1=glm::length(-8.*da(1,0)+4.*local(1,0));
    need(glm::length(tinynurbs::surfacePoint(b,20.,-2.+6*t)-pa)<1e-12);
    std::cout<<"ORIGINAL_RATIONAL_SEAM c0="<<c0<<" c1="<<c1<<"\n";
    std::cout<<"ORIGINAL_REFINEMENT_SPLIT_OK error="<<refine_error<<"\n";
    // This is an independent local control edit, not a call to the future editor.
    std::cout<<"LOCAL_EDIT_OBSERVATION boundary_unchanged=1 transverse_residual="<<local_c1<<"\n";
    std::cout<<"BASELINE_COMPLETE\n";
}
