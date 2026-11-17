#include <MemPDB/MemPDB.hpp>
#include "image.h"
#include <iostream>
#include <stdexcept>
int main() {
  auto a=MemPDB::PDB::ParseFromMemory(fixture::image(false));
  auto f=a.ResolveFunction("inspect");
  if(a.FunctionCount()!=1 || f.RVA!=0x1020 || f.Size!=16 || f.Arguments.size()!=1 || f.Arguments[0].Name!="count")throw std::runtime_error("production legacy path failed");
  std::cout<<"BASELINE_OK functions="<<a.FunctionCount()<<" RVA="<<f.RVA<<" argument="<<f.Arguments[0].Name<<" type="<<f.Arguments[0].TypeName<<"\n";
  auto b=MemPDB::PDB::ParseFromMemory(fixture::image(true));auto g=b.ResolveFunction("inspect");
  if(g.Arguments.size()!=1)throw std::runtime_error("missing TPI argument");
  std::cout<<"MODERN_ARGUMENT_NAME="<<(g.Arguments[0].Name.empty()?"<empty>":std::string(g.Arguments[0].Name))<<" count="<<g.ArgumentCount<<"\n";
}
