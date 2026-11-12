// SPDX-License-Identifier: Apache-2.0
#ifndef ALAC_LPC_TEST_API_H
#define ALAC_LPC_TEST_API_H
#include <vector>

namespace alac_lpc_test {
enum Result { Passed = 0, Failed = 1, Skipped = 77 };
struct Case {
    const char *name;
    Result (*run)();
};
typedef std::vector<Case> Registry;
// Implement in tests/lpc_analysis.cpp; each callback runs its own assertions.
void register_tests(Registry &registry);

// These are group names, not implementations or expected codec results.
static const char *const required_groups[] = {
    "baseline_gain", "mono16_fit", "stereo16_matrix", "stereo24_shift",
    "frequency_change", "header_residual", "degenerate", "fullscale",
    "short_tail", "entropy_fallback", "state_isolation", "disabled_fast"
};
}
#endif
