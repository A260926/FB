// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
// On-demand SrMesh paging regressions.  No main(): task_support/driver.c drives task_cases.
#include "task_support/check.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include "libHh/GMesh.h"
#include "libHh/PMesh.h"
#include "libHh/SrMesh.h"
#include "libHh/SrMeshPages.h"

using namespace hh;

namespace {

constexpr const char* k_srm_path = "demos/data/gcanyon-prefix.srm";
constexpr const char* k_pm_path = "demos/data/imagesquare.pm";

// --- Small mesh/value helpers (standard containers only for the checking logic). -------------------------------

struct Vp {
  SrViewParams vp;
};

SrViewParams make_view(const Point& eye, const Vector& vdir, float thresh) {
  SrViewParams vp;
  Vector left(vdir[1], -vdir[0], 0.f);
  if (mag2(left) == 0.f) left = Vector(0.f, 1.f, 0.f);
  left.normalize();
  const Vector top = cross(vdir, left);
  vp.set_frame(Frame(vdir, left, top, eye));
  vp.set_zooms(Vec2<float>(1.f, 1.f));
  vp.set_screen_thresh(thresh);
  return vp;
}

std::unique_ptr<SrMesh> load_srm() {
  auto sr = std::make_unique<SrMesh>();
  std::ifstream f(k_srm_path, std::ios::binary);
  if (!f) return nullptr;  // read_srm() below asserts on a corrupt stream.
  sr->read_srm(f);
  sr->ok();
  return sr;
}

// A backing store of encoded page bytes, simulating slow storage without resident SrVsplit payloads.
using PageStore = std::map<int, std::vector<uint8_t>>;

PageStore build_store(const SrMesh& sr, const SrPageDirectory& dir) {
  PageStore store;
  for_int(pageid, dir.num_pages()) store.emplace(pageid, SrPageCodec::encode_page(sr, dir, pageid));
  return store;
}

// A split is executed while its fl face is active.
bool split_executed(const SrMesh& sr, int vspli) {
  return sr.sr_face_active(sr.sr_face_of_vsplit(vspli));
}

// Independent directed-edge manifold audit over an extracted mesh.
struct EdgeAudit {
  bool no_duplicate_directed{true};
  bool manifold{true};
  bool has_boundary{false};
  int num_directed{0};
};

EdgeAudit audit_edges(const GMesh& g) {
  EdgeAudit a;
  std::set<std::pair<int, int>> directed;
  std::map<std::pair<int, int>, int> undirected;  // Sorted endpoint pair -> face count.
  for (Face f : g.ordered_faces()) {
    Array<Vertex> va;
    g.get_vertices(f, va);
    if (va.num() != 3) a.no_duplicate_directed = false;
    int id[3];
    for_int(j, 3) id[j] = g.vertex_id(va[j]);
    for_int(j, 3) {
      const int u = id[j], v = id[mod3(j + 1)];
      if (!directed.insert({u, v}).second) a.no_duplicate_directed = false;  // No oriented edge twice.
      ++undirected[std::minmax(u, v)];
    }
  }
  for (const auto& [key, count] : undirected) {
    if (count > 2) a.manifold = false;  // At most two triangles share an edge.
    if (count == 1) a.has_boundary = true;
  }
  a.num_directed = int(directed.size());
  return a;
}

std::set<int> executed_pages(const SrMesh& sr, const SrPageDirectory& dir) {
  std::set<int> pages;
  for_int(vspli, dir.num_vsplits)
    if (split_executed(sr, vspli)) pages.insert(dir.page_of(vspli));
  return pages;
}

// Drive adaptation against a store, optionally supplying pages in a chosen order.  Returns the number of rounds.
int drive(SrPagedMesh& pm, const SrViewParams& vp, const PageStore& store, int max_rounds, bool supply,
          std::vector<int>* last_pending = nullptr, const std::vector<int>* only_pages = nullptr) {
  int round = 0;
  for (; round < max_rounds; ++round) {
    pm.adapt_refinement(vp);
    pm.mesh().ok();
    // Every intermediate frontier must also pass the independent directed-edge table, not just SrMesh::ok().
    const EdgeAudit audit = audit_edges(pm.mesh().extract_gmesh());
    TASK_CHECK(audit.manifold);
    TASK_CHECK(audit.no_duplicate_directed);
    TASK_CHECK(audit.num_directed == 3 * pm.mesh().num_active_faces());
    std::vector<int> pending = pm.pending_pages();
    // pending_pages() must always be sorted and deduplicated.
    TASK_CHECK(std::is_sorted(pending.begin(), pending.end()));
    TASK_CHECK(std::adjacent_find(pending.begin(), pending.end()) == pending.end());
    if (last_pending) *last_pending = pending;
    if (pending.empty() || !supply) break;
    for (int pid : pending) {
      if (only_pages && std::find(only_pages->begin(), only_pages->end(), pid) == only_pages->end()) continue;
      SrPageError err = SrPageError::none;
      const bool okk = pm.supply_page(pid, store.at(pid), err);
      TASK_CHECK(okk);
      TASK_CHECK(err == SrPageError::none);
    }
  }
  return round;
}

// Canonical face signature: global vertex indices plus the material string.
struct FaceSig {
  std::array<int, 3> vi{};
  std::string mat;
  bool operator<(const FaceSig& o) const {
    if (vi != o.vi) return vi < o.vi;
    return mat < o.mat;
  }
  bool operator==(const FaceSig& o) const { return vi == o.vi && mat == o.mat; }
};

struct MeshSig {
  std::map<int, std::array<float, 6>> verts;  // Global vertex id -> point + normal.
  std::set<FaceSig> faces;
};

MeshSig signature(const GMesh& g) {
  MeshSig s;
  for (Vertex v : g.ordered_vertices()) {
    const int id = g.vertex_id(v);
    const Point& p = g.point(v);
    std::array<float, 6> a{};
    for_int(c, 3) a[c] = p[c];
    if (const char* si = g.get_string(v)) {
      Array<float> ar(3);
      if (parse_key_vec(si, "normal", ar)) for_int(c, 3) a[3 + c] = ar[c];
    }
    s.verts.emplace(id, a);
  }
  for (Face f : g.ordered_faces()) {
    Array<Vertex> va;
    g.get_vertices(f, va);
    FaceSig sig;
    for_int(j, 3) sig.vi[j] = g.vertex_id(va[j]);
    sig.mat = g.get_string(f) ? g.get_string(f) : "";
    s.faces.insert(sig);
  }
  return s;
}

// Reference all-resident adaptation; returns the converged mesh and its demanded page set for one view.
std::unique_ptr<SrMesh> reference_frontier(const SrViewParams& vp, int rounds = 20) {
  auto ref = load_srm();
  ref->set_view_params(vp);
  for_int(i, rounds) ref->adapt_refinement();
  ref->ok();
  return ref;
}

SrViewParams near_view() {
  auto sr = load_srm();
  const Bbox b = sr->get_bbox();
  const Point c = interp(b[0], b[1], .5f);
  return make_view(Point(c[0], c[1], b[1][2] + b.max_side() * 2.f), Vector(0.f, 0.f, -1.f), 1e-4f);
}

SrViewParams far_view() {
  auto sr = load_srm();
  const Bbox b = sr->get_bbox();
  const Point c = interp(b[0], b[1], .5f);
  return make_view(Point(c[0], c[1], b[1][2] + b.max_side() * 50.f), Vector(0.f, 0.f, -1.f), 0.5f);
}

// A second, more distant camera that demands a strict subset of the near camera's pages.
SrViewParams mid_view() {
  auto sr = load_srm();
  const Bbox b = sr->get_bbox();
  const Point c = interp(b[0], b[1], .5f);
  return make_view(Point(c[0], c[1], b[1][2] + b.max_side() * 4.f), Vector(0.f, 0.f, -1.f), 1e-4f);
}

// ============================================================================
// dependency group: cross-page prerequisite split chains.
// ============================================================================

int dep_out_of_order() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  // Discover the pages the all-resident reference needs under this view.
  auto ref = reference_frontier(vp);
  const std::set<int> need = executed_pages(*ref, dir);
  TASK_CHECK(need.size() >= 2);  // The chain genuinely spans several pages.

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));  // Large budget; delivery order is what we control.
  // First adaptation with no supply: only base mesh, requests registered for multiple pages.
  pm.adapt_refinement(vp);
  pm.mesh().ok();
  std::vector<int> pending0 = pm.pending_pages();
  TASK_CHECK(pending0.size() >= 2);
  TASK_CHECK(pm.mesh().num_active_faces() == dir.base_nfaces);

  // Supply the demanded pages in descending (reverse dependency) order, one page per round.
  std::vector<int> order(need.begin(), need.end());
  std::sort(order.begin(), order.end(), std::greater<int>());
  for (int pid : order) {
    SrPageError err = SrPageError::none;
    TASK_CHECK(pm.supply_page(pid, store.at(pid), err));
    pm.adapt_refinement(vp);
    pm.mesh().ok();  // Every intermediate frontier is legal despite out-of-order arrival.
  }
  for_int(i, 8) pm.adapt_refinement(vp);
  pm.mesh().ok();
  TASK_CHECK(pm.pending_pages().empty());
  // Connectivity must match the all-resident reference under the same view.
  const MeshSig a = signature(pm.mesh().extract_gmesh());
  const MeshSig b = signature(ref->extract_gmesh());
  TASK_CHECK(a.faces == b.faces);
  TASK_CHECK(a.verts == b.verts);
  return 0;
}

