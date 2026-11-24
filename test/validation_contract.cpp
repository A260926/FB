// Contract tests for cista::validation_session.
//
// Every object graph here is built through the legal cista API or obtained
// from cista::serialize.  Rejections are observed only by withholding an
// authorized region or by lowering the expansion budget; no bytes are
// corrupted and no crash inputs are constructed.  Serialized inputs are
// compared byte-for-byte before and after validation to prove the walk is
// read-only.

#include "task_support/check.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "cista/serialization.h"
#include "cista/validation.h"

namespace data = cista::offset;

namespace {

struct node {
  int value;
  data::ptr<node> next;
  data::ptr<node> other;
};

struct graph {
  data::indexed_vector<node> nodes;
};

struct inner {
  int x;
  data::ptr<inner> p;
};

struct outer {
  inner in;
  int z;
};

struct vec_holder {
  data::vector<int> nums;
};

struct composite {
  data::string name;
  data::cstring label;
  data::vector<int> nums;
  data::unique_ptr<node> link;
  cista::optional<int> opt;
  cista::variant<int, node> choice;
  data::ptr<void> any;
};

bool bytes_unchanged(cista::byte_buf const& before,
                     cista::byte_buf const& after) {
  return before.size() == after.size() &&
         std::memcmp(before.data(), after.data(), before.size()) == 0;
}

// --- regions ---------------------------------------------------------------

int region_descriptors_rejected() {
  char a[32];
  char b[32];
  int probe = 0;

  cista::validation_session empty_desc({{1, a, 0}, {2, b, sizeof b}}, 8);
  TASK_CHECK(empty_desc.validate(&probe).error ==
             cista::validation_error::invalid_regions);

  cista::validation_session null_desc({{1, nullptr, sizeof a}}, 8);
  TASK_CHECK(null_desc.validate(&probe).error ==
             cista::validation_error::invalid_regions);

  cista::validation_session duplicate_id(
      {{1, a, sizeof a}, {1, b, sizeof b}}, 8);
  TASK_CHECK(duplicate_id.validate(&probe).error ==
             cista::validation_error::invalid_regions);

  cista::validation_session overlapping(
      {{1, a, sizeof a}, {2, a + 4, sizeof a - 4U}}, 8);
  TASK_CHECK(overlapping.validate(&probe).error ==
             cista::validation_error::invalid_regions);

  cista::validation_session overflowing(
      {{1, reinterpret_cast<void const*>(~static_cast<std::uintptr_t>(0)),
        16}},
      8);
  TASK_CHECK(overflowing.validate(&probe).error ==
             cista::validation_error::invalid_regions);

  // Adjacent, non-overlapping descriptors are accepted.
  cista::validation_session adjacent(
      {{1, a, 16}, {2, a + 16, sizeof a - 16U}}, 8);
  TASK_CHECK(adjacent.validate(a).error == cista::validation_error::none);
  return 0;
}

int unauthorized_root_uses_origin_coordinate() {
  int authorized = 0;
  int foreign = 0;
  cista::validation_session session({{1, &authorized, sizeof authorized}}, 4);
  auto const report = session.validate(&foreign);
  TASK_CHECK(report.error == cista::validation_error::outside_region);
  TASK_CHECK(report.region_id == 0U);
  TASK_CHECK(report.offset == 0U);
  TASK_CHECK(report.expanded == 0U);
  TASK_CHECK(report.reused == 0U);
  TASK_CHECK(session.committed_objects() == 0U);
  return 0;
}

int omitted_region_points_at_reference_slot() {
  node leaf{9, nullptr, nullptr};
  node root{7, &leaf, &leaf};

  {
    cista::validation_session partial({{10, &root, sizeof root}}, 16);
    auto const report = partial.validate(&root);
    TASK_CHECK(report.error == cista::validation_error::outside_region);
    TASK_CHECK(report.region_id == 10U);
    TASK_CHECK(report.offset == offsetof(node, next));
    TASK_CHECK(report.expanded == 0U);
    TASK_CHECK(report.reused == 0U);
    TASK_CHECK(partial.committed_objects() == 0U);
  }

  // Restoring the missing authorization makes the same root succeed.
  cista::validation_session complete(
      {{10, &root, sizeof root}, {20, &leaf, sizeof leaf}}, 16);
  auto const report = complete.validate(&root);
  TASK_CHECK(report.error == cista::validation_error::none);
  TASK_CHECK(report.expanded == 2U);
  TASK_CHECK(complete.committed_objects() == 2U);
  return 0;
}

int gap_between_regions_is_not_readable() {
  vec_holder holder;
  holder.nums = {10, 20, 30, 40, 50, 60};
  auto bytes = cista::serialize(holder);
  auto const before = bytes;
  auto const* root = reinterpret_cast<vec_holder const*>(bytes.data());

  {
    cista::validation_session full({{1, bytes.data(), bytes.size()}}, 16);
    TASK_CHECK(full.validate(root).error == cista::validation_error::none);
  }

  // Two non-empty regions separated by a one-byte gap inside the tail
  // element block: the contiguous storage cannot be authorized.
  std::vector<cista::validation_region> split;
  split.push_back({1, bytes.data(), bytes.size() - 2U});
  split.push_back(
      {2, bytes.data() + bytes.size() - 1U, 1U});
  cista::validation_session gapped(split, 16);
  auto const report = gapped.validate(root);
  TASK_CHECK(report.error == cista::validation_error::outside_region);
  TASK_CHECK(report.region_id == 1U);
  TASK_CHECK(bytes_unchanged(before, bytes));
  return 0;
}

int live_owning_container_is_invalid_state() {
  vec_holder holder;
  holder.nums = {1, 2, 3, 4, 5};  // Live storage is self-allocated.
  TASK_CHECK(holder.nums.self_allocated_);

  cista::validation_session live(
      {{1, &holder, sizeof holder},
       {2, holder.nums.data(), holder.nums.size() * sizeof(int)}},
      16);
  auto const report = live.validate(&holder);
  TASK_CHECK(report.error == cista::validation_error::invalid_state);
  TASK_CHECK(report.region_id == 1U);
  TASK_CHECK(report.expanded == 0U);
  TASK_CHECK(report.reused == 0U);
  TASK_CHECK(live.committed_objects() == 0U);

  // The serialized form satisfies the container state rules.
  auto bytes = cista::serialize(holder);
  auto const before = bytes;
  auto const* root = reinterpret_cast<vec_holder const*>(bytes.data());
  cista::validation_session serialized({{3, bytes.data(), bytes.size()}}, 16);
  TASK_CHECK(serialized.validate(root).error ==
             cista::validation_error::none);
  TASK_CHECK(bytes_unchanged(before, bytes));
  return 0;
}

// --- sharing ---------------------------------------------------------------

int cross_region_sharing_and_cycle() {
  node n0{0, nullptr, nullptr};
  node n1{1, nullptr, nullptr};
  node n2{2, nullptr, nullptr};
  n0.next = &n1;
  n0.other = &n1;  // Shared target reached through two references.
  n1.next = &n2;
  n2.next = &n0;   // Back-edge closing the cycle across regions.

  cista::validation_session session(
      {{10, &n0, sizeof n0}, {20, &n1, sizeof n1}, {30, &n2, sizeof n2}},
      16);
  auto const report = session.validate(&n0);
  TASK_CHECK(report.error == cista::validation_error::none);
  TASK_CHECK(report.expanded == 3U);
  TASK_CHECK(report.reused == 2U);  // Shared n1 plus the n0 back-edge.
  TASK_CHECK(session.committed_objects() == 3U);
  return 0;
}

int serialized_graph_sharing_and_cycle() {
  graph g;
  g.nodes.resize(3);
  for (int i = 0; i != 3; ++i) {
    g.nodes[i].value = i + 7;
  }
  g.nodes[0].next = &g.nodes[1];
  g.nodes[0].other = &g.nodes[1];
  g.nodes[1].next = &g.nodes[2];
  g.nodes[2].next = &g.nodes[0];

  auto bytes = cista::serialize(g);
  auto const before = bytes;
  auto const* root = reinterpret_cast<graph const*>(bytes.data());

  cista::validation_session session(
      {{99, bytes.data(), bytes.size()}}, 64);
  auto const report = session.validate(root);
  TASK_CHECK(report.error == cista::validation_error::none);
  TASK_CHECK(report.expanded == 3U);
  TASK_CHECK(report.reused == 2U);
  TASK_CHECK(session.committed_objects() == 3U);
  TASK_CHECK(bytes_unchanged(before, bytes));

  // Withholding the tail of the buffer rejects the graph.
  cista::validation_session shrunk(
      {{99, bytes.data(), bytes.size() / 2U}}, 64);
  auto const cut = shrunk.validate(root);
  TASK_CHECK(cut.error == cista::validation_error::outside_region);
  TASK_CHECK(cut.region_id == 99U);
  TASK_CHECK(cut.expanded == 0U);
  TASK_CHECK(cut.reused == 0U);
  TASK_CHECK(bytes_unchanged(before, bytes));
  return 0;
}

int proofs_are_distinguished_by_static_type() {
  inner target{5, nullptr};
  outer o{inner{3, &target}, 8};

  cista::validation_session session(
      {{1, &o, sizeof o}, {2, &target, sizeof target}}, 16);

  auto const outer_report = session.validate(&o);
  TASK_CHECK(outer_report.error == cista::validation_error::none);
  TASK_CHECK(outer_report.expanded == 2U);

  // The same start address with a different static type is a distinct object.
  auto const* same_start = reinterpret_cast<inner const*>(&o);
  auto const inner_report = session.validate(same_start);
  TASK_CHECK(inner_report.error == cista::validation_error::none);
  TASK_CHECK(inner_report.expanded == 1U);
  TASK_CHECK(session.committed_objects() == 3U);

  TASK_CHECK(session.validate(&o).reused == 1U);
  TASK_CHECK(session.validate(&o).expanded == 0U);
  TASK_CHECK(session.validate(same_start).reused == 1U);
  TASK_CHECK(session.validate(same_start).expanded == 0U);
  return 0;
}

int serialized_containers_and_helpers() {
  composite c;
  c.name = data::string{"a string long enough to leave inline storage"};
  c.label = data::cstring{"cstring label"};
  c.nums = {1, 2, 3, 4};
  node shared{42, nullptr, nullptr};
  c.link = data::unique_ptr<node>{new node{shared}, true};
  c.opt = 11;
  c.choice = node{7, nullptr, nullptr};
  node any_target{3, nullptr, nullptr};
  c.any = &any_target;

  auto bytes = cista::serialize(c);
  auto const before = bytes;
  auto const* root = reinterpret_cast<composite const*>(bytes.data());

  cista::validation_session session(
      {{7, bytes.data(), bytes.size()}}, 32);
  auto const report = session.validate(root);
  TASK_CHECK(report.error == cista::validation_error::none);
  TASK_CHECK(report.expanded >= 2U);
  TASK_CHECK(bytes_unchanged(before, bytes));

  // The exact budget succeeds; one slot less is rejected.
  cista::validation_session exact(
      {{7, bytes.data(), bytes.size()}}, report.expanded);
  TASK_CHECK(exact.validate(root).error == cista::validation_error::none);
  cista::validation_session tight(
      {{7, bytes.data(), bytes.size()}}, report.expanded - 1U);
  auto const failed = tight.validate(root);
  TASK_CHECK(failed.error == cista::validation_error::budget_exhausted);
  TASK_CHECK(failed.expanded == 0U);
  TASK_CHECK(failed.reused == 0U);
  TASK_CHECK(tight.committed_objects() == 0U);
  TASK_CHECK(bytes_unchanged(before, bytes));
  return 0;
}

int shared_reference_is_reused_not_reexpanded() {
  node leaf{9, nullptr, nullptr};
  node root{7, &leaf, &leaf};

  cista::validation_session session(
      {{10, &root, sizeof root}, {20, &leaf, sizeof leaf}}, 16);
  auto const first = session.validate(&root);
  TASK_CHECK(first.error == cista::validation_error::none);
  TASK_CHECK(first.expanded == 2U);
  TASK_CHECK(first.reused == 1U);
  TASK_CHECK(session.committed_objects() == 2U);

  auto const again = session.validate(&root);
  TASK_CHECK(again.error == cista::validation_error::none);
  TASK_CHECK(again.expanded == 0U);
  TASK_CHECK(again.reused == 1U);
  TASK_CHECK(session.committed_objects() == 2U);
  return 0;
}

// --- transactions ----------------------------------------------------------

int successful_closure_is_committed_and_reusable() {
  node leaf{9, nullptr, nullptr};
  node root{7, &leaf, &leaf};

  cista::validation_session session(
      {{10, &root, sizeof root}, {20, &leaf, sizeof leaf}}, 16);
  TASK_CHECK(session.validate(&root).error ==
             cista::validation_error::none);
  TASK_CHECK(session.committed_objects() == 2U);

  // A committed sub-object is a whole-root hit on its own.
  auto const leaf_report = session.validate(&leaf);
  TASK_CHECK(leaf_report.error == cista::validation_error::none);
  TASK_CHECK(leaf_report.expanded == 0U);
  TASK_CHECK(leaf_report.reused == 1U);
  return 0;
}

int failed_validation_keeps_existing_proofs() {
  node good_leaf{1, nullptr, nullptr};
  node good_root{0, &good_leaf, nullptr};
  node foreign{9, nullptr, nullptr};

  cista::validation_session session(
      {{1, &good_root, sizeof good_root},
       {2, &good_leaf, sizeof good_leaf}},
      8);
  TASK_CHECK(session.validate(&good_root).error ==
             cista::validation_error::none);
  TASK_CHECK(session.committed_objects() == 2U);

  auto const failed = session.validate(&foreign);
  TASK_CHECK(failed.error == cista::validation_error::outside_region);
  TASK_CHECK(failed.region_id == 0U);
  TASK_CHECK(failed.offset == 0U);
  TASK_CHECK(failed.expanded == 0U);
  TASK_CHECK(failed.reused == 0U);
  TASK_CHECK(session.committed_objects() == 2U);

  auto const reused = session.validate(&good_root);
  TASK_CHECK(reused.error == cista::validation_error::none);
  TASK_CHECK(reused.expanded == 0U);
  TASK_CHECK(reused.reused == 1U);
  return 0;
}

int budget_failure_retries_after_raising_budget() {
  node leaf{9, nullptr, nullptr};
  node root{7, &leaf, &leaf};

  cista::validation_session session(
      {{10, &root, sizeof root}, {20, &leaf, sizeof leaf}}, 1);
  auto const failed = session.validate(&root);
  TASK_CHECK(failed.error == cista::validation_error::budget_exhausted);
  TASK_CHECK(failed.expanded == 0U);
  TASK_CHECK(failed.reused == 0U);
  TASK_CHECK(session.committed_objects() == 0U);

  session.set_budget(2);
  auto const passed = session.validate(&root);
  TASK_CHECK(passed.error == cista::validation_error::none);
  TASK_CHECK(passed.expanded == 2U);
  TASK_CHECK(session.committed_objects() == 2U);
  return 0;
}

int mid_graph_budget_failure_then_full_retry() {
  node n2{2, nullptr, nullptr};
  node n1{1, &n2, nullptr};
  node n0{0, &n1, nullptr};

  cista::validation_session session(
      {{1, &n0, sizeof n0},
       {2, &n1, sizeof n1},
       {3, &n2, sizeof n2}},
      2);
  auto const failed = session.validate(&n0);
  TASK_CHECK(failed.error == cista::validation_error::budget_exhausted);
  TASK_CHECK(failed.expanded == 0U);
  TASK_CHECK(failed.reused == 0U);
  TASK_CHECK(session.committed_objects() == 0U);

  session.set_budget(3);
  auto const passed = session.validate(&n0);
  TASK_CHECK(passed.error == cista::validation_error::none);
  TASK_CHECK(passed.expanded == 3U);
  TASK_CHECK(session.committed_objects() == 3U);

  auto const again = session.validate(&n0);
  TASK_CHECK(again.expanded == 0U);
  TASK_CHECK(again.reused == 1U);
  return 0;
}

int clear_drops_committed_proofs() {
  node only{1, nullptr, nullptr};
  cista::validation_session session({{1, &only, sizeof only}}, 4);

  TASK_CHECK(session.validate(&only).error ==
             cista::validation_error::none);
  TASK_CHECK(session.committed_objects() == 1U);

  session.clear();
  TASK_CHECK(session.committed_objects() == 0U);

  auto const rebuilt = session.validate(&only);
  TASK_CHECK(rebuilt.error == cista::validation_error::none);
  TASK_CHECK(rebuilt.expanded == 1U);
  TASK_CHECK(rebuilt.reused == 0U);
  TASK_CHECK(session.committed_objects() == 1U);
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"regions", "descriptor_rejects", region_descriptors_rejected},
    {"regions", "unauthorized_root_origin",
     unauthorized_root_uses_origin_coordinate},
    {"regions", "omitted_region_slot", omitted_region_points_at_reference_slot},
    {"regions", "region_gap_unreadable", gap_between_regions_is_not_readable},
    {"regions", "live_container_invalid_state",
     live_owning_container_is_invalid_state},

    {"sharing", "cross_region_share_cycle",
     cross_region_sharing_and_cycle},
    {"sharing", "serialized_graph_share_cycle",
     serialized_graph_sharing_and_cycle},
    {"sharing", "static_type_distinction",
     proofs_are_distinguished_by_static_type},
    {"sharing", "serialized_containers", serialized_containers_and_helpers},
    {"sharing", "shared_reference_reuse", shared_reference_is_reused_not_reexpanded},

    {"transactions", "commit_and_reuse",
     successful_closure_is_committed_and_reusable},
    {"transactions", "failure_keeps_proofs",
     failed_validation_keeps_existing_proofs},
    {"transactions", "budget_retry", budget_failure_retries_after_raising_budget},
    {"transactions", "mid_graph_budget_retry",
     mid_graph_budget_failure_then_full_retry},
    {"transactions", "clear_proofs", clear_drops_committed_proofs},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
