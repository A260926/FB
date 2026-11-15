#include <Mathematics/AdaptiveSkeletonClimbing3.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
using ASC=gte::AdaptiveSkeletonClimbing3<int32_t,double>;
int main(int argc,char**){
  const int N=4,S=1<<N,B=S+1;
  std::vector<int32_t> data(B*B*B);
  for(int z=0;z<B;++z)for(int y=0;y<B;++y)for(int x=0;x<B;++x)
    data[x+B*(y+B*z)]=3*x+5*y+7*z;
  size_t counts[2][2]{};
  if(argc>1){ASC asc(N,data.data(),false);std::vector<ASC::Vertex> v;std::vector<ASC::Triangle> t;asc.Extract(107.5,-1,v,t);if(!v.empty()||!t.empty())throw std::runtime_error("root observation changed");std::cout<<"LEGACY_ROOT vertices=0 triangles=0\n";}
  for(int fix=0;fix<2;++fix)for(int d=0;d<2;++d){
    ASC asc(N,data.data(),fix!=0);
    std::vector<ASC::Vertex> v;std::vector<ASC::Triangle> t;
    int depth=d?N:2;
    asc.Extract(107.5,depth,v,t);asc.MakeUnique(v,t);asc.OrientTriangles(v,t,true);
    if(v.empty()||t.empty())throw std::runtime_error("empty production mesh");
    double residual=0;for(auto const&p:v)residual=std::max(residual,std::abs(3*p[0]+5*p[1]+7*p[2]-107.5));
    if(residual>1e-9)throw std::runtime_error("plane interpolation");
    for(auto const&tri:t)for(int i:tri.V)if(i<0||size_t(i)>=v.size())throw std::runtime_error("bad index");
    std::ostringstream box;asc.PrintBoxes(box);size_t boxes=0;std::istringstream(box.str())>>boxes;
    counts[fix][d]=t.size();
    std::cout<<"ASC fix="<<fix<<" depth="<<depth<<" vertices="<<v.size()<<" triangles="<<t.size()<<" boxes="<<boxes<<" residual="<<residual<<"\n";
  }
  if(!(counts[0][0]<counts[1][0]&&counts[1][0]<=counts[1][1]))throw std::runtime_error("unexpected partition comparison");
  std::cout<<"ASC_BASELINE_OK\n";
}
