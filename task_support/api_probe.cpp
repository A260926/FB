#define FUZZCOVER_NOFUZZER
#include <fuzzcover/fuzzcover.hpp>
#include <fuzzcover/coverage.hpp>
#include <dlfcn.h>
#include <cassert>
#include <filesystem>
class Probe : public fuzzcover::fuzzcover_interface<unsigned,int> {
public:
 int(*call)(int)=nullptr;
 unsigned value_from_bytes(const uint8_t *p,size_t n) override {return n?p[0]:0;}
 int test_function(const unsigned& n) override {return call(static_cast<int>(n));}
};
int main() {
 void* module=dlopen(".task-build/mod-a/probe.so",RTLD_NOW|RTLD_LOCAL);assert(module);
 void* anchor=dlsym(module,"covered_value");assert(anchor);
 assert(fuzzcover::BindCoverageModule(anchor,"arithmetic",std::string(64,'b'),std::string(64,'c')));
 std::filesystem::create_directories(".task-build/probe-corpus");
 std::ofstream(".task-build/probe-corpus/one",std::ios::binary)<<"A";
 std::filesystem::remove(".task-build/probe-snapshot.json");
 std::filesystem::remove(".task-build/probe-report.json");
 Probe p;p.call=reinterpret_cast<int(*)(int)>(anchor);
 assert(p.record_snapshot(".task-build/probe-corpus",std::string(40,'a'),".task-build/probe-snapshot.json")==0);
 auto snapshot=nlohmann::json::parse(std::ifstream(".task-build/probe-snapshot.json"));
 assert(snapshot.at("format")=="fuzzcover-regression-1");
 assert(snapshot.at("source")==std::string(40,'a'));
 assert(snapshot.at("entries").size()==1 && !snapshot.at("entries")[0].at("coverage").empty());
 assert(snapshot.at("entries")[0].at("output")==131);
 assert(p.verify_snapshot(".task-build/probe-snapshot.json",std::string(40,'a'),".task-build/probe-report.json")==0);
 auto report=nlohmann::json::parse(std::ifstream(".task-build/probe-report.json"));
 assert(report.at("ok")==true && report.at("entries").size()==1);
 // No dlclose: registered modules remain alive for the scope of the process.
}
