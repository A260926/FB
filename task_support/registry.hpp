#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace ordered_test {
struct Case {
    std::uint64_t checks=0;
    void check(bool ok, const std::string& message) {
        ++checks;
        if (!ok) throw std::runtime_error(message);
    }
};
struct Entry { std::string group, name; std::function<void(Case&)> run; };
struct Registry {
    std::vector<Entry> entries;
    void add(std::string group, std::string name, std::function<void(Case&)> run) {
        entries.push_back({std::move(group),std::move(name),std::move(run)});
    }
};
}
// Defined in tests/tiny_bvh_ordered_test.cpp. The fixed driver supplies main.
void register_ordered_tests(ordered_test::Registry&);
