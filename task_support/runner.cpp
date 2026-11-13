#include "check.hpp"
#include <cstring>
int main() {
    const char* required[] = {"density", "transport", "passes"};
    if (taskCases().size() != 3) return 3;
    for (const char* name : required) {
        const TaskCase* selected = nullptr;
        for (const auto& c : taskCases()) if (!std::strcmp(c.name, name)) {
            if (selected) return 3;
            selected = &c;
        }
        if (!selected) return 3;
        taskChecks() = 0;
        selected->run();
        if (taskChecks() == 0) return 3;
        std::printf("GROUP %s CHECKS %lu DONE\n", name, taskChecks());
    }
    std::puts("TARGET COMPLETE groups=3");
}
