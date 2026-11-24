// validation_session example.
//
// A validation_session proves that a native-endian cista offset object graph
// only reads memory the caller authorized.  It is constructed from:
//   * a list of regions {id, data, size} that stay unchanged for the session
//     and do not overlap, and
//   * a per-call expansion budget counting the distinct objects reached as a
//     root or through a non-null offset reference.
//
// validate(root) returns a validation_report with an error code, the failure
// coordinate (region id plus byte offset), and the expanded/reused counters.
// Successful roots and their dependency closures become reusable proofs;
// proofs are committed only when the whole closure validates.
//
// This example shows:
//   1. rejecting a graph because a region was not authorized, then rebuilding
//      the session with the complete authorization and succeeding, and
//   2. failing because the expansion budget is too small, then raising it and
//      retrying the same root.

#include <cstdint>
#include <iostream>
#include <vector>

#include "cista/serialization.h"
#include "cista/validation.h"

namespace data = cista::offset;

struct node {
  int value;
  data::ptr<node> next;
  data::ptr<node> other;
};

struct graph {
  data::indexed_vector<node> nodes;
};

char const* name(cista::validation_error const error) {
  switch (error) {
    case cista::validation_error::none: return "none";
    case cista::validation_error::invalid_regions: return "invalid_regions";
    case cista::validation_error::outside_region: return "outside_region";
    case cista::validation_error::invalid_state: return "invalid_state";
    case cista::validation_error::budget_exhausted:
      return "budget_exhausted";
  }
  return "unknown";
}

int main() {
  // A serialized graph is a realistic read-only input: its offsets resolve
  // inside the byte buffer that holds it.
  graph g;
  g.nodes.resize(3);
  g.nodes[0].value = 1;
  g.nodes[1].value = 2;
  g.nodes[2].value = 3;
  g.nodes[0].next = &g.nodes[1];
  g.nodes[0].other = &g.nodes[1];  // Shared target.
  g.nodes[1].next = &g.nodes[2];
  g.nodes[2].next = &g.nodes[0];   // Cycle back-edge.

  auto bytes = cista::serialize(g);
  auto const* root = reinterpret_cast<graph const*>(bytes.data());

  // 1. Insufficient authorization: only the first half of the buffer is
  //    authorized, so a reference near the end points outside the region.
  {
    cista::validation_session partial(
        {{1, bytes.data(), bytes.size() / 2U}}, 64);
    auto const report = partial.validate(root);
    std::cout << "partial authorization: error=" << name(report.error)
              << " region=" << report.region_id
              << " offset=" << report.offset << '\n';
  }

  // Rebuild the session with the complete buffer and validate successfully.
  cista::validation_session full({{1, bytes.data(), bytes.size()}}, 64);
  auto const first = full.validate(root);
  std::cout << "full authorization: error=" << name(first.error)
            << " expanded=" << first.expanded
            << " reused=" << first.reused
            << " committed=" << full.committed_objects() << '\n';

  // A second call on the same root hits the committed proof.
  auto const again = full.validate(root);
  std::cout << "repeated root: expanded=" << again.expanded
            << " reused=" << again.reused << '\n';

  // 2. A fresh session with a budget smaller than the graph cannot finish.
  //    No partial proof is committed.
  cista::validation_session tight({{1, bytes.data(), bytes.size()}}, 1);
  auto const failed = tight.validate(root);
  std::cout << "small budget: error=" << name(failed.error)
            << " expanded=" << failed.expanded
            << " reused=" << failed.reused
            << " committed=" << tight.committed_objects() << '\n';

  // Raise the budget for the next call and retry the same root.
  tight.set_budget(64);
  auto const retried = tight.validate(root);
  std::cout << "after set_budget: error=" << name(retried.error)
            << " expanded=" << retried.expanded
            << " reused=" << retried.reused
            << " committed=" << tight.committed_objects() << '\n';

  return (first.error == cista::validation_error::none &&
          again.reused == 1U &&
          failed.error == cista::validation_error::budget_exhausted &&
          retried.error == cista::validation_error::none)
             ? 0
             : 1;
}
