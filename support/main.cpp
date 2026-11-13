#include "check.h"
#include <iostream>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    try {
        for (int i=2; i<argc; ++i) {
            unsigned count=0, before=check::checks();
            for (const auto& c: check::cases()) if (c.group==argv[i]) {
                unsigned n=check::checks();
                c.fn();
                if (check::checks()==n) throw std::runtime_error("case made no checks: "+c.name);
                ++count;
                std::cout << "CASE " << c.group << "/" << c.name << " PASS\n";
            }
            if (!count) throw std::runtime_error(std::string("empty group: ")+argv[i]);
            std::cout << "GROUP " << argv[i] << " cases=" << count << " checks="
                      << check::checks()-before << " DONE\n";
        }
        std::cout << "HARNESS_COMPLETE mode=" << argv[1] << " groups=" << argc-2 << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
