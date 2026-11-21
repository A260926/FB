// -*- C++ -*-  Copyright (c) Microsoft Corporation; see LICENSE.
#include "libHh/SrMeshPages.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace hh {

namespace {

// *** Deterministic big-endian byte encoding; no process pointer is ever serialized.

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}

void put_i32(std::vector<uint8_t>& b, int32_t v) { put_u32(b, uint32_t(v)); }

void put_f32(std::vector<uint8_t>& b, float v) { put_u32(b, std::bit_cast<uint32_t>(v)); }

bool get_u32(const std::vector<uint8_t>& b, size_t& off, uint32_t& v) {
  if (off + 4 > b.size()) return false;
  v = (uint32_t(b[off]) << 24) | (uint32_t(b[off + 1]) << 16) | (uint32_t(b[off + 2]) << 8) | uint32_t(b[off + 3]);
  off += 4;
  return true;
}

bool get_i32(const std::vector<uint8_t>& b, size_t& off, int32_t& v) {
  uint32_t u;
  if (!get_u32(b, off, u)) return false;
  v = int32_t(u);
  return true;
}

bool get_f32(const std::vector<uint8_t>& b, size_t& off, float& v) {
  uint32_t u;
  if (!get_u32(b, off, u)) return false;
  v = std::bit_cast<float>(u);
  return true;
}

bool finite_f32(float v) { return std::isfinite(v); }

// FNV-1a (32-bit), a deterministic non-cryptographic checksum over the page body.
uint32_t fnv1a_32(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

constexpr size_t k_page_record_bytes = 72;  // (2 + 4 + 2) i32 + (4 + 6) f32.
constexpr size_t k_page_header_bytes = 24;

}  // namespace

const char* SrPageCodec::error_string(SrPageError error) {
  switch (error) {
    case SrPageError::none: return "none";
    case SrPageError::bad_magic: return "bad magic";
    case SrPageError::bad_version: return "bad version";
    case SrPageError::truncated: return "truncated";
    case SrPageError::bad_page_id: return "bad page id";
    case SrPageError::duplicate_split: return "duplicate split record";
    case SrPageError::bad_reference: return "out-of-range reference";
    case SrPageError::non_finite_geometry: return "non-finite geometry";
    case SrPageError::bad_length: return "bad length";
    case SrPageError::count_mismatch: return "record count mismatch";
    case SrPageError::budget_exhausted: return "page budget exhausted (all pages pinned)";
  }
  return "unknown";
}

// *** Directory export from an ordinary, fully coarsened SrMesh.

void SrPageCodec::export_directory(const SrMesh& sr, int splits_per_page, SrPageDirectory& dir) {
  assertx(splits_per_page >= 1 && splits_per_page <= k_max_splits_per_page);
  assertx(sr.num_active_faces() == sr._base_faces.num());  // Must be fully coarsened.
  const int bnv = sr._base_vertices.num();
  const int bnf = sr._base_faces.num();
  const int nvspl = sr._vsplits.num();
  dir = SrPageDirectory{};
  dir.base_nvertices = bnv;
  dir.base_nfaces = bnf;
  dir.num_vsplits = nvspl;
  dir.splits_per_page = splits_per_page;
  dir.bbox = sr._bbox;
  dir.materials = sr._materials;
  dir.base_vgeom.resize(bnv);
  for_int(vi, bnv) {
    assertx(sr._vertices[vi].avertex == &sr._base_vertices[vi]);
    dir.base_vgeom[vi] = sr._vertices[vi].avertex->vgeom;
  }
  dir.base_faces.resize(bnf);
  for_int(fi, bnf) {
    const SrAFace* fa = &sr._base_faces[fi];
    SrPageDirectory::BaseFace& bf = dir.base_faces[fi];
    for_int(j, 3) bf.vi[j] = narrow_cast<int>(fa->vertices[j] - sr._base_vertices.data());
    for_int(j, 3) {
      bf.fni[j] = fa->fnei[j] != &sr._isolated_aface ? narrow_cast<int>(fa->fnei[j] - sr._base_faces.data()) + 1 : 0;
    }
    bf.matid = fa->matid;
  }
  dir.splits.resize(nvspl);
  for_int(vspli, nvspl) {
    const SrVsplit& vspl = sr._vsplits[vspli];
    dir.splits[vspli].parent_vertex = sr.sr_vsplit_parent_vertex(vspli);
    dir.splits[vspli].bounds.uni_error_mag2 = vspl.uni_error_mag2;
    dir.splits[vspli].bounds.dir_error_mag2 = vspl.dir_error_mag2;
    dir.splits[vspli].bounds.radius_neg = vspl.radius_neg;
    dir.splits[vspli].bounds.sin2alpha = vspl.sin2alpha;
  }
}

