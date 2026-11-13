#include "contract.hpp"
#include <map>
int main()
{
    auto cases = unit_cases();
    std::set<std::string> names;
    std::map<std::string, unsigned> counts, checks;
    for (auto c : cases)
    {
        if (!c.name || !*c.name || !c.group || !c.run || !names.insert(c.name).second) return 4;
        std::string group(c.group);
        if (group != "assembly" && group != "emission" && group != "lifetime") return 7;
        const auto before = uf::checks;
        c.run();
        if (uf::checks <= before) return 5;
        ++counts[group];
        checks[group] += uf::checks - before;
        std::printf("CASE %s %s %u\n", c.group, c.name, uf::checks - before);
    }
    if (cases.size() < 6) return 6;
    for (const char* group : {"assembly", "emission", "lifetime"})
    {
        if (!counts[group] || !checks[group]) return 7;
        std::printf("GROUP_COMPLETE %s cases=%u checks=%u\n", group, counts[group], checks[group]);
    }
    std::printf("TARGET_COMPLETE cases=%zu checks=%u\n", cases.size(), uf::checks);
}
