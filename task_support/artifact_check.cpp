#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using Vec=std::vector<double>;
int main(int argc,char**argv) {
  try {
    if(argc!=2)throw std::runtime_error("output directory required");
    std::filesystem::path dir=argv[1];std::map<unsigned long long,Vec> original;
    size_t prevFaces=0;bool reduced=false;
    for(int level=0;level<3;++level) {
      std::string stem="level"+std::to_string(level),line;
      std::ifstream obj(dir/(stem+".obj"));if(!obj)throw std::runtime_error("missing OBJ");
      size_t vertices=0,faces=0;
      while(std::getline(obj,line)) {
        std::istringstream s(line);std::string kind;s>>kind;
        if(kind=="v") {double x,y,z;std::string tail;if(!(s>>x>>y>>z)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||(s>>tail))throw std::runtime_error("invalid vertex");++vertices;}
        if(kind=="f") {std::vector<size_t> ids;std::string token;while(s>>token){size_t end;auto n=std::stoull(token,&end);if(end!=token.size()||n==0||n>vertices)throw std::runtime_error("invalid face index");ids.push_back(n);}if(ids.size()!=3||ids[0]==ids[1]||ids[1]==ids[2]||ids[0]==ids[2])throw std::runtime_error("invalid triangle");++faces;}
      }
      if(!vertices||!faces)throw std::runtime_error("empty OBJ");
      if(level && faces>prevFaces)throw std::runtime_error("LOD increased faces");
      if(level && faces<prevFaces)reduced=true;prevFaces=faces;
      std::ifstream csv(dir/(stem+".csv"));if(!csv)throw std::runtime_error("missing CSV");
      std::getline(csv,line);if(line!="id,target_x,target_y,target_z,x,y,z,distance")throw std::runtime_error("CSV header");
      std::map<unsigned long long,Vec> values;
      while(std::getline(csv,line)) {
        for(char& c:line)if(c==',')c=' ';
        std::istringstream s(line);unsigned long long id;Vec v(7);std::string tail;
        if(!(s>>id)||!id)throw std::runtime_error("invalid marker ID");
        for(double& x:v)if(!(s>>x)||!std::isfinite(x))throw std::runtime_error("invalid marker value");
        if(s>>tail)throw std::runtime_error("CSV extra column");
        if(!values.emplace(id,v).second)throw std::runtime_error("duplicate marker ID");
        double d=std::hypot(std::hypot(v[3]-v[0],v[4]-v[1]),v[5]-v[2]);
        if(v[6]<0 || std::abs(d-v[6])>1e-9*(1+d))throw std::runtime_error("marker distance mismatch");
      }
      if(values.empty())throw std::runtime_error("no markers");
      if(level==0)original=values;
      else {if(values.size()!=original.size())throw std::runtime_error("lost markers");for(auto& [id,v]:values){if(!original.count(id))throw std::runtime_error("changed marker ID");for(size_t k=0;k<3;++k)if(v[k]!=original.at(id)[k])throw std::runtime_error("changed target");}}
      std::cout<<"LOD_ARTIFACT level="<<level<<" vertices="<<vertices<<" faces="<<faces<<" markers="<<values.size()<<"\n";
    }
    if(!reduced)throw std::runtime_error("no demonstrated LOD reduction");
    std::cout<<"LOD_ARTIFACTS_COMPLETE\n";return 0;
  }catch(const std::exception& e){std::cerr<<"ARTIFACT_FAIL "<<e.what()<<"\n";return 2;}
}