std::vector<uint8_t> SrPageCodec::encode_directory(const SrPageDirectory& dir) {
  if (!(dir.splits_per_page >= 1 && dir.splits_per_page <= k_max_splits_per_page))
    throw std::runtime_error("splits_per_page out of range");
  if (dir.base_nvertices <= 0 || dir.base_nfaces <= 0) throw std::runtime_error("empty base mesh");
  std::vector<uint8_t> b;
  put_u32(b, k_directory_magic);
  put_u32(b, k_format_version);
  put_i32(b, dir.base_nvertices);
  put_i32(b, dir.base_nfaces);
  put_i32(b, dir.num_vsplits);
  put_i32(b, dir.splits_per_page);
  for_int(i, 2) for_int(c, 3) put_f32(b, dir.bbox[i][c]);
  put_i32(b, dir.materials.num());
  for_int(mi, dir.materials.num()) {
    const string& ms = dir.materials.get(mi);
    if (ms.find('\n') != string::npos) throw std::runtime_error("material string contains newline");
    put_u32(b, uint32_t(ms.size()));
    b.insert(b.end(), ms.begin(), ms.end());
  }
  for (const SrVertexGeometry& vg : dir.base_vgeom) {
    for_int(c, 3) put_f32(b, vg.point[c]);
    for_int(c, 3) put_f32(b, vg.vnormal[c]);
  }
  for (const SrPageDirectory::BaseFace& bf : dir.base_faces) {
    for_int(j, 3) put_i32(b, bf.vi[j]);
    for_int(j, 3) put_i32(b, bf.fni[j]);
    put_i32(b, bf.matid);
  }
  for (const SrPageSplitInfo& info : dir.splits) {
    put_i32(b, info.parent_vertex);
    put_f32(b, info.bounds.uni_error_mag2);
    put_f32(b, info.bounds.dir_error_mag2);
    put_f32(b, info.bounds.radius_neg);
    put_f32(b, info.bounds.sin2alpha);
  }
  return b;
}

