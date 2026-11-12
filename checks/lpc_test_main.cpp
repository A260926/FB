// SPDX-License-Identifier: Apache-2.0
#include "lpc_test_api.h"
#include <cstdio>
#include <exception>
#include <map>
#include <string>

int main() {
    using namespace alac_lpc_test;
    Registry registry;
    try {
        register_tests(registry);
    } catch (...) {
        std::fprintf(stderr, "LPC_REGISTRATION_FAILED exception\n");
        return 2;
    }
    const unsigned expected = sizeof(required_groups) / sizeof(required_groups[0]);
    if (registry.size() != expected) {
        std::fprintf(stderr, "LPC_REGISTRATION_FAILED expected=%u registered=%zu\n",
                     expected, registry.size());
        return 2;
    }
    std::map<std::string, Case> cases;
    for (Registry::const_iterator it = registry.begin(); it != registry.end(); ++it) {
        if (!it->name || !*it->name || !it->run || !cases.insert(std::make_pair(it->name, *it)).second) {
            std::fprintf(stderr, "LPC_REGISTRATION_FAILED null-or-duplicate\n");
            return 2;
        }
    }
    for (unsigned i = 0; i < expected; ++i) {
        if (cases.find(required_groups[i]) == cases.end()) {
            std::fprintf(stderr, "LPC_REGISTRATION_FAILED missing=%s\n", required_groups[i]);
            return 2;
        }
    }
    unsigned executed = 0, passed = 0, failed = 0, skipped = 0;
    for (unsigned i = 0; i < expected; ++i) {
        const char *name = required_groups[i];
        std::printf("LPC_CASE_BEGIN %s\n", name);
        std::fflush(stdout);
        Result result = Failed;
        try {
            result = cases.find(name)->second.run();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "LPC_CASE_EXCEPTION %s %s\n", name, e.what());
        } catch (...) {
            std::fprintf(stderr, "LPC_CASE_EXCEPTION %s unknown\n", name);
        }
        ++executed;
        if (result == Passed) ++passed;
        else if (result == Skipped) ++skipped;
        else ++failed;
        std::printf("LPC_CASE_END %s result=%s\n", name,
                    result == Passed ? "PASS" : (result == Skipped ? "SKIP" : "FAIL"));
    }
    std::printf("LPC_SUMMARY registered=%zu executed=%u passed=%u skipped=%u failed=%u\n",
                registry.size(), executed, passed, skipped, failed);
    if (executed != expected || passed != expected || skipped || failed) return 1;
    std::puts("ALAC_LPC_ANALYSIS_OK cases=12 passed=12 skipped=0 failed=0");
    return 0;
}
