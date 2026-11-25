// lock_order_report.cpp
//
// Demonstrates rl::test_params::check_lock_order on a synchronization sample
// that is perfectly legal and completes every run, yet contains a latent
// lock-order inversion: one critical section takes A then B, another takes
// B then A.  With checking disabled the sample just passes; with checking
// enabled Relacy reports the potential ring (long before a scheduler
// interleaving could turn it into a real deadlock).
//
// The program prints the explanatory diagnostic and, after checking the
// observed results, prints LOCK_ORDER_DEMO_OK.

#include "relacy/relacy.hpp"

#include <iostream>
#include <sstream>
#include <string>

namespace
{

struct ab_ba_sample : rl::test_suite<ab_ba_sample, 1>
{
    rl::mutex                   lock_a;
    rl::mutex                   lock_b;

    void thread(unsigned)
    {
        // Order A -> B.
        lock_a.lock($);
        lock_b.lock($);
        lock_b.unlock($);
        lock_a.unlock($);

        // ... later, in another (non overlapping) critical section, the
        // reversed order B -> A.  Nothing blocks in this execution.
        lock_b.lock($);
        lock_a.lock($); // lock-order checker closes the A<->B ring here
        lock_a.unlock($);
        lock_b.unlock($);
    }
};

// Returns just the first lock-order report block from captured output.
std::string first_report(std::string const& captured)
{
    std::string const marker = "LOCK ORDER INVERSION";
    std::size_t first = captured.find(marker);
    if (first == std::string::npos)
        return "";
    std::size_t second = captured.find(marker, first + marker.size());
    if (second == std::string::npos)
        return captured.substr(first);
    return captured.substr(first, second - first);
}

} // namespace

int main()
{
    // 1) Default behavior: checker off, legal sample runs cleanly.
    rl::test_params plain;
    plain.iteration_count = 3;
    std::ostringstream plain_out;
    plain.output_stream = &plain_out;
    bool const plain_ok = rl::simulate<ab_ba_sample>(plain);

    if (!plain_ok || plain.test_result != rl::test_result_success)
    {
        std::cout << "DEMO FAIL: expected unchecked sample to pass\n";
        return 1;
    }

    // 2) Opt in to lock-order checking.
    rl::test_params checked;
    checked.check_lock_order = true;
    checked.iteration_count = 1;
    std::ostringstream report_out;
    checked.output_stream = &report_out;
    bool const checked_sim_ok = rl::simulate<ab_ba_sample>(checked);
    (void)checked_sim_ok; // expected_result defaults to success, so false here

    if (checked.test_result != rl::test_result_lock_order_inversion)
    {
        std::cout << "DEMO FAIL: expected test_result_lock_order_inversion, got "
                  << checked.test_result << "\n";
        return 1;
    }

    if (std::string(rl::test_result_str(
            rl::test_result_lock_order_inversion)) != "LOCK ORDER INVERSION")
    {
        std::cout << "DEMO FAIL: bad test_result_str\n";
        return 1;
    }

    std::string const report = first_report(report_out.str());
    if (report.find("LOCK ORDER INVERSION") == std::string::npos
        || report.find("closed lifecycle node sequence") == std::string::npos
        || report.find("simulated thread") == std::string::npos
        || report.find("source: normal") == std::string::npos)
    {
        std::cout << "DEMO FAIL: diagnostic is missing required details\n";
        std::cout << report_out.str() << "\n";
        return 1;
    }

    std::cout
        << "Example: legal AB/BA sample with rl::test_params::check_lock_order\n"
        << "----------------------------------------------------------------\n"
        << "With check_lock_order=false the sample completes with result: "
        << rl::test_result_str(plain.test_result) << "\n"
        << "With check_lock_order=true  the checker reports:\n\n"
        << report
        << "\nThe simulation stops with result: "
        << rl::test_result_str(checked.test_result) << "\n\n";

    std::cout << "LOCK_ORDER_DEMO_OK\n";
    return 0;
}