bool SrPageCodec::decode_directory(const std::vector<uint8_t>& bytes, SrPageDirectory& dir, SrPageError& error) {
  SrPageDirectory d;
  size_t off = 0;
  uint32_t magic, version;
  if (!get_u32(bytes, off, magic) || !get_u32(bytes, off, version))
    return error = SrPageError::truncated, false;
  if (magic != k_directory_magic) return error = SrPageError::bad_magic, false;
  if (version != k_format_version) return error = SrPageError::bad_version, false;
  int32_t bnv, bnf, nvspl, spp;
  if (!get_i32(bytes, off, bnv) || !get_i32(bytes, off, bnf) || !get_i32(bytes, off, nvspl) ||
      !get_i32(bytes, off, spp))
    return error = SrPageError::truncated, false;
  if (bnv <= 0 || bnf <= 0 || nvspl < 0 || spp < 1 || spp > k_max_splits_per_page)
    return error = SrPageError::bad_length, false;
  d.base_nvertices = bnv;
  d.base_nfaces = bnf;
  d.num_vsplits = nvspl;
  d.splits_per_page = spp;
  for_int(i, 2) for_int(c, 3) {
    if (!get_f32(bytes, off, d.bbox[i][c]) || !finite_f32(d.bbox[i][c]))
      return error = SrPageError::non_finite_geometry, false;
  }
  int32_t nmaterials;
  if (!get_i32(bytes, off, nmaterials) || nmaterials < 0) return error = SrPageError::bad_length, false;
  for_int(mi, nmaterials) {
    uint32_t len;
    if (!get_u32(bytes, off, len) || off + len > bytes.size()) return error = SrPageError::truncated, false;
    string ms(reinterpret_cast<const char*>(bytes.data() + off), len);
    off += len;
    if (ms.find('\n') != string::npos) return error = SrPageError::bad_length, false;
    d.materials.set(mi, ms);
  }
  d.base_vgeom.resize(bnv);
  for (SrVertexGeometry& vg : d.base_vgeom) {
    for_int(c, 3) {
      if (!get_f32(bytes, off, vg.point[c]) || !finite_f32(vg.point[c]))
        return error = SrPageError::non_finite_geometry, false;
    }
    for_int(c, 3) {
      if (!get_f32(bytes, off, vg.vnormal[c]) || !finite_f32(vg.vnormal[c]))
        return error = SrPageError::non_finite_geometry, false;
    }
  }
  d.base_faces.resize(bnf);
  for (SrPageDirectory::BaseFace& bf : d.base_faces) {
    for_int(j, 3) {
      if (!get_i32(bytes, off, bf.vi[j]) || bf.vi[j] < 0 || bf.vi[j] >= bnv)
        return error = SrPageError::bad_reference, false;
    }
    for_int(j, 3) {
      if (!get_i32(bytes, off, bf.fni[j]) || bf.fni[j] < 0 || bf.fni[j] > bnf)
        return error = SrPageError::bad_reference, false;
    }
    if (!get_i32(bytes, off, bf.matid) || bf.matid < 0 || bf.matid >= nmaterials)
      return error = SrPageError::bad_reference, false;
  }
  const int total_vertices = bnv + 2 * nvspl;
  d.splits.resize(nvspl);
  for (SrPageSplitInfo& info : d.splits) {
    if (!get_i32(bytes, off, info.parent_vertex) || info.parent_vertex < 0 ||
        info.parent_vertex >= total_vertices)
      return error = SrPageError::bad_reference, false;
    if (!get_f32(bytes, off, info.bounds.uni_error_mag2) ||
        !get_f32(bytes, off, info.bounds.dir_error_mag2) || !get_f32(bytes, off, info.bounds.radius_neg) ||
        !get_f32(bytes, off, info.bounds.sin2alpha))
      return error = SrPageError::truncated, false;
    for (const float f : {info.bounds.uni_error_mag2, info.bounds.dir_error_mag2, info.bounds.radius_neg,
                          info.bounds.sin2alpha})
      if (!finite_f32(f)) return error = SrPageError::non_finite_geometry, false;
  }
  if (off != bytes.size()) return error = SrPageError::bad_length, false;
  dir = std::move(d);
  return error = SrPageError::none, true;
}

// *** Page encoding: the real split payloads (neighbor quad + subdivision geometry) live only here.