int dep_missing_reentry() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));
  // Repeated adaptation while nothing is supplied: the request set must not grow and topology must stay at base.
  std::vector<int> first;
  drive(pm, vp, store, 3, false, &first);
  TASK_CHECK(!first.empty());
  pm.adapt_refinement(vp);
  TASK_CHECK(pm.pending_pages() == first);  // Repeated requests do not inflate.
  TASK_CHECK(pm.mesh().num_active_faces() == dir.base_nfaces);
  TASK_CHECK(pm.total_resident_split_payloads() == 0);  // No startup prefetch read any page.

  // Now satisfy one pending page and re-enter: refinement proceeds from the current frontier, never replaying base.
  const int pid = first.front();
  SrPageError err = SrPageError::none;
  TASK_CHECK(pm.supply_page(pid, store.at(pid), err));
  pm.adapt_refinement(vp);
  pm.mesh().ok();
  TASK_CHECK(pm.total_resident_split_payloads() == dir.page_num_splits(pid));
  TASK_CHECK(pm.mesh().num_active_faces() >= dir.base_nfaces);
  return 0;
}

int dep_shared_pin() {
  auto sr = load_srm();
  constexpr int k_spp = 2;  // Small pages so several executed splits share one page.
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));
  drive(pm, vp, store, 40, true);
  pm.mesh().ok();
  // Find a page holding at least two executed splits: its pin count must equal the number of live splits on it.
  int shared = -1, live = 0;
  for_int(pageid, dir.num_pages()) {
    int cnt = 0;
    const int pfirst = dir.page_first_split(pageid);
    for_int(k, dir.page_num_splits(pageid))
      if (split_executed(pm.mesh(), pfirst + k)) ++cnt;
    if (cnt >= 2) {
      shared = pageid;
      live = cnt;
      break;
    }
  }
  TASK_CHECK(shared >= 0);
  TASK_CHECK(pm.pin_count(shared) == live);
  TASK_CHECK(pm.is_resident(shared));
  // A pinned page can never be selected for eviction, even when the cache is asked to make room.
  const size_t resident_before = pm.resident_page_count();
  SrPageError err = SrPageError::none;
  TASK_CHECK(!pm.supply_page(99999, store.begin()->second, err));  // Bad id rejected, nothing changes.
  TASK_CHECK(pm.resident_page_count() == int(resident_before));
  TASK_CHECK(pm.is_resident(shared));
  TASK_CHECK(pm.pin_count(shared) == live);
  return 0;
}

