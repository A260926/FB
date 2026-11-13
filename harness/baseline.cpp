#include "simdjson.h"
#include <cassert>
#include <iostream>
using namespace simdjson;
int main(){
  auto json=R"({"rows":[{"id":7,"name":"A\nB","tags":[1,2]},{"id":8,"name":"C","tags":[]}],"total":2})"_padded;
  ondemand::parser p;auto d=p.iterate(json);auto root=d.get_object();int count=0;int64_t sum=0;
  for(auto entry:root["rows"].get_array()){
    auto row=entry.get_object();sum+=int64_t(row["id"]);auto name=std::string_view(row["name"]);
    assert(name==(count?"C":"A\nB"));for(auto tag:row["tags"].get_array())assert(int64_t(tag)>0);++count;
  }
  assert(count==2 && sum==15 && int64_t(root["total"])==2);
  auto scalar="42"_padded; auto d2=p.iterate(scalar);assert(int64_t(d2)==42);
  std::cout<<"SIMDJSON_BASELINE_OK rows=2 sum=15 escaped-string nested-array parser-reuse\n";
}
