#include "task_support/contract.h"
#include <cstdio>
#include <cstring>
namespace emitter_contract {
static unsigned checks=0;
void Check(bool value,const char*expr,const char*file,int line) {
    ++checks;
    if (!value) ADD_FAILURE_AT(file,line) << expr;
}
static bool Required(const char*s) {
    return !std::strcmp(s,"EmissiveMapGeometry") || !std::strcmp(s,"EmissiveMapDensity") || !std::strcmp(s,"EmissiveMapTransport");
}
class Observer:public testing::EmptyTestEventListener {
    void OnTestStart(const testing::TestInfo&) override { checks=0; }
    void OnTestEnd(const testing::TestInfo&info) override {
        if (Required(info.test_case_name())) {
            if (!checks) ADD_FAILURE() << "Target case executed no TASK_CHECK assertions";
            std::printf("TASK_CASE_DONE %s.%s checks=%u\n",info.test_case_name(),info.name(),checks);
        }
    }
};
struct Register { Register(){ testing::UnitTest::GetInstance()->listeners().Append(new Observer); } };
static Register reg;
}