// ============================================================================
// residency group: pinning, LRU eviction, budget.
// ============================================================================

int res_coarsen_eviction() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams near = near_view();

  // Budget holds only a handful of pages.
  const int budget = 4;
  SrPagedMesh pm;
  pm.initialize(dir, budget);
  drive(pm, near, store, 40, true);
  pm.mesh().ok();
  TASK_CHECK(pm.resident_page_count() <= budget);
  TASK_CHECK(pm.total_resident_split_payloads() <= budget * k_spp);

  // Move the camera far away so the view coarsens the mesh and releases pins.
  const SrViewParams far = far_view();
  for_int(i, 12) {
    pm.adapt_refinement(far);
    pm.mesh().ok();
  }
  int unpinned = 0;
  for_int(pageid, dir.num_pages())
    if (pm.is_resident(pageid) && pm.pin_count(pageid) == 0) ++unpinned;
  TASK_CHECK(unpinned >= 1);  // Legal coarsening unpinned pages that can now be evicted.

  // A not-yet-loaded page demanded by a fresh near view forces an LRU eviction of an unpinned page.
  pm.adapt_refinement(near);
  std::vector<int> pending = pm.pending_pages();
  int fresh = -1;
  for (int pid : pending)
    if (!pm.is_resident(pid)) {
      fresh = pid;
      break;
    }
  if (fresh >= 0 && pm.resident_page_count() >= budget) {
    const std::set<int> before_resident = [&] {
      std::set<int> s;
      for_int(p, dir.num_pages()) if (pm.is_resident(p)) s.insert(p);
      return s;
    }();
    SrPageError err = SrPageError::none;
    TASK_CHECK(pm.supply_page(fresh, store.at(fresh), err));
    TASK_CHECK(pm.is_resident(fresh));
    TASK_CHECK(pm.resident_page_count() <= budget);
    TASK_CHECK(pm.total_resident_split_payloads() <= budget * k_spp);
    // Exactly one previously resident, unpinned page was replaced.
    int evicted = 0;
    for (int pid : before_resident)
      if (!pm.is_resident(pid)) {
        TASK_CHECK(pm.pin_count(pid) == 0);  // An active-split page was never evicted.
        ++evicted;
      }
    TASK_CHECK(evicted == 1);
  }
  pm.mesh().ok();
  return 0;
}

