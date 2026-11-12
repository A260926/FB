#include <gtest/gtest.h>
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    const auto* unit = testing::UnitTest::GetInstance();
    bool valid = result == 0 && unit->skipped_test_count() == 0;
    for (const char* name : {"stream_input", "stream_prediction", "stream_restart", "stream_lifecycle"}) {
        int count = 0;
        for (int i=0; i<unit->total_test_suite_count(); ++i) {
            const auto* suite = unit->GetTestSuite(i);
            if (std::string(suite->name()) == name)
                count += suite->successful_test_count();
        }
        if (count == 0) { std::cerr << "MISSING_EXECUTED_SUITE " << name << '\n'; valid = false; }
        else std::cout << "EXECUTED_SUITE " << name << " passed=" << count << '\n';
    }
    if (!valid) return 3;
    std::cout << "STREAM_CONTRACT_EXECUTED tests=" << unit->successful_test_count() << '\n';
    return 0;
}
