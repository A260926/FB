#include "cista/serialization.h"
#include <cstring>
#include <iostream>
struct node { int value; cista::offset::ptr<node> next, other; };
struct graph { cista::offset::indexed_vector<node> nodes; };
int main() {
  graph g;
  g.nodes.resize(3);
  for (int i = 0; i != 3; ++i) g.nodes[i].value = i + 7;
  g.nodes[0].next = &g.nodes[1];
  g.nodes[0].other = &g.nodes[1];
  g.nodes[1].next = &g.nodes[2];
  g.nodes[2].next = &g.nodes[0];
  auto bytes = cista::serialize(g);
  auto before = bytes;
  auto p = cista::deserialize<graph, cista::mode::DEEP_CHECK>(bytes);
  if (p->nodes.size() != 3 || !p->nodes[0].next || !p->nodes[1].next ||
      !p->nodes[2].next || p->nodes[0].next->value != 8 ||
      p->nodes[0].next != p->nodes[0].other ||
      p->nodes[0].next->next->next != &p->nodes[0] ||
      bytes.size() != before.size() ||
      std::memcmp(bytes.data(), before.data(), bytes.size())) return 1;
  std::cout << "BASELINE_CISTA nodes=3 shared=1 cycle=1 unchanged=1\n";
}