int res_budget_block_recover() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  const int budget = 2;
  SrPagedMesh pm;
  pm.initialize(dir, budget);
  // Load two pages and make sure both stay pinned by executing their splits.
  pm.adapt_refinement(vp);
  std::vector<int> pending = pm.pending_pages();
  TASK_CHECK(pending.size() >= 2);
  SrPageError err = SrPageError::none;
  TASK_CHECK(pm.supply_page(pending[0], store.at(pending[0]), err));
  TASK_CHECK(pm.supply_page(pending[1], store.at(pending[1]), err));
  for_int(i, 6) pm.adapt_refinement(vp);
  pm.mesh().ok();
  TASK_CHECK(pm.resident_page_count() == budget);

  // Pin every resident page through repeated refinement; then a new page cannot enter.
  int pinned_resident = 0;
  for_int(p, dir.num_pages())
    if (pm.is_resident(p) && pm.pin_count(p) > 0) ++pinned_resident;
  TASK_CHECK(pinned_resident == budget);
  pm.adapt_refinement(vp);
  std::vector<int> more = pm.pending_pages();
  TASK_CHECK(!more.empty());
  TASK_CHECK(pm.blocked_by_budget());
  const int faces_before = pm.mesh().num_active_faces();
  const size_t res_before = pm.resident_page_count();
  SrPageError e2 = SrPageError::none;
  TASK_CHECK(!pm.supply_page(more.front(), store.at(more.front()), e2));
  TASK_CHECK(e2 == SrPageError::budget_exhausted);
  TASK_CHECK(pm.resident_page_count() == int(res_before));  // Nothing evicted, frontier held.
  TASK_CHECK(pm.mesh().num_active_faces() == faces_before);

  // Raising the budget unblocks the same pending page and refinement continues from the legal frontier.
  pm.set_page_budget(budget + 1);
  TASK_CHECK(!pm.blocked_by_budget());
  TASK_CHECK(pm.supply_page(more.front(), store.at(more.front()), e2));
  TASK_CHECK(e2 == SrPageError::none);
  pm.adapt_refinement(vp);
  pm.mesh().ok();
  TASK_CHECK(pm.resident_page_count() == budget + 1);
  return 0;
}

