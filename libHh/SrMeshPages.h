// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
#ifndef MESH_PROCESSING_LIBHH_SRMESHPAGES_H_
#define MESH_PROCESSING_LIBHH_SRMESHPAGES_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "libHh/SrMesh.h"

namespace hh {

// On-demand paging for a selectively refinable progressive mesh.
//
// The directory holds the base mesh, materials, bounding box, stable hierarchy identities and the conservative
// per-split view-admission bounds.  It may grow linearly with the mesh but never contains the per-split neighbor
// face quadruplets or subdivision geometry; those payloads live exclusively in the pages.
//
// All identifiers are stable integers (split indices and page ids); no process pointers are serialized.
// All multi-byte values use fixed big-endian ("network") byte order.
//
// The runtime keeps at most page_budget pages resident, so the split payload allocation is bounded by
// page_budget * splits_per_page.  A split that has been executed and not yet legally merged pins its page; the
// last merge on a page releases it.  Unpinned pages are evicted least-recently-used.

class SrPagedMesh;  // Defined below.

// One page's immutable split payload, decoded from the page bytes.  It uses stable integer identifiers only; the
// SrFace pointers of the working mesh are derived when the page is installed.  This is the only place that holds the
// neighbor face quadruplets and subdivision geometry.
struct SrPageRecord {
  int vspli;            // Stable split index.
  int parent_vertex;    // Stable split-vertex identity.
  Vec4<int> fni;        // 0 for an absent neighbor, else global SrFace index + 1.
  int fl_matid;
  int fr_matid;
  SrSplitBounds bounds;
  SrVertexGeometry vu_vgeom;
};

struct SrPagePayload {
  std::vector<SrPageRecord> records;  // In split-index order, exactly page_num_splits(pageid) entries.
};

// Conservative per-split bounds stored in the directory (linear in the number of splits, no geometry).
struct SrPageSplitInfo {
  int parent_vertex;          // Stable identity: the split vertex index in the full vertex hierarchy.
  SrSplitBounds bounds;       // Frustum sphere, residual errors, backface cone.
};

// Directory header: everything needed to reconstruct the coarsened SrMesh and drive view admission.
struct SrPageDirectory {
  int base_nvertices{0};
  int base_nfaces{0};
  int num_vsplits{0};
  int splits_per_page{0};  // 1..64.
  Bbox<float, 3> bbox;
  Materials materials;
  // Base mesh geometry.
  std::vector<SrVertexGeometry> base_vgeom;
  struct BaseFace {
    Vec3<int> vi;   // Base vertex indices.
    Vec3<int> fni;  // 0 for a boundary edge, else neighbor base-face index + 1.
    int matid;
  };
  std::vector<BaseFace> base_faces;
  std::vector<SrPageSplitInfo> splits;

  [[nodiscard]] int num_pages() const {
    return num_vsplits ? (num_vsplits + splits_per_page - 1) / splits_per_page : 0;
  }
  [[nodiscard]] int page_of(int vspli) const { return vspli / splits_per_page; }
  [[nodiscard]] int page_first_split(int pageid) const { return pageid * splits_per_page; }
  [[nodiscard]] int page_num_splits(int pageid) const {
    const int first = page_first_split(pageid);
    return min(splits_per_page, num_vsplits - first);
  }
};

// Why a page supply was rejected.  On rejection the resident cache, active frontier and request set are untouched.
enum class SrPageError {
  none,
  bad_magic,
  bad_version,
  truncated,
  bad_page_id,
  duplicate_split,
  bad_reference,
  non_finite_geometry,
  bad_length,
  count_mismatch,
  budget_exhausted,  // Every resident page is pinned; the budget must be raised before this page can enter.
};

// Serialization: deterministic, fixed byte order, explicit version and record counts/length bounds.
//
// Wire format (all integers and floats are fixed big-endian; float is IEEE-754 binary32; format version 1).
//
// Directory blob:
//   u32 magic == k_directory_magic ("SRPD")
//   u32 version
//   i32 base_nvertices (>0), i32 base_nfaces (>0), i32 num_vsplits (>=0), i32 splits_per_page (1..64)
//   6 f32 bbox (pmin.xyz then pmax.xyz)
//   i32 nmaterials (>=0); for each: u32 byte-length L, then L raw UTF-8 bytes (no embedded newline)
//   base_nvertices * (3 f32 point + 3 f32 normal)
//   base_nfaces * (3 i32 vertex indices, 3 i32 neighbor face id+1 or 0 for a boundary edge, i32 material id)
//   num_vsplits * (i32 parent_vertex, 4 f32 {uni,dir}_error_mag2, radius_neg, sin2alpha)
//   Every index is range-checked against the declared counts; the whole blob must be exactly consumed.
//
// Page blob (one page id; records are the contiguous original split indices
//   [pageid*splits_per_page, (pageid+1)*splits_per_page), last page truncated):
//   u32 magic == k_page_magic ("SRPP")
//   u32 version
//   u32 page_id, u32 record_count (== directory page_num_splits(page_id), 1..64)
//   u32 body_length (== record_count * 72), u32 fnv1a_32 checksum of the body
//   body: record_count records of exactly 72 bytes each:
//     i32 vspli, i32 parent_vertex (must equal the directory identity), 4 i32 neighbor face id+1 (0 = isolated),
//     i32 fl_matid, i32 fr_matid, 4 f32 admission bounds, 3 f32 refined point, 3 f32 refined normal.
//   Rejected on wrong magic/version/page id, count or length mismatch, checksum failure (truncation/corruption),
//   a repeated vspli, an out-of-range face/material/vertex reference, or any non-finite float.  A rejection never
//   evicts a resident page and never changes the active frontier, resident set, or outstanding requests.
class SrPageCodec {
 public:
  static constexpr uint32_t k_format_version = 1u;
  static constexpr int k_max_splits_per_page = 64;
  static constexpr uint32_t k_directory_magic = 0x53525044u;  // "SRPD".
  static constexpr uint32_t k_page_magic = 0x53525050u;       // "SRPP".

