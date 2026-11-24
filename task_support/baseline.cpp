#define FUZZCOVER_NOFUZZER
#include <fuzzcover/fuzzcover.hpp>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <set>
class Fixture : public fuzzcover::fuzzcover_interface<unsigned,unsigned> {
public:
 unsigned calls=0,conversions=0;
 unsigned value_from_bytes(const uint8_t *data,size_t size) override {
  ++conversions;unsigned value=0;for(size_t i=0;i<size;++i)value+=data[i];return value;
 }
 unsigned test_function(const unsigned& value) override {++calls;return value*3+1;}
};
int main() {
 std::filesystem::create_directories(".task-build/corpus");
 for(const auto& x: {std::pair<const char*,const char*>{"abcdefg-one","A"},{"abcdefg-two","B"},{"duplicate","A"}})
  std::ofstream(std::string(".task-build/corpus/")+x.first,std::ios::binary)<<x.second;
 Fixture replay;char a0[]="fixture",a1[]="--test",a2[]=".task-build/corpus";
 char *test_argv[]={a0,a1,a2};assert(replay.handle_arguments(3,test_argv)==0);assert(replay.calls==3);
 Fixture dump;char b1[]="--dump",b3[]=".task-build/dump.json";char* dump_argv[]={a0,b1,a2,b3};
 assert(dump.handle_arguments(4,dump_argv)==0);
 auto rows=nlohmann::json::parse(std::ifstream(b3));assert(rows.size()==3);
 std::multiset<unsigned> outputs;
 for(const auto&r:rows)outputs.insert(r.at("output").get<unsigned>());
 assert(outputs==std::multiset<unsigned>({196,196,199}));
 std::puts("BASELINE replay=3 records=3 normal_results=ok");
 std::printf("OBSERVATION dump_conversions=%u hash_fields=%s,%s,%s\n",dump.conversions,
 rows[0]["hash"].get<std::string>().c_str(),rows[1]["hash"].get<std::string>().c_str(),rows[2]["hash"].get<std::string>().c_str());
}