int res_bad_page_retry() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));
  pm.adapt_refinement(vp);
  const std::vector<int> pending = pm.pending_pages();
  TASK_CHECK(!pending.empty());
  const int pid = pending.front();
  const int faces_before = pm.mesh().num_active_faces();

  // 1. Wrong page id.
  SrPageError err = SrPageError::none;
  TASK_CHECK(!pm.supply_page(pid, store.at((pid + 1) % dir.num_pages()), err));
  TASK_CHECK(err == SrPageError::bad_page_id);
  // 2a. Truncation inside the fixed header (fewer than 24 bytes).
  std::vector<uint8_t> header_truncated = store.at(pid);
  header_truncated.resize(10);
  TASK_CHECK(!pm.supply_page(pid, header_truncated, err));
  TASK_CHECK(err == SrPageError::truncated);
  // 2b. Truncation inside the body: header survives but the declared length no longer matches.
  std::vector<uint8_t> body_truncated = store.at(pid);
  body_truncated.resize(body_truncated.size() / 2);
  TASK_CHECK(!pm.supply_page(pid, body_truncated, err));
  TASK_CHECK(err == SrPageError::bad_length || err == SrPageError::truncated);
  // 3. A flipped byte corrupts the checksum or content.
  std::vector<uint8_t> corrupted = store.at(pid);
  corrupted.back() ^= 0xFF;
  TASK_CHECK(!pm.supply_page(pid, corrupted, err));
  TASK_CHECK(err == SrPageError::truncated || err == SrPageError::non_finite_geometry ||
              err == SrPageError::bad_reference || err == SrPageError::duplicate_split ||
              err == SrPageError::bad_length);
  // 4. Out-of-range id.
  TASK_CHECK(!pm.supply_page(-1, store.at(pid), err));
  TASK_CHECK(err == SrPageError::bad_page_id);

  // On every rejection: no resident change, no frontier change, request set preserved.
  TASK_CHECK(!pm.is_resident(pid));
  TASK_CHECK(pm.mesh().num_active_faces() == faces_before);
  TASK_CHECK(pm.pending_pages() == pending);
  TASK_CHECK(pm.total_resident_split_payloads() == 0);

  // Retry with the valid page: accepted and idempotent on a second supply.
  TASK_CHECK(pm.supply_page(pid, store.at(pid), err));
  TASK_CHECK(err == SrPageError::none);
  TASK_CHECK(pm.is_resident(pid));
  const size_t res = pm.resident_page_count();
  TASK_CHECK(pm.supply_page(pid, store.at(pid), err));  // Duplicate supply is idempotent.
  TASK_CHECK(pm.resident_page_count() == int(res));
  pm.mesh().ok();
  return 0;
}

// ============================================================================
// compatibility group: view switching, full convergence, PM/closed surface.
// ============================================================================

int cmp_camera_aba() {
  auto sr = load_srm();
  constexpr int k_spp = 4;
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams va = near_view();
  const SrViewParams vb = mid_view();

  // Camera B demands a strict, non-empty subset of camera A's pages.
  auto ref_a = reference_frontier(va);
  auto ref_b = reference_frontier(vb);
  const std::set<int> pages_a = executed_pages(*ref_a, dir);
  const std::set<int> pages_b = executed_pages(*ref_b, dir);
  TASK_CHECK(!pages_a.empty());
  TASK_CHECK(!pages_b.empty());
  TASK_CHECK(pages_b.size() < pages_a.size());
  for (int pid : pages_b) TASK_CHECK(pages_a.contains(pid));

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));
  // Before supplying anything: A registers a non-empty front of its page set, then switching to B must revoke every
  // request for a page that B does not need (the A-only pages).
  pm.adapt_refinement(va);
  const std::vector<int> pending_a = pm.pending_pages();
  TASK_CHECK(!pending_a.empty());
  for (int pid : pending_a) TASK_CHECK(pages_a.contains(pid));
  pm.adapt_refinement(vb);
  const std::vector<int> pending_b0 = pm.pending_pages();
  TASK_CHECK(!pending_b0.empty());
  for (int pid : pending_b0) TASK_CHECK(pages_b.contains(pid));  // A-only requests were withdrawn.
  // Camera A: converge fully.
  drive(pm, va, store, 40, true);
  pm.mesh().ok();
  TASK_CHECK(pm.pending_pages().empty());
  const MeshSig sig_a1 = signature(pm.mesh().extract_gmesh());
  TASK_CHECK(sig_a1.faces == signature(ref_a->extract_gmesh()).faces);

  // Switch to B: stale A-only requests must be withdrawn; pending only reflects what B still needs.
  pm.adapt_refinement(vb);
  std::vector<int> pending_b = pm.pending_pages();
  for (int pid : pending_b) TASK_CHECK(pages_b.contains(pid));  // No request irrelevant to view B.
  drive(pm, vb, store, 40, true);
  pm.mesh().ok();
  const MeshSig sig_b = signature(pm.mesh().extract_gmesh());
  TASK_CHECK(sig_b.faces == signature(ref_b->extract_gmesh()).faces);

  // Back to A: the frontier returns to exactly the earlier A result without replaying from the base.
  drive(pm, va, store, 40, true);
  pm.mesh().ok();
  const MeshSig sig_a2 = signature(pm.mesh().extract_gmesh());
  TASK_CHECK(sig_a2.faces == sig_a1.faces);
  TASK_CHECK(sig_a2.verts == sig_a1.verts);
  return 0;
}