std::vector<uint8_t> SrPageCodec::encode_page(const SrMesh& sr, const SrPageDirectory& dir, int pageid) {
  const int npages = dir.num_pages();
  if (pageid < 0 || pageid >= npages) throw std::runtime_error("bad page id");
  const int first = dir.page_first_split(pageid);
  const int count = dir.page_num_splits(pageid);
  std::vector<uint8_t> body;
  body.reserve(size_t(count) * k_page_record_bytes);
  for_int(k, count) {
    const int vspli = first + k;
    const SrVsplit& vspl = sr._vsplits[vspli];
    put_i32(body, vspli);
    put_i32(body, dir.splits[vspli].parent_vertex);
    for_int(j, 4) {
      const SrFace* fn = vspl.fn[j];
      put_i32(body, fn != &sr._isolated_face ? narrow_cast<int>(fn - sr._faces.data()) + 1 : 0);
    }
#if !defined(SR_PREDICT_MATID)
    put_i32(body, vspl.fl_matid);
    put_i32(body, vspl.fr_matid);
#else
    put_i32(body, 0);
    put_i32(body, 0);
#endif
    put_f32(body, vspl.uni_error_mag2);
    put_f32(body, vspl.dir_error_mag2);
    put_f32(body, vspl.radius_neg);
    put_f32(body, vspl.sin2alpha);
    for_int(c, 3) put_f32(body, vspl.vu_vgeom.point[c]);
    for_int(c, 3) put_f32(body, vspl.vu_vgeom.vnormal[c]);
  }
  std::vector<uint8_t> out;
  out.reserve(k_page_header_bytes + body.size());
  put_u32(out, k_page_magic);
  put_u32(out, k_format_version);
  put_u32(out, uint32_t(pageid));
  put_u32(out, uint32_t(count));
  put_u32(out, uint32_t(body.size()));
  put_u32(out, fnv1a_32(body.data(), body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool SrPageCodec::decode_page(const SrPageDirectory& dir, int expected_pageid, const std::vector<uint8_t>& bytes,
                              SrPagePayload& payload, SrPageError& error) {
  if (bytes.size() < k_page_header_bytes) return error = SrPageError::truncated, false;
  size_t off = 0;
  uint32_t magic, version, pageid, nrec, body_len, checksum;
  if (!get_u32(bytes, off, magic) || !get_u32(bytes, off, version) || !get_u32(bytes, off, pageid) ||
      !get_u32(bytes, off, nrec) || !get_u32(bytes, off, body_len) || !get_u32(bytes, off, checksum))
    return error = SrPageError::truncated, false;
  if (magic != k_page_magic) return error = SrPageError::bad_magic, false;
  if (version != k_format_version) return error = SrPageError::bad_version, false;
  if (int(pageid) != expected_pageid || pageid >= uint32_t(dir.num_pages()))
    return error = SrPageError::bad_page_id, false;
  if (nrec == 0 || nrec > uint32_t(k_max_splits_per_page)) return error = SrPageError::bad_length, false;
  if (nrec != uint32_t(dir.page_num_splits(expected_pageid)))
    return error = SrPageError::count_mismatch, false;
  if (body_len != nrec * k_page_record_bytes || k_page_header_bytes + body_len != bytes.size())
    return error = SrPageError::bad_length, false;
  if (fnv1a_32(bytes.data() + k_page_header_bytes, body_len) != checksum)
    return error = SrPageError::truncated, false;
  const int total_faces = dir.base_nfaces + 2 * dir.num_vsplits;
  const int total_vertices = dir.base_nvertices + 2 * dir.num_vsplits;
  const int first = dir.page_first_split(expected_pageid);
  const int last = first + int(nrec);
  SrPagePayload local;
  local.records.resize(nrec);
  std::vector<uint8_t> seen(dir.splits_per_page, 0);
  for (uint32_t k = 0; k < nrec; ++k) {
    SrPageRecord& rec = local.records[k];
    int32_t vsi;
    if (!get_i32(bytes, off, rec.vspli) || !get_i32(bytes, off, vsi))
      return error = SrPageError::truncated, false;
    if (rec.vspli < first || rec.vspli >= last) return error = SrPageError::bad_page_id, false;
    if (seen[rec.vspli - first]) return error = SrPageError::duplicate_split, false;
    seen[rec.vspli - first] = 1;
    if (vsi != dir.splits[rec.vspli].parent_vertex || vsi < 0 || vsi >= total_vertices)
      return error = SrPageError::bad_reference, false;
    rec.parent_vertex = vsi;
    int32_t fl_matid, fr_matid;
    for_int(j, 4) {
      if (!get_i32(bytes, off, rec.fni[j]) || rec.fni[j] < 0 || rec.fni[j] > total_faces)
        return error = SrPageError::bad_reference, false;
    }
    if (!get_i32(bytes, off, fl_matid) || !get_i32(bytes, off, fr_matid) ||
        fl_matid < 0 || fl_matid >= dir.materials.num() || fr_matid < 0 || fr_matid >= dir.materials.num())
      return error = SrPageError::bad_reference, false;
    rec.fl_matid = fl_matid;
    rec.fr_matid = fr_matid;
    float vals[10];
    for (float& v : vals) {
      if (!get_f32(bytes, off, v)) return error = SrPageError::truncated, false;
      if (!finite_f32(v)) return error = SrPageError::non_finite_geometry, false;
    }
    rec.bounds.uni_error_mag2 = vals[0];
    rec.bounds.dir_error_mag2 = vals[1];
    rec.bounds.radius_neg = vals[2];
    rec.bounds.sin2alpha = vals[3];
    for_int(c, 3) rec.vu_vgeom.point[c] = vals[4 + c];
    for_int(c, 3) rec.vu_vgeom.vnormal[c] = vals[7 + c];
  }
  if (off != bytes.size()) return error = SrPageError::bad_length, false;
  payload = std::move(local);
  return error = SrPageError::none, true;
}

// *** SrPagedMesh

SrPagedMesh::SrPagedMesh() = default;

SrPagedMesh::~SrPagedMesh() {
  if (_mesh) {
    // Executed splits are pinned and therefore still resident, so coarsening always has every payload it needs.
    _mesh->fully_coarsen();
    _mesh->set_paging_policy(nullptr);
  }
}

void SrPagedMesh::initialize(const SrPageDirectory& dir, int page_budget) {
  if (page_budget < 1) throw std::runtime_error("page budget must be positive");
  if (dir.base_nvertices <= 0 || dir.splits_per_page < 1 ||
      dir.splits_per_page > SrPageCodec::k_max_splits_per_page)
    throw std::runtime_error("invalid directory");
  _dir = dir;
  _budget = page_budget;
  _clock = 0;
  const int npages = _dir.num_pages();
  _resident.clear();
  _requested.clear();
  _resident_flags.assign(npages, 0);
  _pin_counts.assign(npages, 0);
  _mesh = std::make_unique<SrMesh>();
  _mesh->sr_initialize_paged(_dir);
  // The on-demand path runs without either geomorph; the ordinary all-resident SrMesh keeps its morph behavior.
  _mesh->set_refine_morph_time(0);
  _mesh->set_coarsen_morph_time(0);
  _mesh->set_paging_policy(this);
}

int SrPagedMesh::find_resident(int pageid) const {
  for (size_t i = 0; i < _resident.size(); ++i)
    if (_resident[i].pageid == pageid) return int(i);
  return -1;
}

bool SrPagedMesh::is_resident(int pageid) const {
  return pageid >= 0 && pageid < int(_resident_flags.size()) && _resident_flags[pageid] != 0;
}

int SrPagedMesh::pin_count(int pageid) const {
  assertx(pageid >= 0 && pageid < int(_pin_counts.size()));
  return _pin_counts[pageid];
}

int SrPagedMesh::total_resident_split_payloads() const {
  int n = 0;
  for (const ResidentPage& rp : _resident) n += rp.num_records;
  return n;
}

bool SrPagedMesh::sr_resident(int vspli) const { return is_resident(page_of(vspli)); }

SrVsplit* SrPagedMesh::sr_vsplit(int vspli) {
  const int pageid = page_of(vspli);
  const int idx = find_resident(pageid);
  assertx(idx >= 0);  // Callers only touch a split whose page is resident.
  return &_mesh->_vsplits[vspli];
}

const SrVsplit* SrPagedMesh::sr_vsplit(int vspli) const {
  const int pageid = page_of(vspli);
  const int idx = find_resident(pageid);
  assertx(idx >= 0);
  return &_mesh->_vsplits[vspli];
}

SrSplitBounds SrPagedMesh::sr_bounds(int vspli) const { return _dir.splits[vspli].bounds; }

void SrPagedMesh::sr_request(int vspli) {
  const int pageid = page_of(vspli);
  if (is_resident(pageid)) return;
  auto it = std::lower_bound(_requested.begin(), _requested.end(), pageid);
  if (it == _requested.end() || *it != pageid) _requested.insert(it, pageid);  // Sorted and deduplicated.
}

void SrPagedMesh::sr_touch(int vspli) {
  const int idx = find_resident(page_of(vspli));
  if (idx >= 0) _resident[idx].last_used = ++_clock;
}

void SrPagedMesh::sr_on_execute(int vspli) {
  const int pageid = page_of(vspli);
  assertx(is_resident(pageid));
  ++_pin_counts[pageid];
  const int idx = find_resident(pageid);
  _resident[idx].last_used = ++_clock;
}

void SrPagedMesh::sr_on_merge(int vspli) {
  const int pageid = page_of(vspli);
  assertx(_pin_counts[pageid] > 0);
  --_pin_counts[pageid];
  const int idx = find_resident(pageid);
  if (idx >= 0) _resident[idx].last_used = ++_clock;
}

std::vector<int> SrPagedMesh::pending_pages() const {
  std::vector<int> out;
  for (const int pageid : _requested)
    if (!is_resident(pageid)) out.push_back(pageid);
  return out;  // Already sorted and unique.
}

void SrPagedMesh::release_page_payload(int pageid) {
  const int idx = find_resident(pageid);
  assertx(idx >= 0);
  assertx(_pin_counts[pageid] == 0);  // Active-split payloads can never be evicted.
  const int first = _dir.page_first_split(pageid);
  const int count = _dir.page_num_splits(pageid);
  // Reclaim the working-mesh slots so no full _vsplits copy of the payload is retained anywhere.
  for_int(k, count) {
    SrVsplit empty;
    for_int(j, 4) empty.fn[j] = &_mesh->_isolated_face;
    _mesh->_vsplits[first + k] = empty;
  }
  _resident.erase(_resident.begin() + idx);
  _resident_flags[pageid] = 0;
}

void SrPagedMesh::evict_one() {
  int victim = -1;
  uint64_t oldest = 0;
  for (size_t i = 0; i < _resident.size(); ++i) {  // Least-recently-used among unpinned pages only.
    const ResidentPage& rp = _resident[i];
    if (_pin_counts[rp.pageid] != 0) continue;
    if (victim < 0 || rp.last_used < oldest) {
      victim = int(i);
      oldest = rp.last_used;
    }
  }
  if (victim >= 0) release_page_payload(_resident[victim].pageid);
}

void SrPagedMesh::install_page(int pageid, SrPagePayload&& payload) {
  assertx(!is_resident(pageid));
  const int first = _dir.page_first_split(pageid);
  // Resolve the stable integer face identifiers to the working mesh's SrFace pointers and publish each payload
  // straight into its working-mesh slot; no second copy of the records is kept.
  for (size_t k = 0; k < payload.records.size(); ++k) {
    const SrPageRecord& rec = payload.records[k];
    const int vspli = first + int(k);
    assertx(rec.vspli == vspli);
    SrVsplit vspl;
    for_int(j, 4) vspl.fn[j] = rec.fni[j] ? &_mesh->_faces[rec.fni[j] - 1] : &_mesh->_isolated_face;
#if !defined(SR_PREDICT_MATID)
    vspl.fl_matid = short(rec.fl_matid);
    vspl.fr_matid = short(rec.fr_matid);
#endif
    vspl.uni_error_mag2 = rec.bounds.uni_error_mag2;
    vspl.dir_error_mag2 = rec.bounds.dir_error_mag2;
    vspl.radius_neg = rec.bounds.radius_neg;
    vspl.sin2alpha = rec.bounds.sin2alpha;
    vspl.vu_vgeom = rec.vu_vgeom;
    _mesh->_vsplits[vspli] = vspl;
  }
  ResidentPage rp;
  rp.pageid = pageid;
  rp.num_records = int(payload.records.size());
  rp.last_used = ++_clock;
  _resident.push_back(rp);
  _resident_flags[pageid] = 1;
  payload.records.clear();  // The working mesh now owns the only copy of the page's payloads.
  auto it = std::lower_bound(_requested.begin(), _requested.end(), pageid);  // The request is now satisfied.
  if (it != _requested.end() && *it == pageid) _requested.erase(it);
}

bool SrPagedMesh::supply_page(int pageid, const std::vector<uint8_t>& bytes, SrPageError& error) {
  if (pageid < 0 || pageid >= _dir.num_pages()) return error = SrPageError::bad_page_id, false;
  SrPagePayload payload;
  // Decode fully before touching resident state, so a failure cannot evict a page or move the frontier/requests.
  if (!SrPageCodec::decode_page(_dir, pageid, bytes, payload, error)) return false;
  if (is_resident(pageid)) return error = SrPageError::none, true;  // Idempotent re-supply.
  if (int(_resident.size()) >= _budget) {
    evict_one();  // Make room via LRU; a pinned page is never selected.
    if (int(_resident.size()) >= _budget) {
      // Every resident page is pinned.  Hold the legal frontier and report the budget block.
      return error = SrPageError::budget_exhausted, false;
    }
  }
  install_page(pageid, std::move(payload));
  return error = SrPageError::none, true;
}

void SrPagedMesh::set_page_budget(int page_budget) {
  if (page_budget < _budget) throw std::runtime_error("page budget may only increase");
  _budget = page_budget;
}

bool SrPagedMesh::blocked_by_budget() const {
  if (int(_resident.size()) < _budget) return false;
  if (pending_pages().empty()) return false;
  for (const ResidentPage& rp : _resident)
    if (_pin_counts[rp.pageid] == 0) return false;  // An LRU victim could make room on the next supply.
  return true;
}

void SrPagedMesh::prune_requests() {
  // Requests are view-scoped: clear them so the traversal registers exactly what this view still demands.
  _requested.clear();
}

void SrPagedMesh::adapt_refinement(const SrViewParams& vp) {
  prune_requests();
  _mesh->set_view_params(vp);
  _mesh->adapt_refinement();
}

}  // namespace hh
