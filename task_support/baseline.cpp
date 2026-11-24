#include "source/traces.h"
#include "source/tokenizer.h"
#include <cassert>
#include <cstdio>
#include <vector>
static void anchor() {}
int main() {
 Traces traces;
 auto pc=reinterpret_cast<size_t>(&anchor);
 traces.TrySaveTrace(pc,2,7); traces.TrySaveTrace(pc,2,7);
 traces.TrySaveTrace(pc,3,7);
 std::vector<std::pair<int,size_t>> rows;
 traces.GetTracesList(&rows);
 assert(rows.size()==2 && traces.GetModulesCount()==1);
 assert(!traces.GetModuleName(0).empty());
 assert((rows[0].second>>60)==2 && ((rows[0].second>>48)&0xfff)==7);
 assert((rows[1].second>>60)==3);
 std::vector<std::pair<std::string,std::string>> opts;
 assert(TokenizeString("coverage=1:coverage_dir=out",&opts));
 assert(opts.size()==2);
 std::puts("BASELINE traces=2 duplicate_suppressed=1 module=resolved tags=preserved config=parsed");
}