int cmp_full_convergence() {
  auto sr = load_srm();
  constexpr int k_spp = 3;  // Deliberately awkward page size (not a power of two).
  SrPageDirectory dir;
  SrPageCodec::export_directory(*sr, k_spp, dir);
  TASK_CHECK(dir.splits_per_page == 3);
  const PageStore store = build_store(*sr, dir);
  const SrViewParams vp = near_view();

  SrPagedMesh pm;
  pm.initialize(dir, int(dir.num_pages()));
  drive(pm, vp, store, 60, true);
  pm.mesh().ok();
  TASK_CHECK(pm.pending_pages().empty());

  auto ref = reference_frontier(vp);
  const GMesh gp = pm.mesh().extract_gmesh();
  const GMesh gr = ref->extract_gmesh();
  gp.ok();
  gr.ok();
  // Not just face counts: connectivity, positions (and therefore materials) must correspond one-to-one.
  TASK_CHECK(gp.num_faces() == gr.num_faces());
  TASK_CHECK(gp.num_vertices() == gr.num_vertices());
  const MeshSig sp = signature(gp);
  const MeshSig sr2 = signature(gr);
  TASK_CHECK(sp.faces == sr2.faces);
  TASK_CHECK(sp.verts == sr2.verts);

  // Every executed split's page is resident and pinned; evicted slots hold no payload.
  TASK_CHECK(pm.total_resident_split_payloads() <= int(dir.num_pages()) * k_spp);
  for_int(vspli, dir.num_vsplits) {
    const int page = dir.page_of(vspli);
    if (split_executed(pm.mesh(), vspli)) {
      TASK_CHECK(pm.is_resident(page));
      TASK_CHECK(pm.pin_count(page) > 0);
    }
  }
  // Directory round-trip is deterministic and preserves every field.
  std::vector<uint8_t> dbytes = SrPageCodec::encode_directory(dir);
  SrPageDirectory dir2;
  SrPageError derr = SrPageError::none;
  TASK_CHECK(SrPageCodec::decode_directory(dbytes, dir2, derr));
  TASK_CHECK(derr == SrPageError::none);
  TASK_CHECK(SrPageCodec::encode_directory(dir2) == dbytes);
  TASK_CHECK(dir2.num_vsplits == dir.num_vsplits);
  TASK_CHECK(dir2.splits_per_page == dir.splits_per_page);
  return 0;
}

// Build a closed (no-boundary) triangulated torus base mesh directory, mirroring PMesh_test's grid construction.
void make_closed_torus_directory(SrPageDirectory& dir) {
  constexpr int ny = 4, nx = 5;
  const int nv = ny * nx, nf = 2 * ny * nx;
  dir = SrPageDirectory{};
  dir.base_nvertices = nv;
  dir.base_nfaces = nf;
  dir.num_vsplits = 0;
  dir.splits_per_page = 1;
  dir.bbox = Bbox{Point(0.f, 0.f, 0.f), Point(float(nx - 1), float(ny - 1), 0.f)};
  dir.materials.set(0, "matid=0");
  dir.base_vgeom.resize(nv);
  for_int(v, nv) {
    const int x = v % nx, y = v / nx;
    dir.base_vgeom[v].point = Point(float(x), float(y), 0.f);
    dir.base_vgeom[v].vnormal = Vector(0.f, 0.f, 1.f);  // Single normal, no UVs, like the attached PM.
  }
  const auto vid = [&](int y, int x) { return (y % ny) * nx + (x % nx); };
  std::vector<Vec3<int>> tris;
  for_int(y, ny) for_int(x, nx) {
    const int v00 = vid(y, x), v01 = vid(y, x + 1), v10 = vid(y + 1, x), v11 = vid(y + 1, x + 1);
    tris.push_back(Vec3<int>{v00, v01, v11});
    tris.push_back(Vec3<int>{v00, v11, v10});
  }
  std::map<std::pair<int, int>, std::pair<int, int>> edge_map;  // (a,b)->(face, corner opposite).
  for_int(f, nf) for_int(j, 3) edge_map.emplace(std::pair{tris[f][mod3(j + 1)], tris[f][mod3(j + 2)]},
                                                 std::pair{f, j});
  dir.base_faces.resize(nf);
  for_int(f, nf) {
    auto& bf = dir.base_faces[f];
    for_int(j, 3) bf.vi[j] = tris[f][j];
    for_int(j, 3) {
      const int a = tris[f][mod3(j + 1)], b = tris[f][mod3(j + 2)];
      const auto it = edge_map.find(std::pair{b, a});
      bf.fni[j] = it == edge_map.end() ? 0 : it->second.first + 1;
    }
    bf.matid = 0;
  }
}

