#include <cstdio>
#include <set>
#include <utility>
#include <vector>

int main() {
  // Known Csaszar face list: (i, i+1, i+3) and (i, i+2, i+3) mod 7.
  std::vector<std::vector<int>> tris;
  for (int i = 0; i < 7; ++i) {
    tris.push_back({i, (i + 1) % 7, (i + 3) % 7});
    tris.push_back({i, (i + 2) % 7, (i + 3) % 7});
  }
  // Count undirected edges.
  std::multiset<std::pair<int, int>> cnt;
  for (auto& t : tris)
    for (int k = 0; k < 3; ++k) cnt.insert({std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])});
  int ne = 0;
  for (auto it = cnt.begin(); it != cnt.end();) {
    auto range = cnt.equal_range(*it);
    int n = 0;
    for (auto j = range.first; j != range.second; ++j) ++n;
    if (n != 2) printf("edge (%d,%d) multiplicity %d\n", it->first, it->first, n);
    ++ne;
    it = range.second;
  }
  printf("V=7 E=%d F=%d chi=%d\n", ne, int(tris.size()), 7 - ne + int(tris.size()));
  // Search orientation flips.
  std::vector<int> flip(14, 0);
  auto ok = [&]() {
    std::set<std::pair<int, int>> dir;
    for (int f = 0; f < 14; ++f) {
      auto t = tris[f];
      if (flip[f]) std::swap(t[1], t[2]);
      for (int k = 0; k < 3; ++k) {
        int u = t[k], v = t[(k + 1) % 3];
        if (dir.count({v, u})) return false;
        if (!dir.insert({u, v}).second) return false;
      }
    }
    return true;
  };
  for (int mask = 0; mask < (1 << 14); ++mask) {
    for (int f = 0; f < 14; ++f) flip[f] = (mask >> f) & 1;
    if (ok()) {
      printf("oriented:\n");
      for (int f = 0; f < 14; ++f) {
        auto t = tris[f];
        if (flip[f]) std::swap(t[1], t[2]);
        printf("  %d %d %d\n", t[0], t[1], t[2]);
      }
      return 0;
    }
  }
  printf("not orientable?\n");
  return 1;
}
