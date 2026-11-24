#include "drmemory/regression.h"
#include <cstdio>
#include <string>
int main() {
 drreg::Ledger x;
 if(!x.Begin("normal",1,2)) return 11;
 drreg::Trial ok{"worker",12,drreg::Outcome::Complete,{}};
 if(!x.Observe("normal",1,0,ok)) return 12;
 if(x.Report()!="CASE normal 1 PENDING\n") {std::fputs("API_PROBE expected PENDING\n",stderr);return 13;}
 if(!x.Observe("normal",1,1,ok)) return 14;
 if(x.Report()!="CASE normal 1 CLEAN\n") {std::fputs("API_PROBE expected CLEAN\n",stderr);return 15;}
 std::puts("API_PROBE normal-convergence COMPLETE");
}
