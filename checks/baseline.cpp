// Client-side independent grids, not a patch tessellator. BSD-3-Clause, 2026.
#include <tinynurbs/tinynurbs.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>
using P = glm::dvec3;
using Surface = tinynurbs::RationalSurface<double>;
static void require(bool b, const char* message) {
    if (!b) throw std::runtime_error(message);
}
// Two ruled quarter-cylinder bands. The second runs backwards in v.
static Surface band(bool second) {
    Surface s;
    s.degree_u=1; s.degree_v=2;
    s.knots_u=second ? std::vector<double>{-4,-4,8,8} : std::vector<double>{2,2,5,5};
    s.knots_v=second ? std::vector<double>{10,10,10,14,14,14} : std::vector<double>{-3,-3,-3,7,7,7};
    s.control_points.resize(2,3); s.weights.resize(2,3);
    const P arc[] = {P(0,1,0),P(0,1,1),P(0,0,1)};
    for (unsigned i=0;i<2;++i) for (unsigned j=0;j<3;++j) {
        s.control_points(i,j)=arc[second ? 2-j : j]+P(i+(second?1:0),0,0);
        s.weights(i,j)=j==1?std::sqrt(0.5):1;
    }
    return s;
}
static P point(const Surface& s, double u, double v) {
    return tinynurbs::surfacePoint(s, s.knots_u.front()+u*(s.knots_u.back()-s.knots_u.front()),
                                 s.knots_v.front()+v*(s.knots_v.back()-s.knots_v.front()));
}
struct Grid { std::vector<P> p; std::vector<std::array<size_t,3>> triangles; };
static Grid grid(const Surface& s, unsigned nv) {
    Grid g;
    for (unsigned i=0;i<2;++i) for(unsigned j=0;j<=nv;++j) g.p.push_back(point(s,i,double(j)/nv));
    for (unsigned j=0;j<nv;++j) {
        size_t a=j,b=nv+1+j,c=b+1,d=a+1;
        g.triangles.push_back({a,b,c}); g.triangles.push_back({a,c,d});
    }
    return g;
}
static double segmentDistance(P p, P a, P b) {
    P d=b-a; double t=glm::dot(p-a,d)/glm::dot(d,d);
    return glm::length(p-(a+std::max(0.,std::min(1.,t))*d));
}
int main() {
    const auto a=band(false),b=band(true);
    require(tinynurbs::surfaceIsValid(a)&&tinynurbs::surfaceIsValid(b),"invalid fixture");
    const P mid=point(a,.5,.5);
    require(glm::length(mid-P(.5,std::sqrt(.5),std::sqrt(.5)))<1e-12,"rational arc reference");
    for(double t: {0.,.125,.25,.5,.75,1.})
        require(glm::length(point(a,1,t)-point(b,0,1-t))<1e-12,"analytic seam disagreement");
    std::cout<<"BASELINE rational quarter-cylinder and reversed domains PASS\n";
    const Grid coarse=grid(a,2),fine=grid(b,4),same=grid(b,2);
    for(unsigned j=0;j<=2;++j) require(glm::length(coarse.p[3+j]-same.p[2-j])<1e-12,"matching grids");
    require(coarse.triangles.size()==4&&fine.triangles.size()==8,"missing triangles");
    std::cout<<"BASELINE equal-resolution seam positions PASS\n";
    unsigned unmatched=0; double maxGap=0;
    for(unsigned j=0;j<=4;++j) {
        P p=fine.p[j]; bool found=false; double gap=1e30;
        for(unsigned k=0;k<=2;++k) found=found||glm::length(p-coarse.p[3+k])<1e-12;
        for(unsigned k=0;k<2;++k) gap=std::min(gap,segmentDistance(p,coarse.p[3+k],coarse.p[4+k]));
        if(!found) ++unmatched;
        maxGap=std::max(maxGap,gap);
    }
    require(unmatched==2&&maxGap>.05,"expected independent-grid chord gap");
    std::cout<<"BASELINE independent grids: 2 unmatched seam vertices\n";
    std::cout<<std::fixed<<std::setprecision(9)<<"BASELINE maximum chord gap "<<maxGap<<"\n";
}
