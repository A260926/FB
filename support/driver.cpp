#include "contract.hpp"
#include <cctype>
#include <map>

static bool token(const char *s) {
    if (!s || !*s) return false;
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
    }
    return true;
}
int main() {
    auto cases = unit_cases();
    std::set<std::string> names;
    std::map<std::string, std::pair<unsigned, unsigned>> groups;
    for (auto c : cases) {
        if (!token(c.name) || !token(c.group) || !c.run
            || !names.insert(c.name).second) return 4;
        if (std::string(c.group) != "checked" && std::string(c.group) != "ownership"
            && std::string(c.group) != "reset") return 7;
        auto before = uf::checks;
        c.run();
        if (uf::checks <= before) return 5;
        auto count = uf::checks - before;
        ++groups[c.group].first;
        groups[c.group].second += count;
        std::printf("CASE %s %s %u\n", c.group, c.name, count);
    }
    if (cases.size() < 6) return 6;
    for (const char *group : {"checked", "ownership", "reset"}) {
        auto result = groups[group];
        if (!result.first || !result.second) return 7;
        std::printf("GROUP %s cases=%u checks=%u\n", group, result.first, result.second);
    }
    std::printf("TARGET_COMPLETE cases=%zu checks=%u\n", cases.size(), uf::checks);
}