  // Export the directory and the encoded pages from an ordinary, fully coarsened SrMesh.
  static void export_directory(const SrMesh& sr, int splits_per_page, SrPageDirectory& dir);
  [[nodiscard]] static std::vector<uint8_t> encode_directory(const SrPageDirectory& dir);
  [[nodiscard]] static bool decode_directory(const std::vector<uint8_t>& bytes, SrPageDirectory& dir,
                                              SrPageError& error);
  // Encode exactly the splits belonging to one page (using the directory's split ordering and the source mesh).
  [[nodiscard]] static std::vector<uint8_t> encode_page(const SrMesh& sr, const SrPageDirectory& dir, int pageid);
  // Decode and fully validate a page.  On failure returns false and sets error without touching the caller state.
  [[nodiscard]] static bool decode_page(const SrPageDirectory& dir, int expected_pageid,
                                        const std::vector<uint8_t>& bytes, SrPagePayload& payload,
                                        SrPageError& error);
  [[nodiscard]] static const char* error_string(SrPageError error);
};

// The runtime paging controller: owns the directory, the resident page cache and the outstanding requests, and
// implements ISrPagingPolicy against a working SrMesh that starts at the fully coarsened base mesh.
class SrPagedMesh : public ISrPagingPolicy {
 public:
  SrPagedMesh();
  ~SrPagedMesh() override;
  SrPagedMesh(const SrPagedMesh&) = delete;
  SrPagedMesh& operator=(const SrPagedMesh&) = delete;

  // Build the runtime from a directory and a positive page budget.  The SrMesh starts fully coarsened and holds no
  // split pages.  Throws std::runtime_error on invalid arguments.
  void initialize(const SrPageDirectory& dir, int page_budget);

  // The active rendering mesh; always a legal frontier once initialize() has succeeded.
  [[nodiscard]] SrMesh& mesh() { return *_mesh; }
  [[nodiscard]] const SrMesh& mesh() const { return *_mesh; }

  // Missing page ids, deduplicated and sorted ascending.  Repeated demands do not grow the set further.
  [[nodiscard]] std::vector<int> pending_pages() const;
  // Supply a page's bytes.  Invalid bytes are rejected (see SrPageError) without evicting anything or changing the
  // frontier, requests or resident set.  Supplying an already-resident page is idempotent.
  [[nodiscard]] bool supply_page(int pageid, const std::vector<uint8_t>& bytes, SrPageError& error);
  // Increase (only) the page budget; newly affordable pages are admitted from the pending set on the next adapt.
  void set_page_budget(int page_budget);
  [[nodiscard]] int page_budget() const { return _budget; }
  [[nodiscard]] int resident_page_count() const { return int(_resident.size()); }
  // True when every demanded page is pinned and the cache cannot admit the next pending page without exceeding the
  // budget; the legal frontier is held until the budget is raised.
  [[nodiscard]] bool blocked_by_budget() const;
  // Run one view-driven adaptation pass from the current legal frontier (never replays history from the base).
  void adapt_refinement(const SrViewParams& vp);
  // Drop demands that the current view no longer needs; called automatically at the start of each adaptation.
  void prune_requests();

  // Cache residency accounting (used by tests to verify the resident-load upper bound).
  [[nodiscard]] int pin_count(int pageid) const;
  [[nodiscard]] bool is_resident(int pageid) const;
  [[nodiscard]] int total_resident_split_payloads() const;  // Number of resident SrVsplit records.
  [[nodiscard]] const SrPageDirectory& directory() const { return _dir; }

  // ISrPagingPolicy.
  [[nodiscard]] bool sr_resident(int vspli) const override;
  [[nodiscard]] SrVsplit* sr_vsplit(int vspli) override;
  [[nodiscard]] const SrVsplit* sr_vsplit(int vspli) const override;
  [[nodiscard]] SrSplitBounds sr_bounds(int vspli) const override;
  void sr_request(int vspli) override;
  void sr_touch(int vspli) override;
  void sr_on_execute(int vspli) override;
  void sr_on_merge(int vspli) override;

 private:
  struct ResidentPage {
    int pageid;
    int num_records{0};  // Number of split payloads published into the working mesh.
    uint64_t last_used{0};
  };
  std::unique_ptr<SrMesh> _mesh;
  SrPageDirectory _dir;
  int _budget{0};
  uint64_t _clock{0};
  std::vector<ResidentPage> _resident;  // Unordered; LRU selected via last_used.
  std::vector<int> _requested;          // Sorted unique page ids demanded by the current view.
  std::vector<uint8_t> _resident_flags; // num_pages entries: 1 if cached.
  std::vector<int> _pin_counts;         // num_pages entries.

  [[nodiscard]] int find_resident(int pageid) const;
  void evict_one();
  void install_page(int pageid, SrPagePayload&& payload);
  void release_page_payload(int pageid);
  [[nodiscard]] int page_of(int vspli) const { return _dir.page_of(vspli); }
};

}  // namespace hh

#endif  // MESH_PROCESSING_LIBHH_SRMESHPAGES_H_
