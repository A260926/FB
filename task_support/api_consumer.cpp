// Fixed public-API consumer. This is a normal-use check, not an implementation.
#include "cista/serialization.h"
#include "cista/validation.h"
#include <iostream>
struct normal_node {
  int value;
  cista::offset::ptr<normal_node> next, other;
};
int main() {
  normal_node leaf{9, nullptr, nullptr};
  normal_node root{7, &leaf, &leaf};
  cista::validation_session session({{10, &root, sizeof(root)},
                                     {20, &leaf, sizeof(leaf)}}, 16);
  auto first = session.validate(&root);
  if (first.error != cista::validation_error::none || first.expanded != 2 ||
      first.reused < 1 || session.committed_objects() != 2) return 71;
  auto second = session.validate(&root);
  if (second.error != cista::validation_error::none || second.expanded != 0 ||
      second.reused != 1 || session.committed_objects() != 2) return 72;
  std::cout << "VALIDATION_API_BOUND regions=2 committed=2\n";
}
