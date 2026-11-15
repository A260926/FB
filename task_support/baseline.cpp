#include "gmsh.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const char *why) { if(!ok) throw std::runtime_error(why); }
static std::vector<double> transform() { return {1,0,0,3,0,1,0,0,0,0,1,0,0,0,0,1}; }
static std::pair<int,int> geometry(bool periodic) {
  gmsh::model::add("quarter-cylinder");
  int a=gmsh::model::geo::addPoint(1,0,0,.35), c=gmsh::model::geo::addPoint(0,0,0,.35), b=gmsh::model::geo::addPoint(0,1,0,.35);
  int arc=gmsh::model::geo::addCircleArc(a,c,b);
  gmsh::vectorpair ext,cp;
  gmsh::model::geo::extrude({{1,arc}},0,0,1,ext);
  int master=-1;
  for(auto e:ext) if(e.first==2) master=e.second;
  require(master>0,"extrude did not produce a surface");
  gmsh::model::geo::copy({{2,master}},cp);
  require(cp.size()==1 && cp[0].first==2,"CAD surface copy failed");
  gmsh::model::geo::translate(cp,3,0,0);
  gmsh::model::geo::synchronize();
  if(periodic) gmsh::model::mesh::setPeriodic(2,{cp[0].second},{master},transform());
  return {master,cp[0].second};
}
static size_t pairs(int slave,bool high,bool checkCoordinates) {
  int master=0;std::vector<size_t>s,m;std::vector<double>t;
  gmsh::model::mesh::getPeriodicNodes(2,slave,master,s,m,t,high);
  require(master>0 && s.size()==m.size() && !s.empty() && t.size()==16,"missing periodic data");
  if(checkCoordinates) for(size_t i=0;i<s.size();++i) {
    std::vector<double>x,y,p;int dim,tag;
    gmsh::model::mesh::getNode(s[i],x,p,dim,tag);
    gmsh::model::mesh::getNode(m[i],y,p,dim,tag);
    require(x.size()==3&&y.size()==3,"bad node coordinates");
    double err=std::hypot(std::hypot(x[0]-y[0]-3,x[1]-y[1]),x[2]-y[2]);
    if(err>=1e-7) std::cerr<<"PAIR_BAD high="<<high<<" s="<<s[i]<<" m="<<m[i]<<" x="<<x[0]<<","<<x[1]<<","<<x[2]<<" y="<<y[0]<<","<<y[1]<<","<<y[2]<<" err="<<err<<"\n";
    require(err<1e-7,"periodic coordinate mismatch");
  }
  return s.size();
}
static size_t parameterChecks() {
  size_t checked=0;
  gmsh::vectorpair ent;gmsh::model::getEntities(ent);
  for(auto e:ent) if(e.first==1||e.first==2) {
    std::vector<size_t>n;std::vector<double>x,p,g;
    gmsh::model::mesh::getNodes(n,x,p,e.first,e.second,false,true);
    require(p.size()==n.size()*size_t(e.first),"missing CAD parameters");
    gmsh::model::getValue(e.first,e.second,p,g);
    require(g.size()==x.size(),"invalid CAD evaluation");
    for(size_t i=0;i<x.size();++i) require(std::abs(g[i]-x[i])<1e-8,"CAD parameter back-evaluation mismatch");
    checked+=n.size();
  }
  require(checked>0,"no classified CAD nodes checked");return checked;
}
int main(int argc,char **argv) {
  try {
    gmsh::initialize();gmsh::option::setNumber("General.Terminal",0);
    gmsh::option::setNumber("General.NumThreads",1);
    gmsh::option::setNumber("Mesh.SaveAll",1);
    gmsh::option::setNumber("Mesh.SaveParametric",1);
    gmsh::option::setNumber("Mesh.MshFileVersion",4.1);
    gmsh::option::setNumber("Mesh.HighOrderSavePeriodic",1);
    auto f=geometry(true);gmsh::model::mesh::generate(2);
    size_t low=pairs(f.second,false,true);
    gmsh::model::mesh::setOrder(3);
    size_t all=pairs(f.second,true,true),low3=pairs(f.second,false,true);
    require(low==low3&&all>low,"fresh high-order periodic correspondence failed");
    size_t pc=parameterChecks();
    std::string prefix=argc>1?argv[1]:".task-build/normal";
    for(int binary=0;binary<2;++binary) {
      gmsh::option::setNumber("Mesh.Binary",binary);
      gmsh::write(prefix+(binary?"-binary.msh":"-ascii.msh"));
    }
    std::cout<<"FRESH_PERIODIC p=3 primary="<<low<<" total="<<all<<"\n";
    std::cout<<"CAD_PARAMETERS checked="<<pc<<"\n";
    gmsh::clear();f=geometry(false);
    gmsh::merge(prefix+"-ascii.msh");
    require(pairs(f.second,true,true)==all,"MSH4 ASCII periodic payload lost");
    require(parameterChecks()==pc,"MSH4 CAD parameters lost");
    gmsh::clear();f=geometry(false);
    gmsh::merge(prefix+"-binary.msh");
    require(pairs(f.second,true,true)==all,"MSH4 binary periodic payload lost");
    require(parameterChecks()==pc,"MSH4 binary CAD parameters lost");
    std::cout<<"MSH4_ROUNDTRIP ascii binary CAD_PASS\nBASELINE_COMPLETE\n";
    gmsh::finalize();return 0;
  } catch(const std::exception &e) {std::cerr<<"BASELINE_ERROR "<<e.what()<<"\n";return 1;}
    catch(...) {std::cerr<<"BASELINE_ERROR Gmsh exception\n";return 2;}
}