int cmp_pm_and_closed() {
  // Ordinary PM traversal of the attached single-normal, no-UV progressive mesh is preserved (it has no splits).
  {
    std::ifstream pmfile(k_pm_path, std::ios::binary);
    TASK_CHECK(bool(pmfile));
    PMesh pm;
    pm.read(pmfile);
    PMeshRStream r(pm);
    PMeshIter it(r);
    const int base = it._vertices.num();
    int steps = 0;
    while (it.next()) ++steps;
    it.ok();
    const int full = it._vertices.num();
    TASK_CHECK(full == base + steps);  // Forward count is internally consistent.
    int back = 0;
    while (it.prev()) ++back;
    TASK_CHECK(back == steps && it._vertices.num() == base);  // Backward traversal restores the base.
    GMesh g = it.extract_gmesh();
    g.ok();
    TASK_CHECK(g.num_faces() == 2);
  }

  // Closed surface: a fully resident paged runtime over a boundary-free base keeps a closed manifold frontier.
  {
    SrPageDirectory dir;
    make_closed_torus_directory(dir);
    std::vector<uint8_t> bytes = SrPageCodec::encode_directory(dir);
    SrPageDirectory decoded;
    SrPageError err = SrPageError::none;
    TASK_CHECK(SrPageCodec::decode_directory(bytes, decoded, err));
    TASK_CHECK(decoded.base_nfaces == dir.base_nfaces);
    SrPagedMesh pm;
    pm.initialize(decoded, 1);
    pm.adapt_refinement(near_view());  // No splits exist; the closed base is the only legal frontier.
    pm.mesh().ok();
    const GMesh g = pm.mesh().extract_gmesh();
    g.ok();
    const EdgeAudit audit = audit_edges(g);
    TASK_CHECK(audit.manifold);
    TASK_CHECK(!audit.has_boundary);  // Closed surface: every edge has two oppositely-oriented triangles.
    TASK_CHECK(pm.pending_pages().empty());
    TASK_CHECK(pm.total_resident_split_payloads() == 0);
  }

  // Open surface: the gcanyon frontier is manifold and may legitimately contain boundary edges.
  {
    auto sr = load_srm();
    constexpr int k_spp = 4;
    SrPageDirectory dir;
    SrPageCodec::export_directory(*sr, k_spp, dir);
    const PageStore store = build_store(*sr, dir);
    SrPagedMesh pm;
    pm.initialize(dir, int(dir.num_pages()));
    drive(pm, near_view(), store, 40, true);
    const GMesh g = pm.mesh().extract_gmesh();
    const EdgeAudit audit = audit_edges(g);
    TASK_CHECK(audit.manifold);
    TASK_CHECK(audit.no_duplicate_directed);
    TASK_CHECK(audit.num_directed == 3 * g.num_faces());
  }
  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"dependency", "out_of_order_pages", dep_out_of_order},
    {"dependency", "missing_page_reentry", dep_missing_reentry},
    {"dependency", "shared_page_pin", dep_shared_pin},
    {"residency", "legal_coarsen_eviction", res_coarsen_eviction},
    {"residency", "budget_block_recover", res_budget_block_recover},
    {"residency", "bad_page_reject_retry", res_bad_page_retry},
    {"compatibility", "camera_aba", cmp_camera_aba},
    {"compatibility", "full_convergence", cmp_full_convergence},
    {"compatibility", "pm_and_closed_surface", cmp_pm_and_closed},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
