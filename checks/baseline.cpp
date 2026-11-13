#include "pmp/algorithms/parameterization.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <iomanip>
using namespace pmp;
int main(){try{
 for(bool uniform:{true,false}){
 SurfaceMesh m;Vertex v[3][8];
 for(int r=0;r<3;++r)for(int i=0;i<8;++i){double a=i*std::acos(-1.)/4.,rad=2.-.5*r;v[r][i]=m.add_vertex(Point(rad*std::cos(a),rad*std::sin(a),0));}
 for(int r=0;r<2;++r)for(int i=0;i<8;++i){int j=(i+1)%8;m.add_triangle(v[r][i],v[r][j],v[r+1][j]);m.add_triangle(v[r][i],v[r+1][j],v[r+1][i]);}
 auto tex=m.vertex_property<TexCoord>("v:tex");
 for(auto x:m.vertices())tex[x]=TexCoord(m.position(x)[0],m.position(x)[1]);
 harmonic_parameterization(m,uniform);
 double spread=0;for(int i=1;i<8;++i)spread=std::max(spread,double(norm(tex[v[2][i]]-tex[v[2][0]])));
 if(spread>1e-10)throw std::runtime_error("expected current first-loop behavior");
 std::cout<<std::setprecision(17)<<"BASELINE_HARMONIC uniform="<<uniform<<" inner_boundary_spread="<<spread<<" faces="<<m.n_faces()<<"\n";
 }
 std::cout<<"BASELINE_EXISTING_UV_NOT_PRESERVED\n";
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
