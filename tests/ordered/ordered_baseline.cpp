#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>
#include "fingerprint_matcher.h"
#include "fingerprinter_configuration.h"
int main() {
    std::vector<uint32_t> a;
    uint32_t state = 0x31415926;
    for (uint32_t i = 0; i < 320; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        a.push_back(((i * 37 % 4096) << 20) | (state & 0xfffff));
    }
    std::vector<uint32_t> b(a.begin(), a.begin() + 112);
    b.insert(b.end(), a.begin() + 176, a.end());
    chromaprint::FingerprintMatcher m(chromaprint::CreateFingerprinterConfiguration(CHROMAPRINT_ALGORITHM_TEST2));
    m.set_match_threshold(1.0);
    if (!m.Match(a, b) || m.segments().empty()) return 1;
    size_t covered = 0;
    for (const auto &s : m.segments()) {
        std::cout << "segment=" << s.pos1 << ',' << s.pos2 << ',' << s.duration << '\n';
        covered += s.duration;
    }
    if (covered >= b.size()) return 2;
    std::cout << "baseline: original=" << a.size() << " edited=" << b.size()
              << " covered=" << covered << " (single-offset matcher)\n";
}
