// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
//
// On-demand paged refinement example (standard library only).
//
// Usage:
//   paged_refinement <input.srm> <output.m> [splits_per_page] [page_budget]
//
// It exports a fully coarsened ordinary SrMesh into a paging directory plus encoded pages, then drives a
// SrPagedMesh view refinement one batch at a time.  The encoded page bytes are held as opaque storage (as if on
// disk): no split page is decoded into the working mesh until it is supplied, and the resident payload never
// exceeds page_budget * splits_per_page splits.  After convergence under a fixed viewpoint, the active mesh is
// written as a GMesh .m file and the request / residency / active-face statistics are printed.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "libHh/GMesh.h"
#include "libHh/SrMesh.h"
#include "libHh/SrMeshPages.h"

using namespace hh;

namespace {

SrViewParams make_view(const Bbox<float, 3>& bbox) {
  const Point center = interp(bbox[0], bbox[1], .5f);
  const Point eye(center[0], center[1], bbox[1][2] + bbox.max_side() * 2.f);
  const Vector vdir(0.f, 0.f, -1.f);
  Vector left(vdir[1], -vdir[0], 0.f);
  if (mag2(left) == 0.f) left = Vector(0.f, 1.f, 0.f);
  left.normalize();
  const Vector top = cross(vdir, left);
  SrViewParams vp;
  vp.set_frame(Frame(vdir, left, top, eye));
  vp.set_zooms(Vec2<float>(1.f, 1.f));
  vp.set_screen_thresh(1e-4f);  // A tight tolerance so several pages are actually demanded.
  return vp;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: " << (argc ? argv[0] : "paged_refinement")
              << " <input.srm> <output.m> [splits_per_page (1..64, default 4)] [page_budget (default 8)]\n";
    return 2;
  }
  const char* input_path = argv[1];
  const char* output_path = argv[2];
  const int splits_per_page = argc > 3 ? std::atoi(argv[3]) : 4;
  // Default to a budget large enough to cache every page, so a single fixed viewpoint always converges; a smaller
  // budget still works but may report blocked_by_budget until it is raised.
  const int page_budget = argc > 4 ? std::atoi(argv[4]) : -1;
  if (splits_per_page < 1 || splits_per_page > SrPageCodec::k_max_splits_per_page ||
      (argc > 4 && page_budget < 1)) {
    std::cerr << "invalid splits_per_page or page_budget\n";
    return 2;
  }

  // 1. Load the ordinary, fully coarsened SrMesh and export the directory + opaque page bytes.
  SrMesh source;
  {
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
      std::cerr << "cannot open input " << input_path << "\n";
      return 2;
    }
    source.read_srm(in);
  }
  source.ok();
  SrPageDirectory directory;
  SrPageCodec::export_directory(source, splits_per_page, directory);
  std::map<int, std::vector<uint8_t>> storage;  // Simulates remote/disk storage: bytes only, no live SrVsplit.
  for_int(pageid, directory.num_pages())
    storage.emplace(pageid, SrPageCodec::encode_page(source, directory, pageid));
  const int effective_budget = page_budget < 0 ? directory.num_pages() : page_budget;
  std::cout << "exported base_v=" << directory.base_nvertices << " base_f=" << directory.base_nfaces
            << " splits=" << directory.num_vsplits << " pages=" << directory.num_pages()
            << " splits_per_page=" << splits_per_page << " budget=" << effective_budget << "\n";

  // 2. Start the paged runtime with the directory alone: zero split pages resident.
  SrPagedMesh paged;
  paged.initialize(directory, effective_budget);
  const SrViewParams vp = make_view(directory.bbox);
  std::cout << "initial active_faces=" << paged.mesh().num_active_faces()
            << " resident_payloads=" << paged.total_resident_split_payloads() << "\n";

  // 3. Repeatedly adapt and satisfy the deduplicated, ascending pending page list in batches.
  int total_requests = 0;
  int stalled_rounds = 0;
  for_int(round, 64) {
    paged.adapt_refinement(vp);
    paged.mesh().ok();
    const std::vector<int> pending = paged.pending_pages();
    if (pending.empty()) {
      std::cout << "round " << round << ": converged active_faces=" << paged.mesh().num_active_faces()
                << " active_vertices=" << paged.mesh().num_active_vertices() << "\n";
      break;
    }
    if (paged.blocked_by_budget()) {
      // The cache only holds pinned pages and cannot admit the next pending page without exceeding the budget.
      std::cout << "round " << round << ": blocked_by_budget with " << pending.size() << " pages still demanded\n";
      break;
    }
    total_requests += int(pending.size());
    std::cout << "round " << round << ": request";
    for (int pid : pending) std::cout << ' ' << pid;
    std::cout << " (n=" << pending.size() << ")\n";
    int supplied = 0;
    for (int pid : pending) {
      SrPageError error = SrPageError::none;
      if (!paged.supply_page(pid, storage.at(pid), error)) {
        std::cerr << "supply_page " << pid << " rejected: " << SrPageCodec::error_string(error) << "\n";
        return 1;
      }
      ++supplied;
    }
    std::cout << "  supplied=" << supplied << " resident_pages=" << paged.resident_page_count()
              << " resident_payloads=" << paged.total_resident_split_payloads()
              << " active_faces=" << paged.mesh().num_active_faces() << "\n";
    stalled_rounds = supplied ? 0 : stalled_rounds + 1;
    if (stalled_rounds >= 2) break;  // No page could be delivered: avoid an unbounded loop.
  }
  paged.mesh().ok();

  // 4. Write the active frontier as a .m mesh and report final statistics.
  GMesh active = paged.mesh().extract_gmesh();
  active.ok();
  {
    std::ofstream out(output_path);
    if (!out) {
      std::cerr << "cannot open output " << output_path << "\n";
      return 2;
    }
    active.write(out);
  }
  std::cout << "total_page_requests=" << total_requests << " resident_pages=" << paged.resident_page_count()
            << " resident_payloads=" << paged.total_resident_split_payloads()
            << " active_faces=" << active.num_faces() << " active_vertices=" << active.num_vertices()
            << " wrote=" << output_path << "\n";
  return 0;
}
