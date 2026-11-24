#include "source/receipt.h"
#include "source/traces.h"
#include <cassert>
#include <string>
int main() {
 Traces t;
 assert(t.BeginRound(std::string(40,'a'),1,std::string(64,'b')));
 assert(t.AbortRound());
 assert(CmpCovBeginRound(std::string(40,'a').c_str(),1,std::string(64,'b').c_str()));
 assert(CmpCovAbortRound());
}
